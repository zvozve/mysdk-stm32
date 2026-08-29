#include "modbus_core.h"    // 提供 MODBUS_ENABLE_TCP（默认 0，可被 CMake -D 覆盖）
#include "modbus_tcp.h"

#if MODBUS_ENABLE_TCP

/* 诊断日志开关：1=打印每个 slot 的 recv/send 详情（定位多客户端哪个连接卡住），
 * 实测稳定后可改 0 关闭以减少刷屏。 */
#ifndef MODBUS_TCP_DIAG
#define MODBUS_TCP_DIAG 1
#endif

/* 非阻塞 connect 超时：对端不回握手时超过此时长即放弃本次连接并重试，
 * 绝不阻塞主循环（原阻塞 connect 在目标不可达时会永久挂起 → IWDG 复位）。 */
#ifndef MB_TCP_CONNECT_TIMEOUT_MS
#define MB_TCP_CONNECT_TIMEOUT_MS  3000
#endif

#include "SEGGER_RTT_Log.h"
#include <string.h>

/* 本文件不 include 任何 lwip 头；网络栈调用全部经 t->driver->xxx 抽象接口，
 * 具体 netconn 实现在 modbus_tcp_adapter.c 中。 */

// ===========================
// MBAP 帧封装（纯字节操作，不依赖任何网络类型）
// ===========================

/* core 视角 PDU 恒为 [unit][func][data...]：
 *  - TX: pdu[0]=unit → MBAP 第 6 字节，pdu[1..]=func+data 顺延；
 *  - RX: 校验后把 MBAP 的 unit 支到 raw[0]，PDU 顺延，供 core 直接解析。
 * 返回：TX 输出长度 = pdu_len + 6（MBAP 7 字节中 unit 与 PDU 共用 1 字节） */
uint16_t tcp_frame_tx(void *ctx, const uint8_t *pdu, uint16_t pdu_len,
                      uint8_t *out, uint16_t out_cap) {
    modbus_tcp_ctx_t *t = (modbus_tcp_ctx_t*)ctx;
    if (!t || !pdu || !out || pdu_len < 2 || pdu_len > MODBUS_BUF_SIZE) return 0;
    if (pdu_len + 6 > out_cap) return 0;

    uint16_t tid = t->is_server ? t->last_rx_tid : t->next_tx_tid++;
    out[0] = (tid >> 8) & 0xFF;
    out[1] = tid & 0xFF;
    out[2] = 0x00;               /* protocol id 高 */
    out[3] = 0x00;               /* protocol id 低（Modbus 恒为 0） */
    out[4] = (pdu_len >> 8) & 0xFF;   /* length = unit(1) + PDU */
    out[5] = pdu_len & 0xFF;
    out[6] = pdu[0];             /* unit id */
    memcpy(out + 7, pdu + 1, pdu_len - 1);
    return pdu_len + 6;
}

int tcp_frame_rx(void *ctx, uint8_t *raw, uint16_t *raw_len) {
    modbus_tcp_ctx_t *t = (modbus_tcp_ctx_t*)ctx;
    if (!t || !raw || !raw_len || *raw_len < 8) return -1;

    uint16_t pid       = ((uint16_t)raw[2] << 8) | raw[3];
    uint16_t len_field = ((uint16_t)raw[4] << 8) | raw[5];
    if (pid != 0) return -1;
    if (len_field != *raw_len - 6) return -1;   /* length = unit(1) + PDU = 总长 - 6 */

    uint16_t rx_tid = ((uint16_t)raw[0] << 8) | raw[1];
    if (!t->is_server && rx_tid != t->next_tx_tid - 1) return -1;  /* 丢弃过期/错配响应 */
    t->last_rx_tid = rx_tid;                    /* server: 应答回显用 */

    /* 重排为 core 布局 [unit][func][data...] */
    raw[0] = raw[6];
    memmove(raw + 1, raw + 7, *raw_len - 7);
    *raw_len -= 6;
    return 0;
}

// ===========================
// transport 回调（经 driver 抽象，不直接调 netconn）
// ===========================

