/**
 * @file    ota_src_uart.c
 * @brief   串口 YMODEM 取数后端实现
 * @version V1.0
 * @date    2026-09-18
 */

#include <stddef.h>
#include <string.h>
#include "ota_src_uart.h"
#include "ota_core.h"
#include "oop_dwt.h"

/* ---------------- 时基 ---------------- */

static uint32_t tick_default(void *ctx)
{
    (void)ctx;
    return oop_GetTickMS();
}

static uint32_t io_tick(void *ctx)
{
    ota_src_uart_t *u = (ota_src_uart_t *)ctx;

    return u->tick(u->tick_ctx);
}

/* ---------------- UART 侧 ---------------- */

/**
 * @brief 读一个字节：1 = 读到，0 = 暂无
 * @note  `uart_drv_get_packet()` 返回的是驱动内部缓冲的指针并**加锁**，
 *        所以取完一整包必须 `release_packet()`；跨包时靠 rx_off/rx_len 续读。
 *        一个 YMODEM 帧（最多 1029 字节）因此会跨好几个 UART 空闲包。
 */
static int io_read_byte(void *ctx, uint8_t *b)
{
    ota_src_uart_t *u = (ota_src_uart_t *)ctx;

    for (;;) {
        if (u->rx_pkt != NULL && u->rx_off < u->rx_len) {
            *b = u->rx_pkt[u->rx_off++];

            /* ★ 取完最后一个字节就**立刻**解锁，不能留到下一次调用。
             *   原因：ymodem 收满一帧后会马上回 ACK，而 `uart_drv_send()` 在
             *   「state == DATA_READY 且 locked」时直接返回 -3 拒绝发送 ——
             *   锁着不放，ACK 就永远发不出去，对端只能一遍遍超时重传直到放弃。
             *   解锁顺带触发驱动的 start_rx()，把 RX DMA 重新武装上。 */
            if (u->rx_off >= u->rx_len) {
                uart_drv_release_packet(u->uart);
                u->rx_pkt = (uint8_t *)0;
                u->rx_len = 0u;
                u->rx_off = 0u;
            }
            return 1;
        }

        if (u->rx_pkt != NULL) {                  /* 兜底：不该发生，但要能自愈 */
            uart_drv_release_packet(u->uart);
            u->rx_pkt = (uint8_t *)0;
            u->rx_len = 0u;
            u->rx_off = 0u;
        }
        if (uart_drv_available(u->uart) == 0u) {
            return 0;
        }
        u->rx_pkt = uart_drv_get_packet(u->uart, &u->rx_len);
        if (u->rx_pkt == NULL || u->rx_len == 0u) {
            u->rx_pkt = (uint8_t *)0;
            u->rx_len = 0u;
            return 0;
        }
        u->rx_off = 0u;
    }
}

/**
 * @brief 写一段（发 'C' / ACK / NAK）
 * @note  `uart_drv_send()` 的返回码：`0` 成功、`-1` 参数错、`-2` TX_BUSY、
 *        `-3` 有未取走的收包且已锁定、`-4` HAL 启动失败。
 *        `-2` 理论上会出现（上一个响应还在发），但 ACK/NAK 只有 1 字节
 *        （115200 下 ~87 µs），而两次响应之间隔着「收一帧」的时间（毫秒级），
 *        所以实际概率极低。真碰上也不致命 —— ymodem 层忽略写返回值，
 *        对端会超时重传，只是慢一个周期。因此**不在这里加阻塞重试**。
 *        `-3` 在修好 io_read_byte 的即时解锁后不应再出现。
 */
static int io_write(void *ctx, const uint8_t *buf, uint32_t len)
{
    ota_src_uart_t *u = (ota_src_uart_t *)ctx;

    if (len == 0u) {
        return 0;
    }
    return (uart_drv_send(u->uart, buf, (uint16_t)len) == 0) ? 0 : -1;
}

/* ---------------- YMODEM 回调 ---------------- */

/**
 * @brief 头包解析完、**回 ACK 之前**的拦截点
 * @note  这里是「放不下就别开始收」的唯一机会：ACK 一出去，对端就开始灌数据了。
 */
static int on_header(void *user, const ymodem_file_t *f)
{
    ota_src_uart_t *u = (ota_src_uart_t *)user;
    uint32_t        limit;

    u->info.total_size = f->size;
    u->info.fw_ver     = 0u;

    limit = ota_core_image_max();                 /* = min(各 RUN 槽容量) */
    if (limit == 0u) {
        return -1;                                /* 还没 ota_init() */
    }
    if (f->size != 0u && f->size > limit) {
        return -1;                                /* 明显放不下 → 让 ymodem 发 CAN */
    }
    return 0;
}

static int on_data(void *user, const uint8_t *data, uint32_t len)
{
    ota_src_uart_t *u = (ota_src_uart_t *)user;

    if (u->pkt_len != 0u && u->pkt_off < u->pkt_len) {
        /* 上一包还没被 read 取走。ymodem 已经为这一包回过 ACK，不能反悔，
         * 所以只能让它失败 —— 静默丢数据要比这糟得多。 */
        return -1;
    }
    u->pkt     = data;
    u->pkt_len = len;
    u->pkt_off = 0u;
    return 0;
}

static void on_event(void *user, ymodem_event_t ev, uint32_t arg)
{
    (void)user;
    (void)ev;
    (void)arg;
}

/* ---------------- ota_source_t 实现 ---------------- */

