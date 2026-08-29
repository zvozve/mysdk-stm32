#include "modbus_uart_adapter.h"

#if MODBUS_ENABLE_RTU
#include "oop_uart_drv.h"
#include "modbus_rtu.h"
#include "SEGGER_RTT_Log.h"
#include <string.h>

typedef struct {
    uart_drv_t *uart_drv;
} uart_adapter_ctx_t;

#define MAX_ADAPTERS 8
static uart_adapter_ctx_t g_adapter_ctxs[MAX_ADAPTERS];
static uint8_t g_adapter_count = 0;

static uint16_t uart_peek(void *ctx) {
    uart_adapter_ctx_t *adapter = (uart_adapter_ctx_t*)ctx;
    if (!adapter || !adapter->uart_drv) return 0;
    return uart_drv_available(adapter->uart_drv);
}

static uint16_t uart_recv(void *ctx, uint8_t *buf, uint16_t len) {
    uart_adapter_ctx_t *adapter = (uart_adapter_ctx_t*)ctx;
    if (!adapter || !adapter->uart_drv || !buf) return 0;
    
    uint16_t data_len = 0;
    uint8_t *data = uart_drv_get_packet(adapter->uart_drv, &data_len);
    if (!data || data_len == 0) return 0;
    
    uint16_t read_len = (len < data_len) ? len : data_len;
    memcpy(buf, data, read_len);
    
    uart_drv_release_packet(adapter->uart_drv);
    return read_len;
}

// ★★★ 关键修复：返回类型改为 int ★★★
static int uart_send(void *ctx, const uint8_t *data, uint16_t len) {
    uart_adapter_ctx_t *adapter = (uart_adapter_ctx_t*)ctx;
    if (!adapter || !adapter->uart_drv || !data || len == 0) return -1;
    
    int ret = uart_drv_send(adapter->uart_drv, data, len);
    if (ret != 0) return ret;
    
    return 0;
}

static uint32_t uart_get_tick(void) {
    return MB_GET_TICK();
}

void modbus_uart_adapter_init(modbus_t *modbus_ctx, uart_drv_t *uart_drv) {
    if (!modbus_ctx || !uart_drv) return;
    if (g_adapter_count >= MAX_ADAPTERS) {
        ERR_LOG("Too many UART adapters!");
        return;
    }
    
    uart_adapter_ctx_t *adapter_ctx = &g_adapter_ctxs[g_adapter_count++];
    adapter_ctx->uart_drv = uart_drv;
    
    modbus_transport_t transport = {
        .ctx = adapter_ctx,
        .send = uart_send,
        .recv = uart_recv,
        .peek = uart_peek,
        .get_tick = uart_get_tick,
        .frame_tx = rtu_frame_tx,
        .frame_rx = rtu_frame_rx,
        .port_poll = NULL,
    };
    
    modbus_set_transport(modbus_ctx, &transport);
}

#endif /* MODBUS_ENABLE_RTU */