# WS1850S RFID 读卡器驱动（异步 / OOP）

WS1850S 是 MFRC522/RC522 的国产兼容 RFID 读卡模块，通过 **UART 二进制帧协议**与 MCU 通信，
支持 ISO14443A(M1) 卡号/数据读取、15693、二代证、Felica 等多种卡型（工作模式可配）。

本模块为 **异步 / 非阻塞** 版本（对应原工程 `mcu-dev` 的 UART 状态机思路），已重写为 SDK 风格：

- **板无关**：UART 传输走 `chip.oop_uart`（`uart_drv_t*` 注入），计时走 `chip.oop_dwt`，
  日志走 `middleware.SEGGER_RTT`，**不引用任何 CubeMX 头 / 全局句柄 / 直调 HAL_***。
- **拉取模型**：应用周期调用 `ws1850s_process()`，驱动内部状态机收包、解析、触发回调，全程不阻塞。
- **两种用法**：
  1. 工作模式设为主动读（`WS1850S_MODE_READ_M1_UID_DATA`）→ 卡片靠近时自动触发 `card_cb`；
  2. 需要按需读某块 → 调 `ws1850s_read_m1_block()`，结果经 `result_cb` 异步返回。

---

## 帧协议

```
[0x60/0xE0][dev_id][len_h][len_l][cmd][data × len][checksum]
  start        data_len(大端, =params 长度)   cmd   params     XOR(前面所有字节)
```

- 起始字节：`0x60` 正常帧，`0xE0` 错误帧（错误帧中 `idx4` 为状态码）。
- 校验和 = 从起始字节到 checksum 前一字节的 **异或**。
- 命令码：`0x02` 查地址 / `0x03` 写地址 / `0x04` 写工作模式 / `0x11` 取卡号 / `0x12` 读 M1 块。
- 状态码：`0x00` 成功，`0xA2` 无卡，`0xA3` 认证失败（详见 `ws1850s.h`）。

> 注：命令应答的 `data` 解析（取卡号/块数据）忠实移植自原工程 demo，
> 与原硬件固件匹配；若你的模块固件版本返回格式不同，调整 `ws1850s.c` 中
> `ws1850s_update_card_from_response()` / `ws1850s_parse_active_card()` 即可，接口不变。

---

## 使用流程

```c
#include "ws1850s.h"
#include "oop_uart_drv.h"

static ws1850s_drv_t *g_rfid;
static uart_drv_t     g_rfid_uart;

/* 1) 准备 oop_uart（含 RS485 DE 如有，在 uart_drv_init 注入） */
uart_drv_init(&g_rfid_uart, &huartX, NULL);
uart_drv_reg_cb(&g_rfid_uart, NULL, NULL, NULL); /* 驱动用拉取模型 */

/* 2) 创建驱动实例 */
ws1850s_params_t p = {
    .device_id       = 0x00,
    .write_device_id = 0x21,
    .work_mode       = WS1850S_MODE_READ_M1_UID_DATA, /* 主动读卡号+数据 */
    .block_addr      = 0x02,
    .resp_timeout_ms = 1000,
    .max_retry       = 3,
    .gpio_approach   = NULL, /* 可选：卡片靠近输入引脚 gpio_dev_t* */
};
g_rfid = ws1850s_create(&g_rfid_uart, &p);

/* 3) 注册回调 */
ws1850s_register_callbacks(g_rfid,
    my_result_cb, NULL,   /* 命令结果（查询地址/读块等） */
    my_card_cb,   NULL,   /* 主动读到卡片 */
    my_state_cb,  NULL);  /* 状态变化（INITIALIZING→READY→ERROR） */

/* 4) 启动（非阻塞：内部发查询地址→写地址→写工作模式，结果经回调/状态机推进） */
ws1850s_start(g_rfid);

/* 5) 周期调用（任务 tick / 定时器，建议 10~50ms 一次） */
void App_Task(void) {
    ws1850s_process(g_rfid);
}

/* 6) 按需读取某块（异步，结果在 my_result_cb 中按 cmd==WS1850S_CMD_READ_M1_DATA 区分） */
ws1850s_read_m1_block(g_rfid, 0x01);
```

### 回调示例

```c
void my_card_cb(const ws1850s_card_t *card, void *ctx) {
    /* card->uid[0..card->uid_len-1] 为卡号，card->block_data[0..15] 为块数据 */
    printf("UID: ");
    for (int i = 0; i < card->uid_len; i++) printf("%02X ", card->uid[i]);
    printf("\n");
}

void my_result_cb(ws1850s_result_t *r, void *ctx) {
    if (r->status != WS1850S_STATUS_SUCCESS) { /* 处理超时/错误 */ return; }
    switch (r->cmd) {
        case WS1850S_CMD_READ_M1_DATA: /* r->data 为块数据 */ break;
        case WS1850S_CMD_QUERY_ADDR:    /* r->data[0] 为设备地址 */ break;
        default: break;
    }
}
```

---

## 与「阻塞版」的区别（为什么是异步）

| 维度 | 阻塞版（mcu-main） | 异步版（本模块，mcu-dev 思路） |
|------|-------------------|-------------------------------|
| 收应答 | `TIM6_Delay_ms(timeout)` 忙等 | `ws1850s_process()` 拉取，不阻塞 |
| 状态 | 顺序调用，函数内等结果 | 状态机 + 回调，调用即返回 |
| 多设备 | 难并发 | 每实例独立 `ws1850s_drv_t`，互不影响 |
| 适用 | 裸机单任务 demo | RTOS / 多外设并发 / 低延迟任务 |

---

## 依赖

- `chip.oop_uart`（UART 传输）
- `chip.oop_dwt`（计时 `oop_GetTickMS`）
- `chip.oop_gpio`（可选：卡片靠近输入 `gpio_approach`）
- `middleware.SEGGER_RTT`（日志）

## 已知边界

- 未做 RS485 方向脚处理：RS485 是 UART 层关注点，由 `oop_uart` 的 `uart_rs485_t` 在 `uart_drv_init` 注入即可，本模块无需关心。
- 帧解析假定 oop_uart 每包恰好一帧（UART 空闲线分隔），与原模块 `ReceiveToIdle_DMA` 行为一致。
- 该驱动未在硬件上实测（无编译器/目标板），协议解析已对齐原工程 demo 源码；首次上板请结合 RTT 日志用 `my_result_cb`/`my_card_cb` 验证帧字段。
