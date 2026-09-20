# services.ota_core — OTA 共用底座

## 定位

BL 与 APP 的**共同底座**：分区表、状态区、镜像包格式、介质抽象、校验、流程骨架。
它不认识传输通道（那属于 `services.ota_src_*`），也不认识「谁在跑它」（那属于
`services.bootloader` / `services.ota` 两个外壳）。

- 依赖：`chip.oop_flash`、`chip.platform`
- 板无关：不出现任何 HAL 符号；地址来自工程侧分区表
- 不动态分配、不打印日志；搬运缓冲由调用方提供

### 为什么合并成一个底座，而不是「BL 一套 + APP 一套」

因为 「A/B 切换」与「单槽 + 暂存搬运」不是两套代码：

| 差异 | 吸收方式 |
|---|---|
| 运行槽有 1 个还是 2 个 | **分区表**里 RUN 区条目有几条 |
| 新镜像怎么生效 | 状态区一个字段 `pending_action ∈ {SWITCH, MOVE}`，由 BL 执行 |

于是 APP 侧两个拓扑完全共用（都是「写某个区 → 双层校验 → 提交」），BL 侧只在
`boot_activate` 里分两个分支。这也是「兼容 + 可选」的落地点。

## 文件构成

| 文件 | 职责 |
|---|---|
| `ota_common.h` | 返回码、介质类型、槽常量、`ota_act_t` |
| `ota_crc32.h/.c` | CRC-32/ISO-HDLC（= `zlib.crc32`）。查表版 + 位运算版（BL 省 1 KB 时用） |
| `ota_flash.h/.c` | 介质抽象 `ota_flash_t` + **内部 Flash 实现**（坐 `oop_flash_*`） |
| `ota_area.h/.c` | 分区表 schema、注册/查询、**目标区选择**（RUN 数 → SWITCH/MOVE） |
| `ota_cfg.h/.c` | 状态记录 `ota_cfg_t` + 双份乒乓读写 |
| `ota_image.h/.c` | `.otapkg` 80 字节包头解析、段查找、校验器接口 |
| `ota_source.h` | 取数后端接口（纯头，无实现） |
| `ota_flow.h/.c` | **流程骨架**（非阻塞步进式） |
| `ota_core.h/.c` | 模块入口 `ota_init(env)` |

## 用法

### 1. 工程侧：写分区表（板级事实，不进 SDK）

```c
/* User/ota_areas.c —— base 是「介质内偏移」，内部 Flash 的偏移 0 == 0x08000000 */
static const ota_area_t s_areas[] = {
    { "boot",  OTA_AREA_ROLE_BOOT,  OTA_SLOT_NONE, 0u, 0x00000u,  32u * 1024u },
    { "cfg",   OTA_AREA_ROLE_CFG,   OTA_SLOT_NONE, 0u, 0x08000u,  32u * 1024u },
    { "slotA", OTA_AREA_ROLE_RUN,   OTA_SLOT_A,    0u, 0x10000u, 448u * 1024u },
    { "slotB", OTA_AREA_ROLE_RUN,   OTA_SLOT_B,    0u, 0x80000u, 512u * 1024u },
};
static ota_flash_t s_flash[1];

void task_ota_init(void)
{
    ota_env_t env = { s_areas, 4u, s_flash, 1u };
    ota_flash_int_get(&s_flash[0]);
    if (ota_init(&env) != OTA_OK) {          /* 表校验 + CFG 区可对半分校验 */
        ERR_LOG("ota_init failed");
    }
}
```

单槽 + 外挂暂存的拓扑只改这张表：

```c
    { "boot",  OTA_AREA_ROLE_BOOT,  OTA_SLOT_NONE, 0u, 0x00000u,  32u * 1024u },
    { "cfg",   OTA_AREA_ROLE_CFG,   OTA_SLOT_NONE, 0u, 0x08000u,  32u * 1024u },
    { "run",   OTA_AREA_ROLE_RUN,   OTA_SLOT_A,    0u, 0x10000u, 448u * 1024u },
    { "stage", OTA_AREA_ROLE_STAGE, OTA_SLOT_NONE, 1u, 0x00000u, 448u * 1024u },
```

### 2. 跑一次升级（非阻塞步进）

