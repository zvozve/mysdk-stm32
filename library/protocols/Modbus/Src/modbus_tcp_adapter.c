#include "modbus_core.h"
#include "modbus_tcp_adapter.h"

#if MODBUS_ENABLE_TCP

#include "SEGGER_RTT_Log.h"
#include <string.h>

/* 本文件是 LwIP netconn 适配层：所有 netconn_* / pbuf 操作集中于此，
 * 协议层(modbus_tcp.c)通过 tcp_driver_t 抽象接口调用，永不见 LwIP 类型。
 * 镜像 UART 的 modbus_uart_adapter.c 设计。 */

// ===========================
// tcp_driver_t 的 netconn 实现
// ===========================

/* client：创建 socket + 发起非阻塞 connect（立即返回，绝不阻塞） */
static void *netconn_client_open(const char *ip, uint16_t port) {
    struct netconn *nc = netconn_new(NETCONN_TCP);
    if (!nc) return NULL;
    netconn_set_nonblocking(nc, 1);   /* ★ 必须在 connect 之前，否则 connect 阻塞 */
    ip_addr_t ipaddr;
    if (!ipaddr_aton(ip, &ipaddr)) {
        netconn_delete(nc);
        return NULL;
    }
    err_t err = netconn_connect(nc, &ipaddr, port);
    /* 非阻塞 connect：ERR_OK/INPROGRESS/ALREADY 均表示"已发起，待握手" */
    if (err == ERR_OK || err == ERR_INPROGRESS || err == ERR_ALREADY) {
        return nc;
    }
    netconn_close(nc);
    netconn_delete(nc);
    return NULL;
}

/* client：轮询非阻塞 connect 状态
 * 返回 1=已连上, 0=进行中, -1=失败 */
static int netconn_client_poll(void *conn) {
    struct netconn *nc = (struct netconn *)conn;
    if (!nc) return -1;
    if (nc->state == NETCONN_CONNECT) {
        return 0;   /* 仍在握手 */
    }
    /* 握手已结束（state!=NETCONN_CONNECT）：成功则 peer 可读，失败则不可读。
     * netconn_peer 通过 tcpip 线程处理，只等消息往返（微秒级），不依赖网络。 */
    ip_addr_t pa;
    u16_t     pp;
    if (netconn_peer(nc, &pa, &pp) == ERR_OK) {
        return 1;   /* connected */
    }
    return -1;     /* failed: PCB 已关闭/不存在 */
}

/* server：创建+bind+listen，返回非阻塞监听句柄 */
static void *netconn_server_listen(uint16_t port) {
    struct netconn *ln = netconn_new(NETCONN_TCP);
    if (!ln) return NULL;
    if (netconn_bind(ln, IP_ADDR_ANY, port) != ERR_OK) {
        netconn_delete(ln);
        return NULL;
    }
    if (netconn_listen(ln) != ERR_OK) {
        netconn_delete(ln);
        return NULL;
    }
    netconn_set_nonblocking(ln, 1);   /* 非阻塞 accept */
    return ln;
}

/* server：非阻塞 accept，返回新连接句柄或 NULL(无挂起连接) */
static void *netconn_server_accept(void *listen, char *ip, uint16_t *port) {
    struct netconn *ln = (struct netconn *)listen;
    if (!ln) return NULL;
    struct netconn *nc = NULL;
    err_t err = netconn_accept(ln, &nc);
    if (err != ERR_OK || !nc) return NULL;
    netconn_set_nonblocking(nc, 1);   /* 新连接也设非阻塞 */
    /* 获取对端 IP+端口 */
    ip_addr_t peer;
    u16_t     peer_port;
    if (netconn_peer(nc, &peer, &peer_port) == ERR_OK) {
        if (ip) ipaddr_ntoa_r(&peer, ip, 16);
        if (port) *port = peer_port;
    } else {
        if (ip) ip[0] = '\0';
        if (port) *port = 0;
    }
    return nc;
}

