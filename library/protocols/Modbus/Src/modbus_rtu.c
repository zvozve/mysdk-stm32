#include "modbus_core.h"   // 提供 MODBUS_ENABLE_RTU（默认 1，可被 CMake -D 覆盖）
#include "modbus_rtu.h"
#include <string.h>         // memcpy（rtu_frame_tx 使用）

#if MODBUS_ENABLE_RTU

// ===========================
// CRC16（RTU 帧校验，纯算法，无 BSP 依赖）
// ===========================
uint16_t modbus_crc16(const uint8_t *data, uint16_t len) {
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

/* 帧封装：在 PDU 尾部追加 RTU CRC（小端），输出到 out。
 * 与原先 core 直接加 CRC 行为一致（out == 原 tx_buf 时字节完全相同）。 */
uint16_t rtu_frame_tx(void *ctx, const uint8_t *pdu, uint16_t pdu_len,
                      uint8_t *out, uint16_t out_cap) {
    (void)ctx;
    if (!out || !pdu || pdu_len == 0 || pdu_len + 2 > out_cap) return 0;
    memcpy(out, pdu, pdu_len);
    uint16_t crc = modbus_crc16(pdu, pdu_len);
    out[pdu_len]     = crc & 0xFF;
    out[pdu_len + 1] = (crc >> 8) & 0xFF;
    return pdu_len + 2;
}

/* 帧校验：校验 RTU CRC，成功返回 0。
 * 就地不修改 raw/rx_len（解析逻辑本就忽略帧尾 CRC，保持与原实现逐字节一致）。 */
int rtu_frame_rx(void *ctx, uint8_t *raw, uint16_t *raw_len) {
    (void)ctx;
    if (!raw || !raw_len || *raw_len < 4) return -1;
    uint16_t crc_calc = modbus_crc16(raw, *raw_len - 2);
    uint16_t crc_recv = raw[*raw_len - 2] | (raw[*raw_len - 1] << 8);
    return (crc_calc == crc_recv) ? 0 : -1;
}

// ===========================
// RTU 时序（字符时间 / 帧超时，纯数学，无 BSP 依赖）
// ===========================
void modbus_rtu_init(modbus_rtu_ctx_t *ctx, uint32_t baudrate) {
    if (!ctx || baudrate == 0) return;
    ctx->baudrate = baudrate;
    // 1字符时间 = 10位 / 波特率 (1起始+8数据+1停止)
    ctx->char_time_us = 1000000 / (baudrate / MODBUS_RTU_CHAR_BITS);
    // 3.5字符时间
    ctx->frame_timeout_us = ctx->char_time_us * 35 / 10;
    ctx->last_byte_tick = 0;
    ctx->rx_state = 0;
}

void modbus_rtu_reset(modbus_rtu_ctx_t *ctx) {
    if (!ctx) return;
    ctx->rx_state = 0;
    ctx->last_byte_tick = 0;
}

uint32_t modbus_rtu_get_char_time_us(modbus_rtu_ctx_t *ctx) {
    return ctx ? ctx->char_time_us : 0;
}

uint32_t modbus_rtu_get_frame_timeout_us(modbus_rtu_ctx_t *ctx) {
    return ctx ? ctx->frame_timeout_us : 0;
}

int modbus_rtu_check_frame(modbus_rtu_ctx_t *ctx, uint32_t now_us,
                           uint8_t *buf, uint16_t *len) {
    if (!ctx || !buf || !len) return 0;

    if (*len == 0) {
        ctx->rx_state = 0;
        ctx->last_byte_tick = now_us;
        return 0;
    }

    // 检查帧间隔
    uint32_t interval = now_us - ctx->last_byte_tick;
    ctx->last_byte_tick = now_us;

    if (ctx->rx_state == 0) {
        // 等待帧头，只要有数据就开始接收
        ctx->rx_state = 1;
        return 0;
    }

    // 接收中，检查是否超时（帧结束）
    if (interval > ctx->frame_timeout_us && *len >= 4) {
        // 完整帧，重置状态
        ctx->rx_state = 0;
        ctx->last_byte_tick = now_us;
        return 1;
    }

    // 缓冲区满
    if (*len >= 256) {
        ctx->rx_state = 0;
        *len = 0;
        return 0;
    }

    return 0;
}

#endif /* MODBUS_ENABLE_RTU */
