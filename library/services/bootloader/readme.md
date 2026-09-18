# services.bootloader —— BL 外壳（V1.0）

> Bootloader 的全部决策逻辑：**读状态 → 生效(SWITCH/MOVE) → 试运行/回滚 → 校验 → 跳**。
> 共用的分区表 / 状态区 / 介质抽象都来自 `services.ota_core`，本模块只做「BL 该怎么做」。

## 职责边界（这条边界是整个设计的地基）

| | BL 能判 | BL 不能判 |
|---|---|---|
| 分区表是否自洽 | ✅ | |
| 目标槽向量表是否合法（SP/PC） | ✅ | |
| 刚下载完的镜像与包头 CRC 是否一致 | ✅（生效前那一次） | |
| **业务是否正常** | | ❌ 只有 APP 自己知道 |

所以「这次升级成功了吗」的判据是：**APP 自检通过后调 `ota_confirm()`**。
在那之前 BL 把新槽当「试运行」，每上电一次计数 +1，超过 `max_try` 就撤回旧槽。

## 状态机

```
IDLE → CFG → ACTIVATE → HEALTH → VERIFY → READY
                                             └→ boot_jump()   （不返回）
                          （任一环节判死）──→ RESCUE
```

| 阶段 | 做什么 |
|---|---|
| `CFG` | 读状态区两份取 seq 大者；**两份都坏 → 出厂默认且不落盘** |
| `ACTIVATE` | `pending_action != NONE` 时执行 SWITCH / MOVE（非阻塞步进） |
| `HEALTH` | 试运行计数 / 回滚判定（`boot_health_apply`） |
| `VERIFY` | 校验目标槽向量表；不可跳则试另一个槽；都不行 → RESCUE |
| `READY` | 交给 `boot_jump()` |

## 试运行与回滚

`ota_cfg_t.trial_slot` 是唯一的判据字段：

| 状态 | 动作 | 代价 |
|---|---|---|
| `trial_slot != NONE` 且 `boot_try < max_try` | `boot_try++` 落盘，跳 `trial_slot` | 一次状态区擦写 |
| `trial_slot != NONE` 且 `boot_try >= max_try` | 清 `trial_slot`，跳 `active_slot` | 一次状态区擦写 |
| `trial_slot == NONE`（已确认） | 直接跳 `active_slot` | **零擦写** |

**为什么 `boot_try` 必须落盘**：不落盘的话每次复位都从 0 开始，新固件永远达不到上限，
回滚机制等于不存在。

**为什么回滚是瞬时的**：旧槽内容从头到尾没被动过 —— 新镜像写在**另一个**槽
（A/B）或**暂存区**（MOVE），回滚只是把目标槽号换个值，不涉及任何搬运。

**为什么正常开机零擦写**：已确认状态下 `boot_health_apply` 返回 0（未改动），
`boot_jump` 只在 `run_slot` 变化时写。频繁复位的设备不会把 CFG 扇区写死 ——
16 KB 扇区的擦写寿命约 1 万次，而正常开机一天也就几次。

> `ota_confirm()` 属于 APP 侧（`services.ota`，P3）：把 `trial_slot` 清空、
> `active_slot` 固化为当前槽、`boot_try` 归零。

## SWITCH 与 MOVE

两者由 `ota_cfg_t.pending_action` 分流，**共用同一段代码**：

| | SWITCH（内部 A/B） | MOVE（单槽 + 暂存） |
|---|---|---|
| 前置 | APP 已写进另一个 RUN 槽 | APP 已写进 STAGE 区（内部或外挂） |
| BL 做的 | 改 `active_slot`（4 个字节） | 擦 RUN 区 → 逐块搬 → 读回校验 |
| 耗时 | 毫秒 | 几百 KB / 秒级（分步，可喂狗） |
| 分区表特征 | RUN 有 2 条 | RUN 只有 1 条 + 有 STAGE |

## MOVE 的断电安全

关键前提：**暂存区在搬完之前是只读源**，全程只写运行区、不做原地交换。

于是「按擦除单位搬 + 先写数据后写进度 + 重启后从进度所在单位起点重搬」
**天然幂等** —— 源没被破坏，重搬多少次结果都一样。掉电的代价只是多搬一次，
而不是数据不一致。

配套两条：

- **擦除阶段不记断点**：擦除本身幂等（重擦若干扇区结果仍是全 0xFF），每次从头擦。
- **进度按擦除单位落盘**，不是每块都写。16 KB 单位下 448 KB 只需写二十几次；
  若按 4 KB 块写，光状态区擦写（每次擦 16 KB 扇区 ~250 ms）就会比搬运本身还慢。