/* 发送：返回 0=成功, -1=失败 */
static int netconn_send_op(void *conn, const uint8_t *data, uint16_t len) {
    struct netconn *nc = (struct netconn *)conn;
    if (!nc) return -1;
    size_t written = 0;
    /* 非阻塞连接必须用 netconn_write_partly：netconn_write() 无法返回已写
     * 字节数，在非阻塞连接上直接返回 ERR_VAL（api_lib.c:1014-1018）。 */
    err_t err = netconn_write_partly(nc, data, len, NETCONN_COPY, &written);
    if (err != ERR_OK || written != len) return -1;
    return 0;
}

/* 接收：非阻塞，把 netbuf 的 pbuf 链展平到 buf
 * 返回 1=有数据, 0=无数据, -1=对端关闭 */
static int netconn_recv_op(void *conn, uint8_t *buf, uint16_t *len) {
    struct netconn *nc = (struct netconn *)conn;
    if (!nc || !buf || !len) return -1;
    uint16_t cap = *len;
    *len = 0;
    struct netbuf *nbuf = NULL;
    err_t err = netconn_recv(nc, &nbuf);
    if (err == ERR_OK && nbuf) {
        uint16_t copied = 0;
        for (struct pbuf *p = nbuf->p; p != NULL && copied < cap; p = p->next) {
            uint16_t remain = (uint16_t)(cap - copied);
            uint16_t n = (p->len > remain) ? remain : (uint16_t)p->len;
            if (n > 0) {
                memcpy(buf + copied, p->payload, n);
                copied += n;
            }
        }
        netbuf_delete(nbuf);
        *len = copied;
        return (copied > 0) ? 1 : 0;
    } else if (err == ERR_CLSD) {
        return -1;   /* 对端关闭 */
    }
    /* ERR_WOULDBLOCK / 其他 → 无数据 */
    return 0;
}

/* 关闭并释放连接 */
static void netconn_close_op(void *conn) {
    struct netconn *nc = (struct netconn *)conn;
    if (!nc) return;
    netconn_close(nc);
    netconn_delete(nc);
}

/* 全局唯一的 netconn driver 实例（协议层通过指针引用，不拷贝） */
static const tcp_driver_t g_netconn_driver = {
    .client_open    = netconn_client_open,
    .client_poll    = netconn_client_poll,
    .server_listen  = netconn_server_listen,
    .server_accept  = netconn_server_accept,
    .send           = netconn_send_op,
    .recv           = netconn_recv_op,
    .close          = netconn_close_op,
};

// ===========================
// 适配器入口（镜像 modbus_uart_adapter_init 的注册式接口）
// ===========================

int modbus_tcp_adapter_client_init(modbus_t *mb, modbus_tcp_ctx_t *tcp,
                                    const char *ip_str, uint16_t port) {
    if (!mb || !tcp || !ip_str) return -1;
    memset(tcp, 0, sizeof(*tcp));

    strncpy(tcp->remote_ip_str, ip_str, sizeof(tcp->remote_ip_str) - 1);
    tcp->remote_ip_str[sizeof(tcp->remote_ip_str) - 1] = '\0';

    tcp->mb = mb;
    tcp->is_server = 0;
    tcp->port = port;

    /* 注入 netconn driver + 绑定 transport 回调（协议层不感知 netconn） */
    modbus_tcp_attach_transport(mb, tcp, &g_netconn_driver);

    MODBUS_LOG("[TCP] client target %s:%u (master, target unit=%u)",
               ip_str, port, mb->slave_addr);
    return 0;
}

int modbus_tcp_adapter_server_init(modbus_t *mb, modbus_tcp_server_t *srv,
                                    uint16_t port) {
    if (!mb || !srv) return -1;
    memset(srv, 0, sizeof(*srv));

    srv->driver = &g_netconn_driver;
    srv->listen_conn = srv->driver->server_listen(port);
    if (!srv->listen_conn) return -1;

    srv->port = port;
    srv->slave_tpl = mb;
    for (int i = 0; i < MODBUS_TCP_MAX_CLIENTS; i++) {
        modbus_tcp_slot_init(&srv->slots[i], mb, srv->driver);
    }

    MODBUS_LOG("[TCP] multi-client server listening on :%u (unit=%u, max=%d)",
               port, mb->slave_addr, MODBUS_TCP_MAX_CLIENTS);
    return 0;
}

#endif /* MODBUS_ENABLE_TCP */
