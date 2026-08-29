#ifndef __MODBUS_SLAVE_H__
#define __MODBUS_SLAVE_H__

#include "modbus_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t slave_addr;              // 从机地址
    uint32_t slave_timeout_ms;       // 从机超时时间（ms）
    
    // 线圈
    uint8_t *coils;                  // 线圈缓冲区（NULL=不使用）
    uint16_t coil_count;             // 线圈数量（位）
    
    // 保持寄存器
    uint16_t *holding_regs;          // 保持寄存器缓冲区（NULL=不使用）
    uint16_t holding_reg_count;      // 保持寄存器数量
} modbus_slave_config_t;

void modbus_slave_init(modbus_t *ctx, const modbus_slave_config_t *cfg);

int modbus_slave_set_coil(modbus_t *ctx, uint16_t addr, bool value);
bool modbus_slave_get_coil(modbus_t *ctx, uint16_t addr);
int modbus_slave_set_reg(modbus_t *ctx, uint16_t addr, uint16_t value);
uint16_t modbus_slave_get_reg(modbus_t *ctx, uint16_t addr);

void modbus_slave_set_reg_change_callback(modbus_t *ctx,
    void (*callback)(uint16_t addr, uint16_t old_val, uint16_t new_val));
void modbus_slave_set_coil_change_callback(modbus_t *ctx,
    void (*callback)(uint16_t addr, bool old_val, bool new_val));

#ifdef __cplusplus
}
#endif

#endif // __MODBUS_SLAVE_H__