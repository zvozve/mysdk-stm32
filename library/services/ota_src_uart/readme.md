# services.ota_src_uart —— 串口 YMODEM 取数后端（V1.0）

> 把 `uart_drv_t` + `protocols.ymodem` 适配成 `ota_source_t`，
> 让 `ota_flow` 完全不知道数据是从串口来的。

通道按模块拆（而不是一个模块里 `#ifdef` 分支），因为拉取的粒度是**目录**：
只有拆开，才能做到「选了 uart 就完全不引入 lwip/fatfs 的依赖」。

| 模块 | 通道 |
|---|---|
| `services.ota_src_uart` | 串口 YMODEM（本模块） |
| `services.ota_src_http` | lwIP HTTP + `Range`（P6） |
| `services.ota_src_tfcard` | TF 卡 FATFS（待定） |
| `services.ota_src_mem` | 内存（自测，零依赖） |

## ⚠ 三个必须知道的前提

### 1. 工程侧要把 UART 缓冲加大

`chip.oop_uart` 的 `UART_DRV_BUF_SIZE` 默认 **256**，而一个 YMODEM 1K 帧是 **1029 字节**
（`1 + 1 + 1 + 1024 + 2`）。缓冲不够时 DMA 会覆盖还没被取走的字节 ——
现象是「随机某个包 CRC 错、重传几次后又能过」，非常难查。

```cmake
# 工程 CMakeLists（必须在 add_executable 之前，mystm32 子目标才继承得到）
add_compile_definitions(UART_DRV_BUF_SIZE=1088)
```

代价：每个 `uart_drv_t` 实例 rx/tx 各多 832 字节（F407 上 ~1.7 KB/实例）。

只发 **128 字节小包**（帧长 133）时默认值就够 —— 但 YMODEM 里用多大包是**发送方**决定的，
接收方只能被动接受。用 SecureCRT / lrzsz 这类工具时不好控制，所以**建议直接加大缓冲**。

### 2. 单次 `read()` 会阻塞，上限 = `poll_timeout_ms`

默认 **2000 ms**，可用 `ota_src_uart_set_poll_timeout()` 调整。

这是流式源的固有属性：「等对端把下一包发来」没有非阻塞的做法。
所以：

- **看门狗超时必须 > `poll_timeout_ms`**，否则一次慢包就复位
- 已经拿到的字节会**立刻返回**（不攒满再给），所以正常传输时单次阻塞 ≈ 一个包的时间
  （115200 下 1 KB 包 ≈ 90 ms）
- 想更保守就把超时收到 500~800 ms，代价是慢包更容易被判失败

### 3. 是流式源：`seek == NULL`、`seekable == 0`

所以只能顺序读。多段包（`seg_count=2`）时 `ota_flow` 会把不需要的段**读出来丢掉**；
串口场景**建议直接打单段包**：

```
python tools/ota_pack.py --slot a --bin app.bin --ver 1.2.3 -o app.otapkg
```

## 数据流

```
PC ──YMODEM 帧──> uart_drv(rx_buf) ──> ymodem_recv ──on_data──> [本模块的 pkt 游标]
                                                                      │
                                              ota_source_t::read ─────┘
                                                      │
                                              ota_flow 写 Flash
```

一个 YMODEM 帧（最多 1029 字节）会跨好几个 UART 空闲包（每包 ≤ `UART_DRV_BUF_SIZE`），
`ota_src_uart` 用 `rx_off` / `rx_len` 逐个字节续读，读完才 `uart_drv_release_packet()`。
**所以不要绕过本模块直接去 `uart_drv_get_packet()`** —— 会把驱动的包锁弄乱。

## 头包处的第一道闸

`on_header` 回调在 **YMODEM 回 ACK 之前**触发，这里用 `ota_core_image_max()`
（= min 各 RUN 槽容量）拦一次「明显放不下」的文件。

拦不住的情况：包比目标区小、但里面的段比槽大 —— 那由 `ota_flow` 的 `DECIDE` 阶段
报 `OTA_ERR_NOSPACE`。两道闸都在，缺一不可（前者省掉整场传输，后者是真正的判据）。

## API

| 函数 | 说明 |
|---|---|
| `ota_src_uart_init(u, uart, tick, tick_ctx)` | `tick` 传 NULL 则用 SDK 的 `oop_GetTickMS()` |
| `ota_src_uart_source(u)` | 取出 `ota_source_t*`，传给 `ota_app_start()` |
| `ota_src_uart_set_poll_timeout(u, ms)` | 单次 read 最长阻塞 |
| `ota_src_uart_state(u)` | YMODEM 引擎当前状态名（日志用） |

## 依赖

`services.ota_core` + `protocols.ymodem` + `chip.oop_uart` + `chip.oop_dwt`。

工程侧前提：`.ioc` 里启用对应 USART（DMA + 空闲中断），并在 `board_cfg.h` 暴露语义名。
