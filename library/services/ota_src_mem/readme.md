# services.ota_src_mem — 取数后端：内存/RAM

## 定位

一块内存冒充 `.otapkg`，让 OTA 逻辑在没有**任何**传输协议栈的情况下跑通全链路。
零依赖（只要 `services.ota_core` 的头）。

它的价值不在产品功能，而在**把 OTA 逻辑与传输通道解耦开**：

> 先用内存源把「写 → 双重校验 → 提交」跑通并确认无误，再去调 YMODEM。
> 否则通道的问题和 OTA 逻辑的问题会混在一起，排查成本翻几倍。

## 用法

```c
#include "ota_src_mem.h"

static const uint8_t s_pkg[] = { /* .otapkg 字节（可用 xxd -i 从文件生成） */ };

static ota_src_mem_t s_st;
static ota_source_t  s_src;

void task_ota_selftest_start(void)
{
    ota_src_mem_setup(&s_src, &s_st, s_pkg, sizeof(s_pkg), 1u);   /* 1 = 可 seek */
    /* 之后照常走 ota_flow_start(&flow, {.source = &s_src, ...}) */
}
```

### 两个 mode 覆盖两条路径

| `seekable` | 模拟的通道 | 覆盖的代码路径 |
|---|---|---|
| `1`（默认） | HTTP / TF 卡 | `DECIDE` 里用 `seek` 直接跳到目标段（双段包只读自己那一段） |
| `0` | 串口 YMODEM | 不可回退 → 顺序读完并丢弃前序段；段不在首位时返回 `OTA_ERR_UNSUPPORTED` |

两种都跑一遍，`ota_flow` 的分支才算都验过。

## 建议的上板自测流程

1. PC 上 `python tools/ota_pack.py --slot b --bin app_slotB.bin --ver 0.0.1 -o test.otapkg`
2. 把 `test.otapkg` 的内容通过 RTT 或命令行灌进 RAM（几 KB 量级，分块喂即可）
3. `ota_src_mem_setup(..., seekable = 1)` 跑一遍、再 `seekable = 0` 跑一遍
4. 观察 `ota_flow` 的 `state` / `err` / `crc_mem` / `crc_flash` 与 CFG 里的
   `pending_action` / `target_slot` / `image_crc32`

这样在接入真实通道**之前**就能确定：包格式、CRC 参数、段匹配、CFG 提交全对。

## 坑位

| 现象 | 原因 |
|---|---|
| `open` 返回 `OTA_ERR_SOURCE` | `data == NULL` 或 `size == 0` |
| `OTA_ERR_UNSUPPORTED` | 用 `seekable = 0` 跑双段包，而目标段是第二段 —— 这正是串口场景要改用**单段包**的原因 |
| 校验通过但设备起不来 | 不是本模块的事：检查段地址（`--load-a/--load-b`）是否与该槽 CPU 地址一致 |
