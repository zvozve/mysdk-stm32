# services.ota —— OTA 的 APP 外壳（V1.0）

> APP 侧的升级外壳：早期 VTOR / 启动升级 / 非阻塞步进 / 自检确认。
> 只是 `ota_flow` 外的一层壳 —— 把「我在哪个槽」「下载完怎么重启」「自检通过怎么确认」
> 这三件只有 APP 知道的事补齐。

## 与 BL 的分工

| | `services.bootloader` | `services.ota`（本模块） |
|---|---|---|
| 跑在哪 | BL 工程 | APP 工程 |
| 干什么 | 决定跳到哪个槽、凭什么跳；执行 SWITCH / MOVE；试运行计数与回滚 | 下载新固件、双重校验、提交状态区、告知 BL「这个版本可用」 |
| 共用 | `services.ota_core`（分区表 / 状态区 / 包格式 / 介质抽象 / 流程骨架） | 同 |

**回滚逻辑全在 BL**。APP 不需要知道「我跑了几次」「什么时候会被撤回」——
它只要能自检、能确认就够了。

## 一次升级的完整生命周期

```
① BL   把 pending_action 写进状态区 → 跳到目标槽 → 该槽进入「试运行」
② APP  ota_app_early_init() 设 VTOR（main 第一行）
③ APP  自检通过 → ota_app_confirm() 固化；没通过就让它复位，
       BL 累计 boot_try，超限自动回滚
④ 下次升级：ota_app_start() + 反复 ota_app_process()
       → 返回 OTA_APP_DONE → ota_app_reboot() 交给 BL 生效
```

## 一个刻意的设计决定：VTOR 用**编译期基址**

状态区里有 `run_slot`（BL 每次跳转前写入，见 `doc/01`），但本模块**不用它设 VTOR**，
而是要求工程用 `-DOTA_SELF_BASE=0x08010000` 把链接基址传进来。

理由：

- 「我在哪个槽」是**链接期就确定的事实**，不需要从别处问。
- 状态区**可能写失败**。BL 若在 `ota_cfg_save()` 之后才发现写失败，`run_slot` 就是过期的；
  拿它去设 VTOR，中断会映到**另一个槽的向量表**上。
- 而两个槽是**同一份源码的两次链接** —— 向量表内容几乎一样（除了硬编码地址）。
  于是这个错误**不会崩**，只会安静地跑错 handler。这正是最该避免的一类故障。

`run_slot` 仍有它的用处（诊断、以及 BL 自己的判断），只是不该拿它决定 APP 的 VTOR。

## API

| 函数 | 说明 |
|---|---|
| `ota_app_early_init(a, self_base)` | **main 第一行**。只做两件事：设 VTOR、开中断 —— 不碰外设、不碰时钟、不读 Flash，所以能在 `HAL_Init()` 之前跑 |
| `ota_app_self_slot(a)` | 由基址反查槽号（需先 `ota_init()`） |
| `ota_app_start(a, src, buf, buf_len)` | 启动升级。`src` 是 `ota_source_t`（如 `ota_src_uart`），`buf` 由调用方提供 |
| `ota_app_process(a)` | `OTA_APP_BUSY` / `DONE` / `IDLE` / `ERR` |
| `ota_app_progress(a)` / `ota_app_state(a)` | 进度与状态；`flow` 成员里还有错误码与 CRC 结果 |
| `ota_app_is_trial(a)` | 本固件是否处于试运行 |
| `ota_app_confirm(a)` | 自检通过 → 固化（`active_slot = 本槽`、清 `trial_slot`、`boot_try = 0`） |
| `ota_app_set_health_cb` + `ota_app_trial_check(a)` | 注册自检回调；在试运行且回调返回非 0 时自动 confirm |
| `ota_app_reboot()` | 软复位（转 `oop_boot_system_reset()`），**不返回** |

## 用法

```c
static ota_app_t   s_ota;
static uint8_t     s_buf[4096];
static ota_src_uart_t s_src;
static uart_drv_t  s_uart;

#define OTA_SELF_BASE  0x08010000u      /* 与链接脚本 / CMake 的槽基址一致 */

int main(void)
{
    ota_app_early_init(&s_ota, OTA_SELF_BASE);      /* ← 第一行 */

    HAL_Init(); SystemClock_Config(); MX_GPIO_Init(); ...

    ota_env_t env = { s_areas, N, s_flash, 1u };
    (void)ota_flash_int_get(&s_flash[0]);
    if (ota_init(&env) != OTA_OK) { /* 分区表或状态区有问题 */ }

    /* --- 业务起来 --- */
    uart_drv_init(&s_uart, BOARD_UART1, NULL);
    ota_src_uart_init(&s_src, &s_uart, NULL, NULL);
    app_main();

    /* --- 自检通过 → 确认本次升级 --- */
    ota_app_confirm(&s_ota);
}

/* 收到升级指令时（比如 CLI / MQTT 命令） */
void on_upgrade_cmd(void)
{
    if (ota_app_start(&s_ota, ota_src_uart_source(&s_src), s_buf, sizeof(s_buf)) == OTA_OK) {
        /* 之后在主循环里轮询 */
        while (ota_app_process(&s_ota) == OTA_APP_BUSY) {
            /* 喂狗、刷进度 */
        }
        if (ota_app_state(&s_ota) == OTA_FLOW_DONE) {
            ota_app_reboot();                       /* 交给 BL 生效，不返回 */
        }
    }
}
```

## 依赖

`services.ota_core` + `chip.oop_boot`。

拉取组合：

```
APP 工程(1MB A/B)  : services.ota_core + services.ota + services.ota_src_uart
                     + protocols.ymodem + chip.oop_uart + chip.oop_boot
```