```c
static ota_flow_t   s_flow;
static uint8_t      s_buf[4096];
static ota_source_t s_src;        /* 来自 services.ota_src_uart / _http / _mem */

void task_ota_start(void)
{
    ota_flow_cfg_t cfg = {
        .source       = &s_src,
        .buf          = s_buf,
        .buf_len      = sizeof(s_buf),
        .running_slot = ota_cfg_run_slot(&s_cfg),   /* 我在哪个槽 */
        .report       = my_report,                  /* 喂狗 / 进度 / 上报都在这里 */
        .user         = NULL,
    };
    ota_flow_start(&s_flow, &cfg);
}

void task_ota_poll(void)                    /* 每次主循环调一次即可 */
{
    ota_flow_ret_t r = ota_flow_step(&s_flow);
    if (r == OTA_FLOW_OK) {
        SYS_LOG("ota ready: act=%d, ver 已提交，准备复位", (int)s_flow.act);
        /* 此处由 APP 外壳决定何时 NVIC_SystemReset() */
    } else if (r == OTA_FLOW_ERR) {
        ERR_LOG("ota failed: state=%s err=%d",
                ota_flow_state_name(s_flow.state), (int)s_flow.err);
    }
    heart_beat_run();                       /* 每次 step 之后喂狗 */
}
```

## 流程状态机

```
IDLE → OPEN → HDR → DECIDE → ERASE → WRITE → VERIFY_MEM → VERIFY_FLASH → COMMIT → DONE
                                            ▲                ▲
                                      第 2 关（内存 CRC）  第 3 关（读回 Flash 重算 CRC）
```

- **每个 step 只做一小块**：读一块（`buf_len`）、擦一个擦除单位、校验一块。
  448 KB 的长擦除因此被切成多步，步与步之间应用能喂狗、刷进度、响应取消。
- 为什么要这样而不是异步状态机：单次操作本身并不长，问题是「一次调用别阻塞太久」。
  把粒度做小 + 进度回调，既满足喂狗，又不用为一块服务写整套状态机。
- `report` 回调在每个 step 之后调用 —— 喂狗、算进度（`ota_flow_progress()`）、
  上报 MQTT 都放这里。想中止就不要再调 step，或直接 `ota_flow_abort()`。

### 长操作必须排在「灌数据之前」（否则流式源必丢字节）

扇区擦除是**秒级**操作（F4 128 KB 扇区约 1 s），而且**它会阻塞 CPU，期间读不了 UART**。
流式接收方（串口）的收发缓冲通常只有一个帧的余量（本项目 UART 驱动 1088 B ≈ 1 个
1029 B 的 YMODEM 帧）。所以「头包 ACK 已发出、主机已开始灌数据，然后才在 WRITE 之前
擦目标区」是**错的**：擦除期间主机发的包连同它的超时重发会把缓冲填满，多出来的字节被丢掉，
恢复读取时字节流从半帧中间开始 —— 而帧解析器只在帧首认 SOH/STX，于是它会落在数据里的
**假帧头**上（1024 B 的包里出现 0x01/0x02 的概率 98%+），然后**一直 NAK，永不恢复**。
现象：`erase 0%` 之后 `write` 卡在 0%，主机第 3 包起永久 NAK，最终报
`OTA_ERR_SOURCE(-9)`（注意**不是**介质错 —— 别被残留的 `FLASH_SR` 带偏）。

正确做法：**在主机还没开始灌数据的那一刻把目标区先擦掉**，即

```c
ota_flow_pre_erase(running_slot, image_size, &info);   /* 此刻手里已有 image_size */
```

主机在等你的握手字节（YMODEM 的 `'C'`）时不会发数据，这段时间擦除是免费的，
代价只是握手慢 1~2 s。`do_decide` 会核对这里登记的区间并把 `erase_cur` 直接推到
`erase_end`，于是 ERASE 阶段零耗时、WRITE 期间只剩毫秒级的页写入。

没调、或流程最终选了别的区间时，ERASE 阶段照常自己擦 —— 功能不受影响，只是长操作落在流内。

### 5 道校验关（写在这一个模块里，别处不要再各做一套）

| # | 时机 | 动作 | 能拦住什么 |
|---|---|---|---|
| 1 | 逐包 | YMODEM 的 CRC16 / HTTP 的 TCP | 传输误码 |
| 2 | 收完（内存） | 增量 CRC32 + 长度 = `VERIFY_MEM` | 收到的东西错、包被截断 |
| 3 | 收完（落盘） | 读回 Flash 重算 CRC32 = `VERIFY_FLASH` | **写 Flash 失败 / 漏写 / 地址算错** |
| 4 | 提交 | CFG 双份乒乓 + 整体 CRC = `COMMIT` | 掉电丢状态 |
| 5 | BL 跳转前 | BL 再复算一次（在 `services.bootloader`） | 复位前这一区间的扰动 |

第 2 与第 3 关的区别是最容易漏的设计点：增量 CRC 只证明「收到的报文对」，
读回 CRC 才证明「落到 Flash 的字节对」。**两关都要，别省。**

第 2/3 关「跟谁比」取决于源给没给期望 CRC：