static int tcp_send(void *ctx, const uint8_t *data, uint16_t len) {
    modbus_tcp_ctx_t *t = (modbus_tcp_ctx_t*)ctx;
    if (!t || !t->is_connected || !t->conn || !t->driver) {
        return -1;   /* not connected：静默丢弃，避免未连接期高频打印阻塞主循环/喂狗 */
    }
    HEX_LOG("TCP-TX: ", data, len);   /* 实际交给 driver 的 MBAP 帧 */
    int r = t->driver->send(t->conn, data, len);
    if (r != 0) {
        MODBUS_LOG("[TCP] slot%d send err (len=%u)",
                   (t->slot_id >= 0 ? t->slot_id : -1), len);
        return -1;
    }
    return 0;
}

static uint16_t tcp_peek(void *ctx) {
    modbus_tcp_ctx_t *t = (modbus_tcp_ctx_t*)ctx;
    return t->ready_len;
}

static uint16_t tcp_recv(void *ctx, uint8_t *buf, uint16_t len) {
    modbus_tcp_ctx_t *t = (modbus_tcp_ctx_t*)ctx;
    uint16_t n = (t->ready_len < len) ? t->ready_len : len;
    if (n > 0) {
        memcpy(buf, t->ready, n);
        t->ready_len = 0;
    }
    return n;
}

static uint32_t tcp_get_tick(void) { return MB_GET_TICK(); }

static void tcp_delay(uint32_t ms) { MB_Delay_ms(ms); }

/* 把 driver 收到的数据追加进 accum（溢出则整体丢弃重新同步） */
static void tcp_accumulate(modbus_tcp_ctx_t *t, const uint8_t *data, uint16_t len) {
    if (t->accum_len + len <= sizeof(t->accum)) {
        memcpy(t->accum + t->accum_len, data, len);
        t->accum_len += len;
    } else {
        t->accum_len = 0;   /* 协议异常/洪泛 → 重新同步 */
        MODBUS_LOG("[TCP] accum overflow, resync");
    }
}

/* 从 accum 切出一帧完整帧到 ready（每轮询节拍最多一帧，core 消费后下次再切） */
static void tcp_extract_frame(modbus_tcp_ctx_t *t) {
    if (t->ready_len != 0 || t->accum_len < 6) return;
    uint16_t len_field = ((uint16_t)t->accum[4] << 8) | t->accum[5];
    uint16_t frame_total = 6 + len_field;
    if (frame_total > MODBUS_BUF_SIZE) {        /* 长度字段非法 → 丢整段重同步 */
        t->accum_len = 0;
        MODBUS_LOG("[TCP] bogus length %u, resync", frame_total);
        return;
    }
    if (t->accum_len < frame_total) return;     /* 半帧，等后续数据 */
    memcpy(t->ready, t->accum, frame_total);
    t->ready_len = frame_total;
    memmove(t->accum, t->accum + frame_total, t->accum_len - frame_total);
    t->accum_len -= frame_total;
}

/* ---- 连接断开的统一清理（client / slot 共用）---- */
static void tcp_conn_closed(modbus_tcp_ctx_t *t, const char *reason) {
    MODBUS_LOG("[TCP] %s", reason ? reason : "connection closed");
    if (t->conn && t->driver) t->driver->close(t->conn);
    t->conn = NULL;
    t->is_connected = 0;
    t->connecting = 0;
    t->accum_len = 0;
    t->ready_len = 0;
    /* 对端关闭 → 通知线路断开（参考 RTU 主机 on_line_break）。
     * 置 DISCONNECTED + 重置 reconnect_tick，避免 check_line_status 定时器在真正
     * 重连成功前误触发恢复通知；重连由 port_poll 按 2s 间隔驱动。 */
    t->mb->line_state = MODBUS_LINE_DISCONNECTED;
    t->mb->reconnect_tick = tcp_get_tick();
    if (t->mb->on_line_break) t->mb->on_line_break(t->mb);
}

