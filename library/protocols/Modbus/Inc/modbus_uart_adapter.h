// modbus_uart_adapter.h
#ifndef __MODBUS_UART_ADAPTER_H__
#define __MODBUS_UART_ADAPTER_H__

#include "modbus_core.h"

#if MODBUS_ENABLE_RTU
#include "oop_uart_drv.h"

#ifdef __cplusplus
extern "C" {
#endif

void modbus_uart_adapter_init(modbus_t *modbus_ctx, uart_drv_t *uart_drv);

#ifdef __cplusplus
}
#endif
#endif /* MODBUS_ENABLE_RTU */

#endif // __MODBUS_UART_ADAPTER_H__