- `.otapkg` → 包头段表里的 CRC32（打包时写入）；
- 裸 bin → 源在 `open` 时填的 `info.expect_crc32`（如 UART 的「元数据优先」小文件）；
  没有（`== 0`）就退化为「第 2 关内存 CRC == 第 3 关闪存 CRC」自校 —— 只证明
  「收到的 == 落盘的」，**发现不了 PC 端源文件本身损坏**。要提前校验就得给期望 CRC。

## 关键设计决策

| 决策 | 理由 |
|---|---|
| 搬运缓冲由调用方提供 | 本模块不动态分配、不占大栈；RAM 预算归应用控制 |
| `ota_flow_t` 别放栈上 | 它含包头副本、段描述、两个校验器状态，约 200 字节 |
| 介质偏移而非 CPU 地址 | 外挂 SPI NOR 不在 CPU 地址空间；要 CPU 地址走 `ota_area_cpu_addr()` |
| 段表按 `load_addr` 匹配 | `SWITCH` 用目标槽地址、`MOVE` 用运行区地址 —— MOVE 拓扑下镜像仍是按**运行槽地址**链接的（这正是 MOVE 能保留单一链接地址的原因） |
| `seek` 可选 | 能 seek 就跳到目标段（HTTP 用 Range 省一半流量）；不能 seek 就顺序丢弃（串口建议改用单段包） |
| CFG 区要求「半区是擦除单位整数倍」 | 否则擦第二份会连带擦掉第一份。`ota_cfg_init()` 直接判为配置错误，而不是等写入时才毁数据 |
| `active_slot` 只归 BL 改 | APP 只通过 BL 的 `confirm` 间接影响它，避免两边对「谁在运行」理解不一致 |

## `ota_cfg_t` 字段速查

| 字段 | 含义 |
|---|---|
| `active_slot` | 已确认可运行的槽（**只有 BL 写**） |
| `trial_slot` | 待试运行槽；`OTA_SLOT_NONE` = 无 |
| `boot_try` | 试运行已尝试次数；BL 跳转前自增并落盘，超上限即回滚 |
| `run_slot` | 本次跳转的槽；**BL 每次跳转前写**，APP 早期读它设 `SCB->VTOR` |
| `pending_action` | `NONE` / `SWITCH` / `MOVE` —— 待生效动作 |
| `target_slot` | SWITCH：目标槽号 |
| `stage_area` / `target_area` | MOVE：暂存区 / 运行区在表里的下标 |
| `image_size` / `image_crc32` | 待生效镜像的长度与 CRC（BL 二次校验用） |
| `move_progress` | MOVE：已搬字节数（断电续做的断点） |
| `factory_flag` | 出厂态标记，防止「空 Flash 上电即回滚」 |

## 坑位

| 现象 | 原因 / 处理 |
|---|---|
| `ota_init` 返回 `OTA_ERR_NO_AREA` | 表里没有 `OTA_AREA_ROLE_CFG`；或区越出介质容量；或同一介质内两区重叠；或 RUN 槽号重复；或 CFG 区太小 / 半区不是擦除单位整数倍 |
| `ota_flow` 卡在 `DECIDE` 返回 `OTA_ERR_SEG` | 包里没有本机要的那一段。段表按 `load_addr` 匹配 —— 检查打包时的 `--load-a/--load-b` 是否与该槽 CPU 地址一致 |
| `OTA_ERR_NOSPACE` | 镜像段大于目标区。双段包里另一段更大没关系，只有本机那一段受检 |
| `OTA_ERR_UNSUPPORTED`（`DECIDE`） | 段不在包内首位、而通道又不可 seek（流式无法回退）→ 改用单段包 |
| `VERIFY_FLASH` 失败但 `VERIFY_MEM` 过 | 大概率是介质写入有问题（外挂 Flash 的 WEL/WP、内部 Flash 未擦干净）；也可能是擦写后又被别处覆盖 |
| 掉电后 `boot_try` 少算了 | `COMMIT` 的 CFG 写入是双份乒乓，最坏丢一次最近写入；`boot_try` 的累加在 BL 侧（见 `services.bootloader`） |
| 提交后立刻复位但状态没生效 | `COMMIT` 里 CFG 写入有回读确认；若返回 `OTA_ERR_MEDIA` 说明写失败，别复位 |

## 未实现（需要时先补这里，别在工程侧另开一份）

- 镜像签名 / 加密（`ota_pkg_hdr_t.flags` 与 `reserved[7]` 已留位；校验器接口是现成的插入点）
- 断点续传（`source->seek` + 段内偏移续写；当前只做「段内整段重写」）
- 差分升级
- 多 STAGE 段的轮换策略（当前固定用第一个 STAGE）