static void tcp_port_poll(void *ctx) {
    modbus_tcp_ctx_t *t = (modbus_tcp_ctx_t*)ctx;
    uint32_t now = MB_GET_TICK();

    if (t->is_server) {
        /* ---- server（单连接模式）：非阻塞 accept ---- */
        if (!t->is_connected && t->listen_conn && t->driver) {
            char ip[16];
            uint16_t port;
            void *conn = t->driver->server_accept(t->listen_conn, ip, &port);
            if (conn) {
                t->conn = conn;
                t->is_connected = 1;
                t->accum_len = 0;
                t->ready_len = 0;
                MODBUS_LOG("[TCP] client connected");
            }
        }
    } else {
        /* ---- client：按 2s 间隔尝试(重)连接（非阻塞，绝不卡主循环）---- */
        if (!t->is_connected) {
            /* 未连接 → 线路视为断开：置 DISCONNECTED 让 master 仲裁器立即停发，
             * 避免"未连上却每 tick 空转发帧 → 刷爆 RTT 日志 → 主循环阻塞/复位"的自旋。
             * 同时持续刷新 core 的 reconnect_tick，压制 check_line_status 把
             * DISCONNECTED 误翻回 OK（否则会每隔 reconnect_interval 触发一次空发自旋）。 */
            if (t->mb->line_state != MODBUS_LINE_DISCONNECTED) {
                t->mb->line_state = MODBUS_LINE_DISCONNECTED;
                if (t->ever_connected && t->mb->on_line_break)
                    t->mb->on_line_break(t->mb);
            }
            t->mb->reconnect_tick = now;   /* 压制 core 自动恢复 */

            if (t->connecting) {
                /* 非阻塞 connect 进行中：轮询 driver 状态，全程不阻塞
                 * （原阻塞 connect 在目标不可达时会永久挂起 → IWDG 复位）。 */
                int r = t->driver->client_poll(t->conn);
                if (r == 1) {
                    /* 握手成功 → 标记连上 */
                    t->is_connected = 1;
                    t->connecting   = 0;
                    t->ever_connected = 1;
                    t->accum_len = 0;
                    t->ready_len = 0;
                    MODBUS_LOG("[TCP] connected to %s:%u", t->remote_ip_str, t->port);
                    if (t->mb->line_state == MODBUS_LINE_DISCONNECTED) {
                        t->mb->line_state = MODBUS_LINE_OK;
                        if (t->mb->on_line_recover) t->mb->on_line_recover(t->mb);
                    }
                } else if (r < 0) {
                    /* 握手失败 */
                    tcp_conn_closed(t, "connect failed");
                    t->connecting = 0;
                } else {
                    /* 仍在握手：超时才放弃，避免无谓等待 */
                    if (now - t->connect_start_tick >= MB_TCP_CONNECT_TIMEOUT_MS) {
                        tcp_conn_closed(t, "connect timeout");
                        t->connecting = 0;
                        MODBUS_LOG("[TCP] connect %s:%u timeout",
                                   t->remote_ip_str, t->port);
                    }
                }
                return;   /* 连接中/刚结束：本 tick 不做 recv */
            }

            /* 未发起连接 → 按 2s 间隔发起一次非阻塞 connect */
            if (now - t->reconnect_tick >= 2000) {
                t->reconnect_tick = now;
                void *conn = t->driver->client_open(t->remote_ip_str, t->port);
                if (conn) {
                    t->conn = conn;
                    t->connecting = 1;
                    t->connect_start_tick = now;
                }
            }
            return;
        }
    }

    if (!t->is_connected || !t->conn || !t->driver) return;

    /* ---- 非阻塞收包 ---- */
    uint8_t rbuf[MODBUS_BUF_SIZE + 8];
    uint16_t rlen = sizeof(rbuf);
    int r = t->driver->recv(t->conn, rbuf, &rlen);
    if (r > 0 && rlen > 0) {
        tcp_accumulate(t, rbuf, rlen);
    } else if (r < 0) {
        tcp_conn_closed(t, "connection closed by peer");
        return;
    }

    tcp_extract_frame(t);
}

