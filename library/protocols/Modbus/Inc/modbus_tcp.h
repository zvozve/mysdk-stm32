#ifndef __MODBUS_TCP_H__
#define __MODBUS_TCP_H__

#include "modbus_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 本文件保持 LwIP-free：不 include 任何 lwip 头，不引用 netconn/pbuf 等类型。
 * 网络栈调用通过 tcp_driver_t 抽象接口注入（类比 UART 的 uart_drv_t），
 * 具体 netconn 实现在 modbus_tcp_adapter.c 中。 */

#if MODBUS_ENABLE_TCP

#define MODBUS_TCP_DEFAULT_PORT   502
#define MODBUS_TCP_MAX_CLIENTS    4   /* 同时在线客户端上限（N=4，按 RAM 调整） */

/* ===========================
 * tcp_driver_t — 抽象网络驱动接口（类比 UART 的 uart_drv_t）
 * 协议层只通过此接口操作网络，永不见 netconn/socket/pbuf 等类型。
 * 适配器（modbus_tcp_adapter.c）提供具体实现。 */
typedef struct tcp_driver_t {
    /* client 模式：创建 socket + 发起非阻塞 connect，返回连接句柄或 NULL */
    void* (*client_open)(const char *ip, uint16_t port);
    /* client 模式：轮询非阻塞 connect 状态
     * 返回 1=已连上, 0=进行中, -1=失败(连接已被关闭) */
    int   (*client_poll)(void *conn);
    /* server 模式：创建+bind+listen，返回监听句柄或 NULL */
    void* (*server_listen)(uint16_t port);
    /* server 模式：非阻塞 accept，返回新连接句柄或 NULL(无挂起连接)
     * 成功时填充 ip[] 和 *port */
    void* (*server_accept)(void *listen, char *ip, uint16_t *port);
    /* 发送：返回 0=成功(全部发出), -1=失败 */
    int   (*send)(void *conn, const uint8_t *data, uint16_t len);
    /* 接收：入口 *len=缓冲区容量；出口：
     *   返回 1, *len=实际字节数 → 有数据
     *   返回 0, *len=0 → 无数据(非阻塞, 稍后再来)
     *   返回 -1 → 对端关闭(调用方应 close) */
    int   (*recv)(void *conn, uint8_t *buf, uint16_t *len);
    /* 关闭并释放连接 */
    void  (*close)(void *conn);
} tcp_driver_t;

/* TCP 端口上下文（每实例一份）：
 *  - server(从机侧连接)：listen_conn 监听，accept 得 conn；应答回显请求 tid
 *  - client(主机侧连接)：conn 主动连接；每请求自增 tid 供应答匹配
 *  - 收包重组：TCP 是字节流，先累积到 accum，按 MBAP 长度字段切出完整帧
 *    放入 ready 单帧槽，core 的 peek/recv 从 ready 取；一次 TCP 段可能含多帧
 *    或半帧，port_poll 每个轮询节拍最多向 ready 投递一帧，天然逐帧串行。 */
typedef struct {
    modbus_t *mb;               /* 关联的 core 实例 */
    const tcp_driver_t *driver; /* 网络驱动（netconn 实现，由适配器注入） */
    void *conn;                 /* 活动连接句柄（server: accept 所得 / client: 已连接） */
    void *listen_conn;          /* server: 监听句柄 */
    uint16_t port;              /* server: 监听端口；client: 远端端口 */
    char remote_ip_str[16];     /* client: 远端 IP（可读字符串，重连时重新解析） */
    uint8_t is_server;          /* 1=server（从机侧），0=client（主机侧） */
    uint8_t is_connected;
    uint8_t ever_connected;      /* client: 是否曾成功连上过（用于断线通知去抖，避免上电误报 BREAK） */
    int     slot_id;             /* 多客户端 server 槽下标（-1=非 slot / client 模式），仅供诊断日志 */

    /* 事务 ID */
    uint16_t next_tx_tid;       /* client(master): 每发一请求自增 */
    uint16_t last_rx_tid;       /* server(slave): 应答回显请求的 tid */

    /* TCP 流重组 */
    uint8_t accum[MODBUS_BUF_SIZE + 8];
    uint16_t accum_len;
    uint8_t ready[MODBUS_BUF_SIZE + 8];
    uint16_t ready_len;

    uint32_t reconnect_tick;    /* client: 重连节拍 */
    uint8_t  connecting;        /* client: 非阻塞 connect 进行中（已发起，未握手完成） */
    uint32_t connect_start_tick;/* client: 本次 connect 发起时刻，用于连接超时判定 */
} modbus_tcp_ctx_t;