static int src_open(void *ctx, ota_src_info_t *info)
{
    ota_src_uart_t *u = (ota_src_uart_t *)ctx;

    if (u == NULL) {
        return OTA_ERR_PARAM;
    }

    u->pkt       = (const uint8_t *)0;
    u->pkt_len   = 0u;
    u->pkt_off   = 0u;
    u->finished  = 0u;
    u->err       = 0;
    u->info.total_size = 0u;
    u->info.fw_ver     = 0u;
    u->info.seekable   = 0u;              /* 流式：不能回退 */

    if (ymodem_recv_start(&u->ym, &u->io, u, on_header, on_data, on_event) != YMODEM_OK) {
        return OTA_ERR_SOURCE;
    }

    u->opened = 1u;
    if (info != NULL) {
        *info = u->info;
    }
    return OTA_OK;
}

/**
 * @brief 顺序读（会阻塞，直到拿到至少 1 字节或超时）
 * @note  「有多少给多少」：不会为了填满 len 而等到超时，所以一次正常传输里
 *        单次阻塞 ≈ 一个包的时间。进度因此在每个包之后都有更新机会。
 */
static int src_read(void *ctx, void *buf, uint32_t len, uint32_t *got)
{
    ota_src_uart_t *u = (ota_src_uart_t *)ctx;
    uint8_t        *out = (uint8_t *)buf;
    uint32_t        n = 0u;
    uint32_t        t0;

    if (u == NULL || buf == NULL || got == NULL) {
        return OTA_ERR_PARAM;
    }
    *got = 0u;

    if (u->opened == 0u) {
        return OTA_ERR_STATE;
    }
    if (u->err != 0) {
        return OTA_ERR_SOURCE;
    }

    t0 = u->tick(u->tick_ctx);

    while (n < len) {
        /* 先交存量 */
        if (u->pkt_off < u->pkt_len) {
            uint32_t avail = u->pkt_len - u->pkt_off;
            uint32_t take  = len - n;

            if (take > avail) {
                take = avail;
            }
            (void)memcpy(out + n, u->pkt + u->pkt_off, take);
            u->pkt_off += take;
            n += take;

            if (u->pkt_off >= u->pkt_len) {
                u->pkt     = (const uint8_t *)0;
                u->pkt_len = 0u;
                u->pkt_off = 0u;
            }
            continue;
        }

        /* 没存量 → 推进 YMODEM（每次最多吞一帧） */
        {
            int r = ymodem_recv_process(&u->ym);

            if (r == YMODEM_OK) {
                u->finished = 1u;        /* 对端说传完了 */
                break;
            }
            if (r < 0) {
                u->err = (int8_t)r;
                break;
            }
        }

        if (u->pkt_len != 0u) {
            continue;                    /* 拿到包了，下一轮取走 */
        }
        if ((uint32_t)(u->tick(u->tick_ctx) - t0) >= u->poll_timeout_ms) {
            break;                       /* 等太久了 */
        }
    }

    if (n > 0u) {
        *got = n;
        return OTA_OK;                   /* 部分数据也算成功，上层按 got 推进 */
    }

    /* 一点都没拿到：不管是超时、结束还是引擎报错，对上层都是「源不可用」。
     * 注意正常结束时不会走到这里 —— ota_flow 收满声明长度就不再调 read 了。 */
    if (u->err == 0) {
        u->err = (int8_t)(u->finished ? OTA_ERR_SOURCE : OTA_ERR_SOURCE);
    }
    return OTA_ERR_SOURCE;
}

static void src_close(void *ctx)
{
    ota_src_uart_t *u = (ota_src_uart_t *)ctx;

    if (u == NULL) {
        return;
    }
    if (u->rx_pkt != NULL) {             /* 别把 UART 驱动锁着不放 */
        uart_drv_release_packet(u->uart);
        u->rx_pkt = (uint8_t *)0;
        u->rx_len = 0u;
        u->rx_off = 0u;
    }
    u->pkt     = (const uint8_t *)0;
    u->pkt_len = 0u;
    u->pkt_off = 0u;
    u->opened  = 0u;
}

/* ---------------- 对外 ---------------- */

int ota_src_uart_init(ota_src_uart_t *u, uart_drv_t *uart,
                      uint32_t (*tick)(void *ctx), void *tick_ctx)
{
    if (u == NULL || uart == NULL) {
        return OTA_ERR_PARAM;
    }

    (void)memset(u, 0u, sizeof(*u));
    u->uart        = uart;
    u->tick        = (tick != NULL) ? tick : tick_default;
    u->tick_ctx    = tick_ctx;
    u->poll_timeout_ms = OTA_SRC_UART_DEF_POLL_MS;

    u->io.read_byte = io_read_byte;
    u->io.write     = io_write;
    u->io.tick      = io_tick;
    u->io.ctx       = u;

    u->src.name  = "uart-ymodem";
    u->src.open  = src_open;
    u->src.read  = src_read;
    u->src.seek  = (int (*)(void *, uint32_t))0;   /* 流式，没有 seek */
    u->src.close = src_close;
    u->src.ctx   = u;

    return OTA_OK;
}

ota_source_t *ota_src_uart_source(ota_src_uart_t *u)
{
    return (u != NULL) ? &u->src : (ota_source_t *)0;
}

void ota_src_uart_set_poll_timeout(ota_src_uart_t *u, uint16_t ms)
{
    if (u != NULL && ms != 0u) {
        u->poll_timeout_ms = ms;
    }
}

const char *ota_src_uart_state(const ota_src_uart_t *u)
{
    if (u == NULL) {
        return "?";
    }
    return ymodem_recv_state_name(u->ym.st);
}
