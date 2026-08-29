#ifndef __MODBUS_RTU_H__
#define __MODBUS_RTU_H__

#include <stdint.h>
#include <stdbool.h>
#include "modbus_core.h"   // 提供 MODBUS_ENABLE_RTU 等开关宏；RTU 端口本就依赖 core

#ifdef __cplusplus
extern "C" {
#endif

#define MODBUS_RTU_CHAR_BITS    10  // 1起始位 + 8数据位 + 1停止位

#if MODBUS_ENABLE_RTU

typedef struct {
    uint32_t baudrate;
    uint32_t char_time_us;
    uint32_t frame_timeout_us;
    uint32_t last_byte_tick;
    uint8_t rx_state;  // 0=等待帧头, 1=接收中
} modbus_rtu_ctx_t;

void modbus_rtu_init(modbus_rtu_ctx_t *ctx, uint32_t baudrate);
void modbus_rtu_reset(modbus_rtu_ctx_t *ctx);
uint32_t modbus_rtu_get_char_time_us(modbus_rtu_ctx_t *ctx);
uint32_t modbus_rtu_get_frame_timeout_us(modbus_rtu_ctx_t *ctx);
int modbus_rtu_check_frame(modbus_rtu_ctx_t *ctx, uint32_t now_us,
                           uint8_t *buf, uint16_t *len);

/* RTU 帧封装（CRC）：供传输端口注册到 modbus_transport_t 的 frame_tx/frame_rx。
 * core 只处理纯 PDU，帧尾 CRC 的加/验完全落在端口层。 */
uint16_t modbus_crc16(const uint8_t *data, uint16_t len);
uint16_t rtu_frame_tx(void *ctx, const uint8_t *pdu, uint16_t pdu_len,
                      uint8_t *out, uint16_t out_cap);
int      rtu_frame_rx(void *ctx, uint8_t *raw, uint16_t *raw_len);

#endif /* MODBUS_ENABLE_RTU */

#ifdef __cplusplus
}
#endif

#endif // __MODBUS_RTU_H__