/* 多客户端 server 的客户端槽：内嵌一个完整 modbus_t（从机角色），
 * 所有 slot 共享模板同一份 data_map（网关模型：多主站连同一设备，寄存器后写覆盖）。
 * slot 的 transport 仅做 recv/帧重组/回显 tid，accept 由 server 容器统一驱动。 */
typedef struct {
    modbus_t  mb;            /* 完整实例，仅填从机相关字段 + 共享 data_map 指针 */
    modbus_tcp_ctx_t tcp;    /* 本 slot 的 conn + accum + ready（is_server=1，应答回显 tid） */
    uint8_t   active;        /* 1=已分配连接 */
    char      ip[16];        /* 对端 IP 字符串，供 on_client_* 回调 */
} modbus_tcp_client_slot_t;

/* 多客户端 server 容器：持有监听 socket + N 个客户端 slot；
 * accept / 空闲踢(5s) / 上下线通知 全部由 modbus_tcp_server_process() 统一驱动。 */
typedef struct {
    const tcp_driver_t *driver;              /* 网络驱动（由适配器注入） */
    void *listen_conn;                       /* 监听句柄 */
    uint16_t port;
    modbus_t *slave_tpl;                     /* 共享 data_map / slave_addr 的模板 */
    modbus_tcp_client_slot_t slots[MODBUS_TCP_MAX_CLIENTS];
    void (*on_client_connect)(int client_id, const char *ip);
    void (*on_client_disconnect)(int client_id, const char *ip);
} modbus_tcp_server_t;

/* ===========================
 * 协议层 API（LwIP-free，由适配器调用）
 * =========================== */

/* 将 tcp_driver_t 绑定到 modbus 实例：填充 transport 回调表，
 * 设置 mb->mode=TCP。drv 由调用方(适配器)提供。 */
void modbus_tcp_attach_transport(modbus_t *mb, modbus_tcp_ctx_t *t,
                                 const tcp_driver_t *drv);

/* 初始化一个 server slot（不注册进全局实例表，避免 modbus_process_all 重复处理）。
 * 共享模板的 data_map 指针。drv 由 server 容器传入。 */
void modbus_tcp_slot_init(modbus_tcp_client_slot_t *s, const modbus_t *tpl,
                          const tcp_driver_t *drv);

/* 每 tick 调用：accept 新连接 → 逐 slot 处理 → 释放断开/空闲超时的 slot 并通知 */
void modbus_tcp_server_process(modbus_tcp_server_t *srv);

/* MBAP 帧封装（TCP 端口层专属）：
 *  - tcp_frame_tx: core 的 [unit][func][data...] → 线上 [tid][pid=0][len][unit][func][data...]
 *  - tcp_frame_rx: 线上帧校验(pid=0、长度匹配)后重排为 [unit][func][data...] 给 core，
 *    并把请求 tid 存入 last_rx_tid 供应答回显；返回 0=成功，<0=帧错误。 */
uint16_t tcp_frame_tx(void *ctx, const uint8_t *pdu, uint16_t pdu_len,
                      uint8_t *out, uint16_t out_cap);
int      tcp_frame_rx(void *ctx, uint8_t *raw, uint16_t *raw_len);

#endif /* MODBUS_ENABLE_TCP */

#ifdef __cplusplus
}
#endif

#endif /* __MODBUS_TCP_H__ */
