#include "modbus_slave.h"
#include "SEGGER_RTT_Log.h"
#include <string.h>

void modbus_slave_init(modbus_t *ctx, const modbus_slave_config_t *cfg) {
    if (!ctx || !cfg) return;
    
    modbus_init(ctx);
    modbus_set_role(ctx, MODBUS_ROLE_SLAVE);
    modbus_set_slave_addr(ctx, cfg->slave_addr);
    modbus_set_timeouts(ctx, 1000, cfg->slave_timeout_ms, 0);
    
    // 线圈
    if (cfg->coils && cfg->coil_count > 0) {
        ctx->data_map.coils = cfg->coils;
        ctx->data_map.coils_size = cfg->coil_count;  // 位数量
    } else {
        ctx->data_map.coils = NULL;
        ctx->data_map.coils_size = 0;
    }
    
    // 保持寄存器
    if (cfg->holding_regs && cfg->holding_reg_count > 0) {
        ctx->data_map.holding_regs = cfg->holding_regs;
        ctx->data_map.holding_size = cfg->holding_reg_count;
    } else {
        ctx->data_map.holding_regs = NULL;
        ctx->data_map.holding_size = 0;
    }
    
    MODBUS_LOG("Slave initialized: addr=%d, coils=%d, regs=%d",
               cfg->slave_addr, cfg->coil_count, cfg->holding_reg_count);
}

int modbus_slave_set_coil(modbus_t *ctx, uint16_t addr, bool value) {
    if (!ctx || !ctx->data_map.coils || addr >= ctx->data_map.coils_size) return -1;
    uint8_t byte_idx = addr / 8;
    uint8_t bit_idx = addr % 8;
    if (value) {
        ctx->data_map.coils[byte_idx] |= (1 << bit_idx);
    } else {
        ctx->data_map.coils[byte_idx] &= ~(1 << bit_idx);
    }
    // 本地修改不触发回调（用户自己知道改了）
    // 如果用户需要触发，可以单独调用
    return 0;
}

bool modbus_slave_get_coil(modbus_t *ctx, uint16_t addr) {
    if (!ctx || !ctx->data_map.coils || addr >= ctx->data_map.coils_size) return false;
    uint8_t byte_idx = addr / 8;
    uint8_t bit_idx = addr % 8;
    return (ctx->data_map.coils[byte_idx] >> bit_idx) & 0x01;
}

int modbus_slave_set_reg(modbus_t *ctx, uint16_t addr, uint16_t value) {
    if (!ctx || !ctx->data_map.holding_regs || addr >= ctx->data_map.holding_size) return -1;
    ctx->data_map.holding_regs[addr] = value;
    // 本地修改不触发回调
    return 0;
}

uint16_t modbus_slave_get_reg(modbus_t *ctx, uint16_t addr) {
    if (!ctx || !ctx->data_map.holding_regs || addr >= ctx->data_map.holding_size) return 0;
    return ctx->data_map.holding_regs[addr];
}

void modbus_slave_set_reg_change_callback(modbus_t *ctx,
    void (*callback)(uint16_t addr, uint16_t old_val, uint16_t new_val)) {
    if (ctx) ctx->on_master_reg_change = callback;
}

void modbus_slave_set_coil_change_callback(modbus_t *ctx,
    void (*callback)(uint16_t addr, bool old_val, bool new_val)) {
    if (ctx) ctx->on_master_coil_change = callback;
}