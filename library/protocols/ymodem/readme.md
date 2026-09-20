# protocols.ymodem —— YMODEM 收发引擎（V1.0）

> 非阻塞状态机的 YMODEM 收发，**零硬件依赖**（收字节 / 发字节 / 时基全部注入），
> 所以它能在 PC 上跑完整单测。

## 为什么不用现成的

参考工程 `refe/Ymodem-master`（= STM32F0xx_IAP 官方版）和 `refe/F407ZG/.../YMODEM` 都是
**阻塞**实现：在一个函数里死等包头、死等 ACK，收完整个文件才返回。

那套写法在独立 BL 里勉强能用，装进带协议栈 / 带任务的 APP 就不行 —— 一次传输几秒到
几十秒，期间什么都不干，看门狗会先咬人。

这里改成**非阻塞状态机**：调用方反复调 `ymodem_recv_process()` / `ymodem_send_process()`，
每次只做「把此刻已经到达的字节吃掉」这一件事。调用频率决定吞吐上限（建议 1 ms 级轮询）。

## 协议要点（都是踩过的地方）

| 要点 | 说明 |
|---|---|
| 帧结构 | `SOH\|STX` + `seq` + `~seq` + data + `crc16(2)`；CRC16-XMODEM（poly `0x1021`，初值 0） |
| **SOH 恒配 128，STX 恒配 1024** | `ymodem_build_frame()` 对其它长度直接返回 0。拿 SOH 发 1024 字节是最经典的上手错误：接收端按 128 解析，整包错位，而且 **CRC 还能过**（校验的就是被截断的那 128 字节），异常得很安静 |
| 起头方 | 接收方发 `'C'`(0x43)，发送方才开始 |
| 序号 0 | 是「文件名 + 十进制大小」头包；数据包从 1 起，255 之后回绕到 0 |
| **重复包** | `seq == 上一包` 时**重新 ACK，但不重复写数据**。ACK 丢了导致对端重发是协议的正常行为，不是错误 |
| EOT 收尾 | `EOT` → ACK → 对端再发一个**空第 0 包** → ACK，传输才结束 |
| 超时重发 | 重发**上一次的响应**（`'C'` / ACK / NAK），而不是无脑发 NAK |
| 中止 | 连发两个 `CAN`，或单发 `'A'` / `'a'` |

### 两处刻意的宽容

- **EOT 一定回 ACK**。XMODEM 风格的对端会期待先 NAK 再 EOT 才 ACK，它收不到 NAK 会重发 EOT ——
  我们第二次收到 EOT 时按「传输结束」处理，两种风格都能收敛。
- **结束阶段没收到空第 0 包就再收到一个 EOT** 时，也认为传输结束。
  `server/fw-ota-ymodem.py` 那种「连发两个 EOT」的 PC 端脚本因此也能用。

### 一个内部约定

`ymodem_recv_process()` **一次只处理一个完整帧就返回**。因为 `on_data` 拿到的指针指向
引擎内部的帧缓冲，下一次 process 会覆盖它 —— 上层（如 `services.ota_src_uart`）需要它
活到下一次调用之前。一次多吞几帧的吞吐收益，抵不上数据被覆盖的风险。

## API

| 函数 | 说明 |
|---|---|
| `ymodem_recv_start(y, io, user, on_header, on_data, on_event)` | 启动接收；**本函数立刻发第一个 `'C'`**，所以通道必须先就绪 |
| `ymodem_recv_process(y)` | `YMODEM_BUSY` / `YMODEM_OK` / 负错误码 |
| `ymodem_recv_set_opts(y, retry_max, timeout_ms)` | 默认 10 次 / 1000 ms |
| `ymodem_send_start(y, io, user, name, size, on_pull, on_event)` | 启动发送；**不立刻发东西**，先等对端 `'C'` |
| `ymodem_send_process(y)` | 同上 |
| `ymodem_send_set_opts(y, use_1k, retry_max, timeout_ms)` | `use_1k` 默认 1（头包仍固定 128） |
| `ymodem_crc16` / `ymodem_build_frame` / `ymodem_check_frame` | 工具，也供上层自测 |

`on_header` **在回 ACK 之前**调用，返回非 0 表示拒收（发 CAN 中止）。
「目标区放不下」这类判断必须放在这里 —— ACK 一出去，对端就开始灌数据了。

## 单测（PC 上真跑，`p3_host_test.c`）

把两个引擎通过内存通道对接，再注入真机上很难复现的场景：

| 用例 | 验的是 |
|---|---|
| 帧工具 ×9 | 长度、`SOH`/`STX` 选择、序号反码、CRC、**SOH 配 1024 必须被拒** |
| 1K 包 / 5000 字节 | 端到端数据一致 |
| 128 包 / 1000 字节 | 小包路径 |
| 整包 2048 字节 | 末包不填充的分支 |
| **整包丢掉 ACK(包1)** | 发送端超时重发 → 接收端**去重**（`dup == 1`） |
| 翻转上行 40 字节 | 接收端 NAK → 发送端重传，最终数据仍一致 |
| 接收端头包处拒收 | CAN 中止，且**一个字节都没写进去** |
| 空文件（size = 0） | 流程能走完 |

> `size == 0` 时协议上无法确定有效长度（末包的 `0x1A` 填充无从区分），
> 所以那一项只验「流程走完」。真机上 `fw-ota-pack.py` 总会写大小。

## 依赖

**零依赖**（只用 `<stdint.h>` / `<stddef.h>` / `<string.h>`）。
`ymodem.c` 因此可以单独用 host gcc 编译 —— 这是它唯一的测试方式，
也是这个模块最值钱的地方。