// ===========================
// 多客户端 server：slot 轮询 / 空闲踢 / 初始化
// ===========================
/* slot 的 port_poll：只做 recv + 帧重组 + 对端关闭处理（不含 accept/connect）。
 * 连接由 server 容器的 modbus_tcp_server_process 统一 accept 后挂入 slot。 */
static void tcp_slot_port_poll(void *ctx) {
    modbus_tcp_ctx_t *t = (modbus_tcp_ctx_t*)ctx;
    if (!t->is_connected || !t->conn || !t->driver) return;

    uint8_t rbuf[MODBUS_BUF_SIZE + 8];
    uint16_t rlen = sizeof(rbuf);
    int r = t->driver->recv(t->conn, rbuf, &rlen);
    if (r > 0 && rlen > 0) {
        tcp_accumulate(t, rbuf, rlen);
        if (MODBUS_TCP_DIAG && t->slot_id >= 0)
            MODBUS_LOG("[TCP] slot%d recv %u B", t->slot_id, rlen);
    } else if (r < 0) {
        if (MODBUS_TCP_DIAG && t->slot_id >= 0)
            MODBUS_LOG("[TCP] slot%d closed by peer", t->slot_id);
        t->driver->close(t->conn);
        t->conn = NULL;
        t->is_connected = 0;
        t->accum_len = 0;
        t->ready_len = 0;
        return;
    }

    tcp_extract_frame(t);
}

/* slot 的 on_line_break：空闲超时（check_line_status 从机分支）触发 → 关闭本 slot socket。
 * 真正释放 slot + 上下线通知在 modbus_tcp_server_process 循环里统一做。 */
static void tcp_slot_line_break(modbus_t *ctx) {
    modbus_tcp_ctx_t *t = (modbus_tcp_ctx_t*)ctx->transport.ctx;
    if (t && t->conn && t->driver) {
        t->driver->close(t->conn);
    }
    if (t) { t->conn = NULL; t->is_connected = 0; }
}

/* 轻量初始化一个 slot（不注册进全局实例表，避免 modbus_process_all 重复处理）。
 * 共享模板的 data_map 指针（不拷贝数据，符合多主站网关语义）。
 * drv 由 server 容器（适配器）传入。 */
void modbus_tcp_slot_init(modbus_tcp_client_slot_t *s, const modbus_t *tpl,
                          const tcp_driver_t *drv) {
    memset(s, 0, sizeof(*s));
    modbus_t *mb = &s->mb;
    mb->slave_addr = tpl->slave_addr;
    mb->role       = MODBUS_ROLE_SLAVE;
    mb->mode       = MODBUS_MODE_TCP;
    mb->state      = MODBUS_STATE_IDLE;
    mb->line_state = MODBUS_LINE_OK;
    mb->response_timeout   = 1000;
    mb->slave_timeout_ms   = 5000;   /* 5s 无请求 → 踢下线（check_line_status 从机分支） */
    mb->max_timeout_count  = 3;
    mb->poll_interval      = 100;
    mb->reconnect_interval = 10000;
    mb->data_map = tpl->data_map;    /* 共享指针 */
    mb->on_master_reg_change  = tpl->on_master_reg_change;
    mb->on_master_coil_change = tpl->on_master_coil_change;
    mb->on_line_break = tcp_slot_line_break;

    modbus_tcp_ctx_t *t = &s->tcp;
    memset(t, 0, sizeof(*t));
    t->mb = mb;
    t->driver = drv;                 /* 注入 driver */
    t->slot_id = -1;                 /* accept 时再赋具体下标 */
    t->is_server = 1;                /* server 侧：应答回显请求 tid */
    modbus_transport_t tr = {
        .ctx       = t,
        .send      = tcp_send,
        .peek      = tcp_peek,
        .recv      = tcp_recv,
        .get_tick  = tcp_get_tick,
        .delay     = tcp_delay,
        .frame_tx  = tcp_frame_tx,
        .frame_rx  = tcp_frame_rx,
        .port_poll = tcp_slot_port_poll,
    };
    modbus_set_transport(mb, &tr);
    mb->last_activity_tick = tcp_get_tick();
}

