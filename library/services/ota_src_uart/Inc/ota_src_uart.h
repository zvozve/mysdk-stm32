/**
 * @file    ota_src_uart.h
 * @brief   取数后端：串口 YMODEM（把 uart_drv + ymodem 适配成 ota_source_t）
 * @version V1.0
 * @date    2026-09-18
 *
 * 分层：`services.ota_core` 只认识 `ota_source_t`，本模块是它在串口上的一个实现。
 * 通道按模块拆（而不是一个模块里 `#ifdef` 分支），是为了让「选了 uart 就完全不
 * 引入 lwip/fatfs 的依赖」成立 —— 拉取粒度是目录。
 *
 * ⚠ **流式源**：`seek == NULL`、`info.seekable == 0`，所以只能顺序读。
 *   多段包（seg_count=2）时 `ota_flow` 会顺序丢弃不需要的段；
 *   串口场景**建议直接打单段包**（`ota_pack.py --slot a --bin ...`）。
 *
 * ⚠ **单次 `read()` 会阻塞，上限 = `poll_timeout_ms`**（默认 2000 ms）。
 *   这是流式源的固有属性 —— 「等对端把下一包发来」这件事没有非阻塞的做法，
 *   只能等到超时为止。所以：
 *     · 看门狗超时必须 > poll_timeout_ms，否则一次慢包就复位
 *     · 已经拿到的字节会立刻返回（不攒满再给），所以正常传输时单次阻塞 ≈ 一个包的时间
 *       （115200 下 1 KB 包 ≈ 90 ms）
 *
 * ⚠ **工程侧前置条件**：`UART_DRV_BUF_SIZE` 必须 ≥ 一个 YMODEM 帧
 *   （1 KB 包 = 1029 字节，建议 `-DUART_DRV_BUF_SIZE=1088`）。
 *   保持默认 256 时只能收 128 字节小包，且 PC 端也要配成 128 —— 见 readme.md。
 */

#ifndef __OTA_SRC_UART_H
#define __OTA_SRC_UART_H

#include <stdint.h>
#include "ota_source.h"
#include "ymodem.h"
#include "oop_uart_drv.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 默认单次 read 的最长阻塞（毫秒） */
#define OTA_SRC_UART_DEF_POLL_MS   2000u

/**
 * @brief 等待数据期间的空闲回调（喂狗 / 让出 CPU / 请求中止）
 * @return 非 0 = 请求中止本次 read（read 将以 OTA_ERR_SOURCE 失败）
 * @note  单次 read 最长阻塞 poll_timeout_ms，这个回调是这段阻塞里**唯一**的
 *        喂狗 / 让出 CPU 机会 —— 板上有看门狗就必须设。
 */
typedef int (*ota_src_uart_idle_cb)(void *user);

typedef struct {
    /* 对外接口（用 ota_src_uart_source() 取） */
    ota_source_t   src;

    /* 私有 */
    uart_drv_t    *uart;
    ymodem_recv_t  ym;
    ymodem_io_t    io;

    /* UART 收包游标（一个 YMODEM 帧会跨多个 UART 空闲包） */
    uint8_t       *rx_pkt;
    uint16_t       rx_len;
    uint16_t       rx_off;

    /* YMODEM 刚交付的那一包（等待被 read 取走；指针指向 ym.frame 内部） */
    const uint8_t *pkt;
    uint32_t       pkt_len;
    uint32_t       pkt_off;

    ota_src_info_t info;
    uint16_t       poll_timeout_ms;

    ota_src_uart_idle_cb idle;      /*!< 等包期间的钩子（喂狗/节流/中止）；可 NULL */
    void                *idle_user;

    uint8_t        opened;
    uint8_t        finished;
    int8_t         err;

    uint32_t     (*tick)(void *ctx);
    void          *tick_ctx;
} ota_src_uart_t;

/**
 * @brief  初始化
 * @param  uart     已 `uart_drv_init()` 并接好中断/回调的串口实例
 * @param  tick     毫秒时基；传 NULL 则用 SDK 的 `oop_GetTickMS()`
 * @param  tick_ctx 传给 tick 的上下文
 * @return OTA_OK / OTA_ERR_PARAM
 * @note   YMODEM 引擎与 UART 收包游标都在这里复位；`open()` 时才发第一个 'C'。
 */
int ota_src_uart_init(ota_src_uart_t *u, uart_drv_t *uart,
                      uint32_t (*tick)(void *ctx), void *tick_ctx);

/** @brief 取出 `ota_source_t*`（传给 `ota_app_start()` / `ota_flow_start()`） */
ota_source_t *ota_src_uart_source(ota_src_uart_t *u);

/** @brief 调整单次 read 的最长阻塞；0 = 不改（默认 2000 ms） */
void ota_src_uart_set_poll_timeout(ota_src_uart_t *u, uint16_t ms);

/**
 * @brief 注册「等包期间」的空闲钩子
 * @param  cb   回调（见类型注释）；NULL = 不设（板子没看门狗时可以）
 * @note   推荐实现：喂狗 + `vTaskDelay(1)`（既喂狗又节流，还避免同优先级任务饿死）。
 *         返回非 0 会让正在进行的 read 立即失败。
 */
void ota_src_uart_set_idle_cb(ota_src_uart_t *u, ota_src_uart_idle_cb cb, void *user);

/** @brief 调试用：YMODEM 引擎当前状态名 */
const char *ota_src_uart_state(const ota_src_uart_t *u);

#ifdef __cplusplus
}
#endif

#endif /* __OTA_SRC_UART_H */
