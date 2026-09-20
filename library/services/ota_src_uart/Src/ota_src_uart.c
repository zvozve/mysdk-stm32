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

    /* 元数据优先：bin 声明的大小必须与元数据的 size 一致
     * （不一致 = 发错文件 / 选错 bin，早拒比收完再发现好） */
    if (u->exp_size != 0u && f->size != 0u && f->size != u->exp_size) {
        return -1;
    }

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

/* ---------------- 元数据优先（方案 2） ---------------- */

/**
 * @brief 元数据会话的头包回调：只接受**恰好 8 字节**（size+crc32），否则拒收
 * @note  长度不对 = 对端没按「先发元数据」来（或直接发了 bin），拒收比误当成
 *        元数据去解析要好得多。
 */
static int meta_on_header(void *user, const ymodem_file_t *f)
{
    ota_src_uart_t *u = (ota_src_uart_t *)user;

    if (f->size != 8u) {
        return -1;
    }
    u->meta_len = 0u;
    return 0;
}

static int meta_on_data(void *user, const uint8_t *data, uint32_t len)
{
    ota_src_uart_t *u = (ota_src_uart_t *)user;
    uint32_t        i;

    /* 数据包会被填充到 128/1024 字节，只留前 8 个 */
    for (i = 0u; i < len && u->meta_len < 8u; i++) {
        u->meta_buf[u->meta_len++] = data[i];
    }
    return 0;
}

static void meta_on_event(void *user, ymodem_event_t ev, uint32_t arg)
{
    (void)user;
    (void)ev;
    (void)arg;
}

int ota_src_uart_meta_start(ota_src_uart_t *u)
{
    if (u == NULL) {
        return OTA_ERR_PARAM;
    }

    u->meta_len          = 0u;
    u->meta_done         = 0u;
    u->meta_err          = 0;
    u->meta_active       = 1u;
    u->meta_t0           = u->tick(u->tick_ctx);
    u->exp_size          = 0u;      /* 清掉上一轮的期望值，避免误用 */
    u->info.expect_crc32 = 0u;
    u->info.total_size   = 0u;

    if (ymodem_recv_start(&u->ym, &u->io, u, meta_on_header, meta_on_data, meta_on_event)
        != YMODEM_OK) {
        u->meta_active = 0u;
        return OTA_ERR_SOURCE;
    }
    return OTA_OK;
}

int ota_src_uart_meta_poll(ota_src_uart_t *u)
{
    int r;

    if (u == NULL) {
        return -1;
    }
    if (u->meta_done != 0u) {
        return 1;
    }
    if (u->meta_active == 0u) {
        return -1;
    }

    r = ymodem_recv_process(&u->ym);
    if (r == YMODEM_OK) {
        uint32_t sz;
        uint32_t cr;

        if (u->meta_len < 8u) {
            u->meta_active = 0u;
            u->meta_err    = (int8_t)OTA_ERR_SOURCE;
            return -1;
        }
        sz = (uint32_t)u->meta_buf[0]
           | ((uint32_t)u->meta_buf[1] << 8)
           | ((uint32_t)u->meta_buf[2] << 16)
           | ((uint32_t)u->meta_buf[3] << 24);
        cr = (uint32_t)u->meta_buf[4]
           | ((uint32_t)u->meta_buf[5] << 8)
           | ((uint32_t)u->meta_buf[6] << 16)
           | ((uint32_t)u->meta_buf[7] << 24);

        u->exp_size          = sz;        /* bin 段 on_header 交叉校验用 */
        u->info.total_size   = sz;
        u->info.expect_crc32 = cr;        /* ota_flow 据此校验整段镜像 */
        u->meta_done         = 1u;
        u->meta_active       = 0u;
        return 1;
    }
    if (r < 0) {
        u->meta_active = 0u;
        u->meta_err    = (int8_t)r;
        return -1;
    }

    /* 超时保护：'U' 之后迟迟收不到元数据就不死等 */
    if ((uint32_t)(u->tick(u->tick_ctx) - u->meta_t0) >= OTA_SRC_UART_META_TIMEOUT_MS) {
        u->meta_active = 0u;
        u->meta_err    = (int8_t)OTA_ERR_SOURCE;
        return -1;
    }
    return 0;
}

void ota_src_uart_meta_get(const ota_src_uart_t *u, ota_src_uart_meta_t *out)
{
    if (out == NULL) {
        return;
    }
    out->size  = 0u;
    out->crc32 = 0u;
    if (u == NULL || u->meta_done == 0u || u->meta_len < 8u) {
        return;
    }
    out->size  = (uint32_t)u->meta_buf[0] | ((uint32_t)u->meta_buf[1] << 8)
               | ((uint32_t)u->meta_buf[2] << 16) | ((uint32_t)u->meta_buf[3] << 24);
    out->crc32 = (uint32_t)u->meta_buf[4] | ((uint32_t)u->meta_buf[5] << 8)
               | ((uint32_t)u->meta_buf[6] << 16) | ((uint32_t)u->meta_buf[7] << 24);
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
    /* 流式源在 open 时本拿不到总大小 —— 大小只能靠「元数据优先」段先告知
     * （见 ota_src_uart_meta_start/poll 填的 exp_size）。没有元数据（0）时保持 0：
     * 此时裸 bin 无法定长（用 .otapkg 包则包头自带长度，不受影响）。 */
    u->info.total_size = u->exp_size;
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
        /* 这段阻塞里唯一的喂狗 / 节流 / 中止机会（板上有看门狗就必须设这个钩子） */
        if (u->idle != NULL && u->idle(u->idle_user) != 0) {
            u->err = (int8_t)OTA_ERR_SOURCE;
            break;
        }
        if ((uint32_t)(u->tick(u->tick_ctx) - t0) >= u->poll_timeout_ms) {
            break;                       /* 等太久了 */
        }
    }

    /* 声明长度已全部送达 → 在本模块内把 YMODEM 收尾（EOT + 空第 0 包）走完。
     * 上游 ota_flow 收满声明长度就不再调 read，若不在这里补这一步，对端发完数据后
     * 会等不到 ACK 而一直重试（收尾握手是本模块的协议职责，不该上抛给 ota_flow）。 */
    if (n > 0u && u->finished == 0u && u->err == 0
        && u->ym.file.size != 0u && u->ym.got >= u->ym.file.size) {
        uint32_t t1 = u->tick(u->tick_ctx);

        while (u->finished == 0u && u->err == 0) {
            int r = ymodem_recv_process(&u->ym);

            if (r == YMODEM_OK) {
                u->finished = 1u;
                break;
            }
            if (r < 0) {
                u->err = (int8_t)r;
                break;
            }
            if (u->pkt_len != 0u) {
                break;                       /* 不该再有数据；交给下一轮 */
            }
            if (u->idle != NULL && u->idle(u->idle_user) != 0) {
                break;
            }
            if ((uint32_t)(u->tick(u->tick_ctx) - t1) >= u->poll_timeout_ms) {
                break;                       /* 对端迟迟不发收尾，别死等 */
            }
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

void ota_src_uart_set_idle_cb(ota_src_uart_t *u, ota_src_uart_idle_cb cb, void *user)
{
    if (u == NULL) {
        return;
    }
    u->idle      = cb;
    u->idle_user = user;
}

const char *ota_src_uart_state(const ota_src_uart_t *u)
{
    if (u == NULL) {
        return "?";
    }
    return ymodem_recv_state_name(u->ym.st);
}