// ===========================
// transport 绑定（协议层入口，适配器调用）
// ===========================

void modbus_tcp_attach_transport(modbus_t *mb, modbus_tcp_ctx_t *t,
                                 const tcp_driver_t *drv) {
    t->driver = drv;   /* 注入 driver：协议层经此调用所有网络操作 */
    modbus_transport_t tr = {
        .ctx       = t,
        .send      = tcp_send,
        .peek      = tcp_peek,
        .recv      = tcp_recv,
        .get_tick  = tcp_get_tick,
        .delay     = tcp_delay,
        .frame_tx  = tcp_frame_tx,
        .frame_rx  = tcp_frame_rx,
        .port_poll = tcp_port_poll,
    };
    modbus_set_transport(mb, &tr);
    mb->mode = MODBUS_MODE_TCP;
    /* 端口接管线路活性：TCP 连接的断开/恢复由 port_poll 直接反映，
     * 防止 core 在首次请求前因 last_activity_tick=0 误判从机断线 */
    mb->last_activity_tick = tcp_get_tick();
}

// ===========================
// 多客户端 server 容器驱动（协议层，经 driver 抽象 accept）
// ===========================

void modbus_tcp_server_process(modbus_tcp_server_t *srv) {
    if (!srv || !srv->listen_conn || !srv->driver) return;
    uint32_t now = MB_GET_TICK();

    /* 1. 批量 accept 所有挂起连接（nonblocking，直到无连接）。
     *    改为 while 循环：Modbus Poll 等工具常"同时"发起多连接，
     *    若每 tick 只 accept 一次，并发连接会积压甚至被丢弃（表现为第二个 client 超时）。 */
    char ip[16];
    uint16_t peer_port;
    void *conn = srv->driver->server_accept(srv->listen_conn, ip, &peer_port);
    while (conn) {
        int idx = -1;
        for (int i = 0; i < MODBUS_TCP_MAX_CLIENTS; i++) {
            if (!srv->slots[i].active) { idx = i; break; }
        }
        if (idx >= 0) {
            modbus_tcp_client_slot_t *s = &srv->slots[idx];
            s->tcp.conn = conn;
            s->tcp.is_connected = 1;
            s->tcp.slot_id = idx;
            s->tcp.accum_len = 0;
            s->tcp.ready_len = 0;
            s->tcp.last_rx_tid = 0;
            s->active = 1;
            s->mb.state = MODBUS_STATE_IDLE;
            s->mb.line_state = MODBUS_LINE_OK;
            s->mb.timeout_count = 0;
            s->mb.last_activity_tick = now;
            strncpy(s->ip, ip, sizeof(s->ip) - 1);
            s->ip[sizeof(s->ip) - 1] = '\0';
            MODBUS_LOG("[TCP] accept -> client %d (%s)", idx, s->ip);
            if (srv->on_client_connect) srv->on_client_connect(idx, s->ip);
        } else {
            MODBUS_LOG("[TCP] max clients reached, reject");
            srv->driver->close(conn);
        }
        ip[0] = '\0'; peer_port = 0;
        conn = srv->driver->server_accept(srv->listen_conn, ip, &peer_port);
    }

    /* 2. 逐 slot 处理；断开/空闲超时（on_line_break 已关 socket）则释放 + 通知 */
    for (int i = 0; i < MODBUS_TCP_MAX_CLIENTS; i++) {
        modbus_tcp_client_slot_t *s = &srv->slots[i];
        if (!s->active) continue;
        modbus_process(&s->mb);
        if (!s->tcp.is_connected) {
            MODBUS_LOG("[TCP] client %d disconnected", i);
            if (srv->on_client_disconnect) srv->on_client_disconnect(i, s->ip);
            s->active = 0;
            s->tcp.conn = NULL;
            s->ip[0] = '\0';
        }
    }
}

#endif /* MODBUS_ENABLE_TCP */
