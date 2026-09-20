/**
 * @file    ymodem.c
 * @brief   YMODEM 收发引擎实现（非阻塞状态机）
 * @version V1.0
 * @date    2026-09-18
 */

#include <stddef.h>
#include <string.h>
#include "ymodem.h"

/* 数据区在帧内的偏移：包类型(1) + 序号(1) + 序号反码(1) */
#define YM_DATA_OFF   3u

/* ---------------- 工具 ---------------- */

uint16_t ymodem_crc16(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0u;

    if (data == NULL) {
        return 0u;
    }
    for (uint32_t i = 0u; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (uint8_t b = 0u; b < 8u; b++) {
            if ((crc & 0x8000u) != 0u) {
                crc = (uint16_t)((crc << 1) ^ 0x1021u);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

uint32_t ymodem_build_frame(uint8_t *out, uint8_t seq, const uint8_t *data, uint32_t data_len)
{
    uint16_t crc;

    if (out == NULL || data == NULL) {
        return 0u;
    }
    if (data_len != YMODEM_DATA_128 && data_len != YMODEM_DATA_1K) {
        return 0u;                       /* 只允许这两个长度，杜绝 SOH 配 1024 */
    }

    out[0] = (data_len == YMODEM_DATA_128) ? YMODEM_SOH : YMODEM_STX;
    out[1] = seq;
    out[2] = (uint8_t)(0xFFu - (uint16_t)seq);

    /* 调用方可能直接把数据取到 out+3（发送侧就这么干，省一次 1 KB 拷贝），
     * 那种情况下源与目标重叠，必须用 memmove 语义。 */
    if (data != (const uint8_t *)(out + YM_DATA_OFF)) {
        (void)memmove(out + YM_DATA_OFF, data, data_len);
    }

    crc = ymodem_crc16(out + YM_DATA_OFF, data_len);
    out[YM_DATA_OFF + data_len]      = (uint8_t)(crc >> 8);
    out[YM_DATA_OFF + data_len + 1u] = (uint8_t)(crc & 0xFFu);

    return data_len + YMODEM_OVERHEAD;
}

int ymodem_check_frame(const uint8_t *frame, uint32_t frame_len,
                       uint8_t *seq_out, const uint8_t **data_out, uint32_t *data_len_out)
{
    uint32_t dlen;
    uint16_t crc_rx;
    uint16_t crc_calc;

    if (frame == NULL || frame_len < (YMODEM_OVERHEAD + YMODEM_DATA_128)) {
        return YMODEM_ERR_PARAM;
    }

    if (frame[0] == YMODEM_SOH) {
        dlen = YMODEM_DATA_128;
    } else if (frame[0] == YMODEM_STX) {
        dlen = YMODEM_DATA_1K;
    } else {
        return YMODEM_ERR_PROTO;
    }
    if (frame_len != (dlen + YMODEM_OVERHEAD)) {
        return YMODEM_ERR_PROTO;
    }
    if ((uint8_t)(frame[1] + frame[2]) != 0xFFu) {
        return YMODEM_ERR_PROTO;         /* 序号反码不符 */
    }

    crc_rx   = (uint16_t)(((uint16_t)frame[YM_DATA_OFF + dlen] << 8) |
                           (uint16_t)frame[YM_DATA_OFF + dlen + 1u]);
    crc_calc = ymodem_crc16(frame + YM_DATA_OFF, dlen);
    if (crc_rx != crc_calc) {
        return YMODEM_ERR_PROTO;
    }

    if (seq_out != NULL)      { *seq_out = frame[1]; }
    if (data_out != NULL)     { *data_out = frame + YM_DATA_OFF; }
    if (data_len_out != NULL) { *data_len_out = dlen; }
    return YMODEM_OK;
}

/* ============================================================
 *  接收侧
 * ============================================================ */

static void ev_recv(ymodem_recv_t *y, ymodem_event_t e, uint32_t arg)
{
    if (y->on_event != NULL) {
        y->on_event(y->user, e, arg);
    }
}

static void send_resp(ymodem_recv_t *y, uint8_t b)
{
    y->last_resp = b;
    (void)y->io->write(y->io->ctx, &b, 1u);
}

static void recv_touch(ymodem_recv_t *y)
{
    y->t_ref = y->io->tick(y->io->ctx);
    y->retry = 0u;
}

static void recv_fail(ymodem_recv_t *y, ymodem_ret_t e)
{
    uint8_t can[2];

    y->st  = YMODEM_RECV_FAILED;
    y->err = e;

    can[0] = YMODEM_CAN;
    can[1] = YMODEM_CAN;
    (void)y->io->write(y->io->ctx, can, 2u);   /* 通知对端别发了 */
}

/** @brief 解析头包：文件名 + 十进制大小 */
static void parse_header(ymodem_recv_t *y, const uint8_t *data, uint32_t dlen)
{
    uint32_t i = 0u;
    uint32_t n = 0u;
    uint32_t sz = 0u;

    while (i < dlen && data[i] != 0u && n < (YMODEM_NAME_MAX - 1u)) {
        y->file.name[n++] = (char)data[i++];
    }
    y->file.name[n] = '\0';

    while (i < dlen && data[i] == 0u) {
        i++;                             /* 跳到大小字段 */
    }
    while (i < dlen && data[i] == ' ') {
        i++;                             /* 容错：有些实现留了空格 */
    }
    while (i < dlen && data[i] >= '0' && data[i] <= '9') {
        if (sz > 0x00FFFFFFu) {
            sz = 0x00FFFFFFu;            /* 防溢出；真实固件远小于此 */
        }
        sz = (sz * 10u) + (uint32_t)(data[i] - '0');
        i++;
    }
    y->file.size = sz;
}

static void handle_header(ymodem_recv_t *y, const uint8_t *data, uint32_t dlen)
{
    parse_header(y, data, dlen);

    /* 先问上层收不收，**再**回 ACK —— ACK 一出去，对端就开始灌数据了。
     * 「目标区放不下」这类判断必须在这里做。 */
    if (y->on_header != NULL && y->on_header(y->user, &y->file) != 0) {
        recv_fail(y, YMODEM_ERR_NOSPACE);
        return;
    }

    y->has_header = 1u;
    y->has_last   = 0u;
    y->st         = YMODEM_RECV_DATA;

    send_resp(y, YMODEM_ACK);
    recv_touch(y);
    ev_recv(y, YMODEM_EV_HEADER, y->file.size);
}

static void handle_data(ymodem_recv_t *y, uint8_t seq, const uint8_t *data, uint32_t dlen)
{
    uint32_t valid;

    if (y->st != YMODEM_RECV_DATA) {
        send_resp(y, YMODEM_NAK);        /* 头包还没来就来数据包 */
        return;
    }

    /* 重复包：ACK 丢了、对端重发同一包。这是协议的正常行为，
     * 重新 ACK 即可，**绝不能重复写数据**。 */
    if (y->has_last != 0u && seq == y->last_seq) {
        y->dup++;
        ev_recv(y, YMODEM_EV_DUP, seq);
        send_resp(y, YMODEM_ACK);
        recv_touch(y);
        return;
    }

    /* 只认「上一包序号 +1」（255 → 0 的回绕正好也满足 +1） */
    if (y->has_last != 0u) {
        if (seq != (uint8_t)(y->last_seq + 1u)) {
            send_resp(y, YMODEM_NAK);
            return;
        }
    } else if (seq != 1u) {
        send_resp(y, YMODEM_NAK);        /* 第一个数据包必须是 1 */
        return;
    }

    /* 末包会用 0x1A 填充，声明大小之外的部分不能交给上层 */
    valid = dlen;
    if (y->file.size != 0u) {
        uint32_t left = y->file.size - y->got;

        if (valid > left) {
            valid = left;
        }
    }

    if (valid > 0u && y->on_data != NULL && y->on_data(y->user, data, valid) != 0) {
        recv_fail(y, YMODEM_ERR_USER);
        return;
    }

    y->got     += valid;
    y->last_seq = seq;
    y->has_last = 1u;
    y->pkts++;

    send_resp(y, YMODEM_ACK);
    recv_touch(y);
    ev_recv(y, YMODEM_EV_PACKET, valid);
}

static void handle_eot(ymodem_recv_t *y)
{
    if (y->has_header == 0u) {
        /* 还没收头包就 EOT（对端在取消自己的发送）。回 ACK 让它早点收场。 */
        send_resp(y, YMODEM_ACK);
        recv_touch(y);
        return;
    }

    send_resp(y, YMODEM_ACK);

    if (y->st == YMODEM_RECV_DATA) {
        y->st = YMODEM_RECV_FINAL;       /* 等结束用的那个空第 0 包 */
        recv_touch(y);
        return;
    }
    if (y->st == YMODEM_RECV_FINAL) {
        /* 对端没发结束包就直接又发了一个 EOT —— 不少 PC 端脚本这么写。
         * 宽容处理：认为传输已经结束（标准流程的收尾包是空第 0 包，见 on_frame）。 */
        y->st = YMODEM_RECV_DONE;
        ev_recv(y, YMODEM_EV_DONE, y->got);
    }
}

static void on_frame(ymodem_recv_t *y)
{
    const uint8_t *data = (const uint8_t *)0;
    uint32_t       dlen = 0u;
    uint8_t        seq  = 0u;

    if (ymodem_check_frame(y->frame, y->expect, &seq, &data, &dlen) != YMODEM_OK) {
        send_resp(y, YMODEM_NAK);        /* 坏包就 NAK，对端会重发 */
        return;
    }

    if (y->has_header == 0u && seq == 0u) {
        handle_header(y, data, dlen);
        return;
    }
    if (y->st == YMODEM_RECV_FINAL && seq == 0u) {
        send_resp(y, YMODEM_ACK);        /* 空第 0 包：标准收尾 */
        y->st = YMODEM_RECV_DONE;
        ev_recv(y, YMODEM_EV_DONE, y->got);
        return;
    }

    handle_data(y, seq, data, dlen);
}

/** @brief 把此刻已到达的字节全部吃掉（不等待） */
static void recv_feed(ymodem_recv_t *y)
{
    uint8_t b;

    while (y->st != YMODEM_RECV_DONE && y->st != YMODEM_RECV_FAILED) {
        if (y->io->read_byte(y->io->ctx, &b) != 1) {
            break;
        }

        if (y->fill == 0u) {
            /* 帧首字节：决定这一帧有多长，或者干脆是个单字节控制符 */
            if (b == YMODEM_EOT) {
                handle_eot(y);
                continue;
            }
            if (b == YMODEM_CAN) {
                y->can_cnt++;
                if (y->can_cnt >= 2u) {
                    recv_fail(y, YMODEM_ERR_ABORT);
                    return;
                }
                continue;
            }
            if (b == YMODEM_ABORT1 || b == YMODEM_ABORT2) {
                recv_fail(y, YMODEM_ERR_ABORT);
                return;
            }
            if (b == YMODEM_SOH) {
                y->expect = YMODEM_OVERHEAD + YMODEM_DATA_128;
            } else if (b == YMODEM_STX) {
                y->expect = YMODEM_OVERHEAD + YMODEM_DATA_1K;
            } else {
                continue;                /* 空闲噪声（含握手期的 ACK/NAK 残渣）：丢弃 */
            }
        }

        y->can_cnt = 0u;
        y->frame[y->fill++] = b;

        if (y->fill == y->expect) {
            on_frame(y);
            y->fill   = 0u;
            y->expect = 0u;
            /* 一次 process 只处理一个完整帧就返回。on_data 拿到的指针指向本帧缓冲，
             * 上层（如 ota_src_uart）要它活到下一次 process 之前；一次多吞几帧的
             * 吞吐收益，抵不上「数据被下一帧覆盖」的风险。 */
            return;
        }
    }
}

/** @brief 超时重发「上一次的响应」——重发 ACK/NAK，而不是无脑发 NAK */
static void recv_resend(ymodem_recv_t *y)
{
    if (y->st == YMODEM_RECV_HANDSHAKE || y->last_resp == 0u) {
        send_resp(y, YMODEM_C);          /* 握手期：继续催 'C' */
        return;
    }
    (void)y->io->write(y->io->ctx, &y->last_resp, 1u);
}

int ymodem_recv_start(ymodem_recv_t *y, const ymodem_io_t *io, void *user,
                      int (*on_header)(void *user, const ymodem_file_t *f),
                      int (*on_data)(void *user, const uint8_t *data, uint32_t len),
                      void (*on_event)(void *user, ymodem_event_t ev, uint32_t arg))
{
    if (y == NULL || io == NULL ||
        io->read_byte == NULL || io->write == NULL || io->tick == NULL) {
        return YMODEM_ERR_PARAM;
    }

    (void)memset(y, 0u, sizeof(*y));
    y->io         = io;
    y->user       = user;
    y->on_header  = on_header;
    y->on_data    = on_data;
    y->on_event   = on_event;
    y->retry_max  = YMODEM_DEF_RETRY;
    y->timeout_ms = YMODEM_DEF_TIMEOUT_MS;
    y->st         = YMODEM_RECV_HANDSHAKE;
    y->err        = YMODEM_OK;

    recv_touch(y);
    ev_recv(y, YMODEM_EV_START, 0u);
    send_resp(y, YMODEM_C);              /* 起头：告诉对端「用 CRC16，开始」 */
    return YMODEM_OK;
}

void ymodem_recv_set_opts(ymodem_recv_t *y, uint8_t retry_max, uint16_t timeout_ms)
{
    if (y == NULL) {
        return;
    }
    if (retry_max != 0u) {
        y->retry_max = retry_max;
    }
    if (timeout_ms != 0u) {
        y->timeout_ms = timeout_ms;
    }
}

int ymodem_recv_process(ymodem_recv_t *y)
{
    if (y == NULL || y->io == NULL) {
        return YMODEM_ERR_PARAM;
    }
    if (y->st == YMODEM_RECV_DONE) {
        return YMODEM_OK;
    }
    if (y->st == YMODEM_RECV_FAILED) {
        return y->err;
    }

    /* ★ 半帧保护：帧收到一半就断了（对端重发被打断 / 应用长时间阻塞导致丢字节 /
     *   流被拼接），残帧留在缓冲里会让后面每个字节都错位 —— 现象是「一直 NAK，
     *   直到重试用尽」。超过一个帧间隙还没补齐，就丢掉残帧并 NAK，让对端重发整帧。
     *   NAK（而不是重发上次的 ACK）很关键：重发 ACK 会让对端以为这一包已收下，
     *   于是数据被静默丢掉。 */
    if (y->fill != 0u &&
        (uint32_t)(y->io->tick(y->io->ctx) - y->t_ref) >= YMODEM_INFRAME_GAP_MS) {
        y->fill   = 0u;
        y->expect = 0u;
        send_resp(y, YMODEM_NAK);
        y->t_ref = y->io->tick(y->io->ctx);   /* 给对端一个完整的重发窗口 */
    }

    recv_feed(y);
    if (y->st == YMODEM_RECV_DONE) {
        return YMODEM_OK;
    }
    if (y->st == YMODEM_RECV_FAILED) {
        return y->err;
    }

    if ((uint32_t)(y->io->tick(y->io->ctx) - y->t_ref) >= y->timeout_ms) {
        y->retry++;
        if (y->retry > y->retry_max) {
            recv_fail(y, YMODEM_ERR_TIMEOUT);
            return y->err;
        }
        ev_recv(y, YMODEM_EV_RETRY, y->retry);
        recv_resend(y);
        y->t_ref = y->io->tick(y->io->ctx);
    }

    return YMODEM_BUSY;
}

void ymodem_recv_abort(ymodem_recv_t *y)
{
    if (y == NULL || y->io == NULL) {
        return;
    }
    if (y->st == YMODEM_RECV_DONE || y->st == YMODEM_RECV_FAILED) {
        return;
    }
    recv_fail(y, YMODEM_ERR_ABORT);
}

const char *ymodem_recv_state_name(ymodem_recv_state_t s)
{
    switch (s) {
    case YMODEM_RECV_IDLE:      return "idle";
    case YMODEM_RECV_HANDSHAKE: return "handshake";
    case YMODEM_RECV_HEADER:    return "header";
    case YMODEM_RECV_DATA:      return "data";
    case YMODEM_RECV_FINAL:     return "final";
    case YMODEM_RECV_DONE:      return "done";
    case YMODEM_RECV_FAILED:    return "failed";
    default:                    return "?";
    }
}

/* ============================================================
 *  发送侧
 * ============================================================ */

static void ev_send(ymodem_send_t *y, ymodem_event_t e, uint32_t arg)
{
    if (y->on_event != NULL) {
        y->on_event(y->user, e, arg);
    }
}

static void send_touch(ymodem_send_t *y)
{
    y->t_ref = y->io->tick(y->io->ctx);
    y->retry = 0u;
}

static void send_fail(ymodem_send_t *y, ymodem_ret_t e)
{
    uint8_t can[2];

    y->st  = YMODEM_SEND_FAILED;
    y->err = e;

    can[0] = YMODEM_CAN;
    can[1] = YMODEM_CAN;
    (void)y->io->write(y->io->ctx, can, 2u);
}

static int write_frame(ymodem_send_t *y)
{
    return (y->io->write(y->io->ctx, y->frame, y->frame_len) == 0) ? 0 : -1;
}

static int send_eot(ymodem_send_t *y)
{
    uint8_t e = YMODEM_EOT;

    return (y->io->write(y->io->ctx, &e, 1u) == 0) ? 0 : -1;
}

/** @brief 组并发出头包（固定 128 字节，与数据包长度无关） */
static int send_header_packet(ymodem_send_t *y)
{
    uint8_t *d = y->frame + YM_DATA_OFF;
    uint32_t n = 0u;
    uint32_t v;
    char     num[16];
    uint32_t k = 0u;

    (void)memset(d, 0, YMODEM_DATA_128);

    if (y->file_name != NULL) {
        while (y->file_name[n] != '\0' && n < (YMODEM_DATA_128 - 2u)) {
            d[n] = (uint8_t)y->file_name[n];
            n++;
        }
    }
    n++;                                  /* 文件名结束的 '\0' */

    v = y->file_size;
    if (v == 0u) {
        num[k++] = '0';
    } else {
        char rev[16];
        uint32_t m = 0u;

        while (v > 0u) {
            rev[m++] = (char)('0' + (v % 10u));
            v /= 10u;
        }
        while (m > 0u) {
            num[k++] = rev[--m];
        }
    }
    for (uint32_t i = 0u; i < k && (n + 1u) < YMODEM_DATA_128; i++) {
        d[n++] = (uint8_t)num[i];
    }

    y->frame_len = ymodem_build_frame(y->frame, 0u, d, YMODEM_DATA_128);
    return write_frame(y);
}

/** @brief 取新数据、组帧并发出（一包） */
static int send_next_data_packet(ymodem_send_t *y)
{
    uint32_t want = (y->use_1k != 0u) ? YMODEM_DATA_1K : YMODEM_DATA_128;
    uint8_t *d    = y->frame + YM_DATA_OFF;
    uint32_t got  = 0u;

    if (y->on_pull != NULL && y->on_pull(y->user, d, want, &got) != 0) {
        send_fail(y, YMODEM_ERR_USER);
        return -1;
    }
    if (got > want) {
        got = want;
    }
    if (got < want) {
        (void)memset(d + got, YMODEM_PAD_BYTE, want - got);   /* 末包填充 */
        y->last_pkt_eot = 1u;
    } else {
        y->last_pkt_eot = 0u;
    }

    y->frame_len = ymodem_build_frame(y->frame, y->seq, d, want);
    y->seq++;
    y->sent += got;

    if (write_frame(y) != 0) {
        send_fail(y, YMODEM_ERR_IO);
        return -1;
    }
    ev_send(y, YMODEM_EV_PACKET, got);
    return 0;
}

/** @brief 发结束用的空第 0 包 */
static int send_final_packet(ymodem_send_t *y)
{
    uint8_t *d = y->frame + YM_DATA_OFF;

    (void)memset(d, 0, YMODEM_DATA_128);
    y->frame_len = ymodem_build_frame(y->frame, 0u, d, YMODEM_DATA_128);
    return write_frame(y);
}

/** @brief 超时/NAK 时原样重发当前这一步的东西（**不能重新取数据**，否则数据会跳） */
static void send_resend(ymodem_send_t *y)
{
    switch (y->st) {
    case YMODEM_SEND_HEADER:
    case YMODEM_SEND_DATA:
    case YMODEM_SEND_FINAL:
        (void)write_frame(y);
        break;
    case YMODEM_SEND_EOT:
        (void)send_eot(y);
        break;
    default:
        break;
    }
}

static void send_advance(ymodem_send_t *y)
{
    switch (y->st) {
    case YMODEM_SEND_HEADER:
        y->st = YMODEM_SEND_DATA;
        (void)send_next_data_packet(y);
        break;
    case YMODEM_SEND_DATA:
        if (y->last_pkt_eot != 0u) {
            y->st = YMODEM_SEND_EOT;
            (void)send_eot(y);
        } else {
            (void)send_next_data_packet(y);
        }
        break;
    case YMODEM_SEND_EOT:
        y->st = YMODEM_SEND_FINAL;
        (void)send_final_packet(y);
        break;
    case YMODEM_SEND_FINAL:
        y->st = YMODEM_SEND_DONE;
        ev_send(y, YMODEM_EV_DONE, y->sent);
        break;
    default:
        break;
    }
}

/** @brief 吃掉对端的响应字节 */
static void send_poll_resp(ymodem_send_t *y)
{
    uint8_t b;

    while (y->st != YMODEM_SEND_DONE && y->st != YMODEM_SEND_FAILED) {
        if (y->io->read_byte(y->io->ctx, &b) != 1) {
            return;
        }

        if (b == YMODEM_CAN) {
            y->can_cnt++;
            if (y->can_cnt >= 2u) {
                send_fail(y, YMODEM_ERR_ABORT);
                return;
            }
            continue;
        }
        if (b == YMODEM_ABORT1 || b == YMODEM_ABORT2) {
            send_fail(y, YMODEM_ERR_ABORT);
            return;
        }
        y->can_cnt = 0u;

        if (y->st == YMODEM_SEND_WAIT_C) {
            if (b == YMODEM_C) {
                if (send_header_packet(y) != 0) {
                    send_fail(y, YMODEM_ERR_IO);
                    return;
                }
                y->st = YMODEM_SEND_HEADER;
                send_touch(y);
                ev_send(y, YMODEM_EV_HEADER, y->file_size);
            }
            continue;                    /* 其它字节（'C' 之外的噪声）忽略 */
        }

        if (b == YMODEM_ACK) {
            send_touch(y);
            send_advance(y);
            continue;                    /* 可能对端已经把下一个 ACK 也发来了 */
        }
        if (b == YMODEM_NAK || b == YMODEM_C) {
            send_resend(y);              /* 对端没收到 → 原样重发 */
            y->t_ref = y->io->tick(y->io->ctx);
        }
    }
}

int ymodem_send_start(ymodem_send_t *y, const ymodem_io_t *io, void *user,
                      const char *file_name, uint32_t file_size,
                      int (*on_pull)(void *user, uint8_t *buf, uint32_t len, uint32_t *got),
                      void (*on_event)(void *user, ymodem_event_t ev, uint32_t arg))
{
    if (y == NULL || io == NULL || file_name == NULL ||
        io->read_byte == NULL || io->write == NULL || io->tick == NULL) {
        return YMODEM_ERR_PARAM;
    }

    (void)memset(y, 0u, sizeof(*y));
    y->io         = io;
    y->user       = user;
    y->on_pull    = on_pull;
    y->on_event   = on_event;
    y->file_name  = file_name;
    y->file_size  = file_size;
    y->use_1k     = 1u;
    y->retry_max  = YMODEM_DEF_RETRY;
    y->timeout_ms = YMODEM_DEF_TIMEOUT_MS;
    y->seq        = 1u;                  /* 数据包序号从 1 起（0 留给头包） */
    y->st         = YMODEM_SEND_WAIT_C;
    y->err        = YMODEM_OK;

    send_touch(y);
    ev_send(y, YMODEM_EV_START, 0u);
    return YMODEM_OK;
}

void ymodem_send_set_opts(ymodem_send_t *y, uint8_t use_1k, uint8_t retry_max, uint16_t timeout_ms)
{
    if (y == NULL) {
        return;
    }
    y->use_1k = (use_1k != 0u) ? 1u : 0u;
    if (retry_max != 0u) {
        y->retry_max = retry_max;
    }
    if (timeout_ms != 0u) {
        y->timeout_ms = timeout_ms;
    }
}

int ymodem_send_process(ymodem_send_t *y)
{
    if (y == NULL || y->io == NULL) {
        return YMODEM_ERR_PARAM;
    }
    if (y->st == YMODEM_SEND_DONE) {
        return YMODEM_OK;
    }
    if (y->st == YMODEM_SEND_FAILED) {
        return y->err;
    }

    send_poll_resp(y);
    if (y->st == YMODEM_SEND_DONE) {
        return YMODEM_OK;
    }
    if (y->st == YMODEM_SEND_FAILED) {
        return y->err;
    }

    if ((uint32_t)(y->io->tick(y->io->ctx) - y->t_ref) >= y->timeout_ms) {
        y->retry++;
        if (y->retry > y->retry_max) {
            send_fail(y, YMODEM_ERR_TIMEOUT);
            return y->err;
        }
        ev_send(y, YMODEM_EV_RETRY, y->retry);
        send_resend(y);
        y->t_ref = y->io->tick(y->io->ctx);
    }

    return YMODEM_BUSY;
}

void ymodem_send_abort(ymodem_send_t *y)
{
    if (y == NULL || y->io == NULL) {
        return;
    }
    if (y->st == YMODEM_SEND_DONE || y->st == YMODEM_SEND_FAILED) {
        return;
    }
    send_fail(y, YMODEM_ERR_ABORT);
}

const char *ymodem_send_state_name(ymodem_send_state_t s)
{
    switch (s) {
    case YMODEM_SEND_IDLE:     return "idle";
    case YMODEM_SEND_WAIT_C:   return "wait-c";
    case YMODEM_SEND_HEADER:   return "header";
    case YMODEM_SEND_DATA:     return "data";
    case YMODEM_SEND_EOT:      return "eot";
    case YMODEM_SEND_FINAL:    return "final";
    case YMODEM_SEND_DONE:     return "done";
    case YMODEM_SEND_FAILED:   return "failed";
    default:                   return "?";
    }
}
