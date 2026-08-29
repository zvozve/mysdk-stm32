#ifndef __MODBUS_TCP_ADAPTER_H__
#define __MODBUS_TCP_ADAPTER_H__

#include "modbus_core.h"

#if MODBUS_ENABLE_TCP

#include "modbus_tcp.h"   /* tcp_driver_t 等协议层类型 */
#include "lwip/api.h"      /* netconn API（LwIP 头只在本适配器文件引用） */
#include "lwip/ip_addr.h"
#include "lwip/netbuf.h"
#include "lwip/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* LwIP netconn 适配器入口（镜像 modbus_uart_adapter_init 的注册式接口）：
 *
 * modbus_tcp_adapter_client_init:
 *   mb  须先 modbus_master_init()（传输无关仲裁器）。
 *   内部注入 netconn driver 到 tcp ctx 并绑定 transport。
 *   返回 0=成功（仅指参数合法，不代表已连上），<0=失败。
 *
 * modbus_tcp_adapter_server_init:
 *   mb  须先配好 data_map/slave_addr/变更回调。
 *   内部创建监听 socket + 初始化 N 个 slot（共享 mb->data_map）。
 *   返回 0=成功，<0=失败。每 tick 调用 modbus_tcp_server_process() 驱动。 */
int modbus_tcp_adapter_client_init(modbus_t *mb, modbus_tcp_ctx_t *tcp,
                                   const char *ip_str, uint16_t port);

int modbus_tcp_adapter_server_init(modbus_t *mb, modbus_tcp_server_t *srv,
                                    uint16_t port);

#ifdef __cplusplus
}
#endif
#endif /* MODBUS_ENABLE_TCP */

#endif /* __MODBUS_TCP_ADAPTER_H__ */