明确**否决**了「运行区 ↔ 暂存区就地交换」那种省空间的花招：它的幂等性要靠复杂
协议才成立，而省下的是便宜的外挂容量，不值。

## 错误处理

| 出错点 | 动作 |
|---|---|
| `pending_action` 指向的区找不到 / 镜像超容量 | 清 `pending_action` 落盘，回落 `active_slot` |
| 生效过程介质读写失败 | 同上（避免每次上电都在坏镜像上重试） |
| 生效前 CRC 校验不过 | 同上 |
| 状态区写失败 | 上报 `BOOT_EV_CFG_FAIL`；**不跑没确认过的新槽**，退回 `active_slot` |
| 目标槽向量表非法 | 试另一个槽；另一个也不行 → `RESCUE` |

「跳过去」优先于「保守不跳」：`boot_jump` 里 `run_slot` 写失败也照跳 ——
跳过去至少可能活，不跳必砖。

## 安全闸（BL 的最后一道保险）

`boot_init()` 会把 `chip.oop_flash` 的安全闸收窄成
**`[BOOT 区末尾, 内部 Flash 末尾)`**。

于是即便后面分区表算错、或 MOVE 的地址算错，`oop_flash_erase()` 也会在展开到完整
扇区后复核窗口，把擦到 BL 自己的操作拒掉。分区表里**必须登记 BOOT 区**
（`OTA_AREA_ROLE_BOOT`），否则 `boot_init` 直接返回 `OTA_ERR_NO_AREA` —— 宁可不启动，
也不要一个没有自我保护的 BL。

## 用法

```c
static ota_flash_t      s_flash[1];
static const ota_area_t s_areas[] = { /* User/ota_areas.c */ };
static uint8_t          s_buf[4096];
static boot_ctx_t       s_boot;          /* 勿放栈上：含 ota_cfg_t + 完整上下文 */

int main(void)
{
    HAL_Init(); SystemClock_Config(); MX_GPIO_Init(); /* ... */

    ota_env_t env = { s_areas, N, s_flash, 1u };
    (void)ota_flash_int_get(&s_flash[0]);
    if (ota_init(&env) != OTA_OK) { rescue(); }

    boot_cfg_t bc = { s_buf, sizeof(s_buf), 3u, on_boot_event, NULL };
    if (boot_init(&bc) != OTA_OK) { rescue(); }

    boot_start(&s_boot);
    for (;;) {
        boot_step_ret_t r = boot_step(&s_boot);
        if (r == BOOT_STEP_BUSY)  { continue; }
        if (r == BOOT_STEP_READY) { boot_jump(&s_boot); }   /* 成功不返回 */
        rescue();                                           /* 没得跳 */
    }
}
```

## 事件表（`report` 回调）

| 事件 | arg | 用途 |
|---|---|---|
| `BOOT_EV_CFG_FACTORY` | – | 状态区两份都坏，按出厂态处理 |
| `BOOT_EV_CFG_FAIL` | – | 状态区写失败（介质故障） |
| `BOOT_EV_ACTIVATE_SWITCH` | 目标槽 | 即将改槽号 |
| `BOOT_EV_ACTIVATE_MOVE` | 待搬字节 | 即将开始搬运 |
| `BOOT_EV_MOVE_PROGRESS` | 已处理字节 | 喂狗 / 刷进度（擦除、搬运、回读共用） |
| `BOOT_EV_ACT_FAIL` | 取正的错误码 | 生效失败 |
| `BOOT_EV_SLOT_BAD` | 槽号 | 向量表不合法 |
| `BOOT_EV_TRIAL` | 已尝试次数 | 第 N 次试运行 |
| `BOOT_EV_ROLLBACK` | 回滚到的槽 | 回滚发生 |
| `BOOT_EV_RESCUE` | – | 无可用镜像 |
| `BOOT_EV_JUMP` | 槽号 | **最后一次能打日志的机会**（跳转后 UART 就没了） |

## 文件

| 文件 | 职责 |
|---|---|
| `bootloader.c/h` | 状态机、向量表校验、跳转编排、安全闸 |
| `boot_activate.c/h` | SWITCH / MOVE 执行（含断点续做、读回校验、状态区提交） |
| `boot_health.c/h` | 试运行计数与回滚判定（**纯逻辑，无 HAL/chip 依赖，可 PC 单测**） |

## 依赖

`services.ota_core` + `chip.oop_boot` + `chip.oop_flash`。

拉取组合：

```
BL 工程(1MB A/B)   : services.ota_core + services.bootloader
BL 工程(512KB MOVE): services.ota_core + services.bootloader
                     + services.ota_flash_ext + devices.spi_nor_flash
```
