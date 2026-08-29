#include "modbus_core.h"
#include "SEGGER_RTT_Log.h"
#include <string.h>
#include <stdlib.h>

// ===========================
// 全局实例列表
// ===========================
static modbus_t *g_modbus_instances[MODBUS_MAX_INSTANCES];
static uint8_t g_modbus_instance_count = 0;

// ===========================
// CRC16
// ===========================
// CRC16 已移至 modbus_rtu.c（RTU 帧封装，#ifdef MODBUS_ENABLE_RTU）

// ===========================
// 实例管理
// ===========================
void modbus_register_instance(modbus_t *ctx) {
    if (!ctx) return;
    if (g_modbus_instance_count >= MODBUS_MAX_INSTANCES) {
        ERR_LOG("Modbus instance count exceeds max!");
        return;
    }
    for (int i = 0; i < g_modbus_instance_count; i++) {
        if (g_modbus_instances[i] == ctx) return;
    }
    g_modbus_instances[g_modbus_instance_count++] = ctx;
    MODBUS_LOG("Registered modbus instance %d", g_modbus_instance_count);
}

void modbus_unregister_instance(modbus_t *ctx) {
    if (!ctx) return;
    for (int i = 0; i < g_modbus_instance_count; i++) {
        if (g_modbus_instances[i] == ctx) {
            for (int j = i; j < g_modbus_instance_count - 1; j++) {
                g_modbus_instances[j] = g_modbus_instances[j + 1];
            }
            g_modbus_instance_count--;
            MODBUS_LOG("Unregistered modbus instance");
            return;
        }
    }
}

void modbus_process_all(void) {
    for (int i = 0; i < g_modbus_instance_count; i++) {
        if (g_modbus_instances[i]) {
            modbus_process(g_modbus_instances[i]);
        }
    }
}

// ===========================
// 内部函数
// ===========================
static uint16_t build_exception_response(uint8_t slave_addr, uint8_t func_code,
                                          uint8_t exception_code, uint8_t *buf) {
    buf[0] = slave_addr;
    buf[1] = func_code | 0x80;
    buf[2] = exception_code;
    return 3;
}

// ===========================
// 断线检测
// ===========================
static void check_line_status(modbus_t *ctx) {
    uint32_t now = MB_GET_TICK();
    
    // 已断线
    if (ctx->line_state == MODBUS_LINE_DISCONNECTED) {
        if (ctx->role == MODBUS_ROLE_MASTER) {
            if (now - ctx->reconnect_tick >= ctx->reconnect_interval) {
                ctx->line_state = MODBUS_LINE_OK;
                ctx->timeout_count = 0;
                ctx->reconnect_tick = now;
                MODBUS_LOG("Line recovered (reconnect)");
                if (ctx->on_line_recover) ctx->on_line_recover(ctx);
            }
        }
        return;
    }
    
    // 线路正常
    if (ctx->role == MODBUS_ROLE_MASTER) {
        if (ctx->state == MODBUS_STATE_WAITING_RESPONSE) {
            uint32_t elapsed = now - ctx->send_tick;
            if (elapsed > ctx->response_timeout) {
                ctx->timeout_count++;
                MODBUS_LOG("Timeout %d/%d", ctx->timeout_count, ctx->max_timeout_count);
                
                if (ctx->timeout_count >= ctx->max_timeout_count) {
                    ctx->line_state = MODBUS_LINE_DISCONNECTED;
                    ctx->reconnect_tick = now;
                    MODBUS_LOG("Line DISCONNECTED!");
                    if (ctx->on_line_break) ctx->on_line_break(ctx);
                    ctx->state = MODBUS_STATE_IDLE;
                    if (ctx->transaction.pending) {
                        ctx->transaction.pending = false;
                        ctx->transaction.completed = true;
                        ctx->transaction.result = -1;
                        if (ctx->transaction.resp_len) *ctx->transaction.resp_len = 0;
                        /* 通知 master 层：当前作业以失败收尾（释放作业槽 / 回调 done） */
                        if (ctx->on_master_response) ctx->on_master_response(ctx);
                    }
                } else {
                    ctx->line_state = MODBUS_LINE_TIMEOUT;
                    if (ctx->transaction.req_data && ctx->transaction.req_len > 0) {
                        MODBUS_LOG("Retry send...");
                        uint8_t *rbuf = ctx->transaction.req_data;
                        uint16_t rlen = ctx->transaction.req_len;
                        if (ctx->transport.frame_tx) {
                            rlen = ctx->transport.frame_tx(ctx->transport.ctx,
                                                           ctx->transaction.req_data,
                                                           ctx->transaction.req_len,
                                                           ctx->tx_frame, MODBUS_BUF_SIZE);
                            rbuf = ctx->tx_frame;
                        }
                        ctx->transport.send(ctx->transport.ctx, rbuf, rlen);
                        ctx->send_tick = now;
                    }
                }
            }
        }
    } else {
        // 从机：空闲超时
        uint32_t elapsed = now - ctx->last_activity_tick;
        if (elapsed > ctx->slave_timeout_ms && ctx->line_state == MODBUS_LINE_OK) {
            ctx->line_state = MODBUS_LINE_DISCONNECTED;
            MODBUS_LOG("Slave line DISCONNECTED (idle %lu ms)", elapsed);
            if (ctx->on_line_break) ctx->on_line_break(ctx);
        }
    }
}

// ===========================
// 从机请求处理（由modbus_process调用）
// ===========================
static void process_slave_request(modbus_t *ctx) {
    uint8_t addr = ctx->rx_buf[0];
    uint8_t func = ctx->rx_buf[1];
    bool is_broadcast = (addr == MODBUS_BROADCAST_ADDR);
    uint16_t resp_len = 0;
    uint8_t exception = 0;
    
    MODBUS_LOG("Slave recv: addr=0x%02X func=0x%02X", addr, func);
    HEX_LOG("RX: ", ctx->rx_buf, ctx->rx_len);
    
    // 功能码分发（只实现核心功能，具体实现在slave模块中）
    // 这里由外部注册的处理函数处理
    // 简化版本：直接处理
    switch(func) {
        case MODBUS_FC_READ_COILS: {
            uint16_t start_addr = (ctx->rx_buf[2] << 8) | ctx->rx_buf[3];
            uint16_t count = (ctx->rx_buf[4] << 8) | ctx->rx_buf[5];
            
            SYS_LOG("[SLAVE] READ_COILS: start=%d, count=%d, coils=%p, size=%d", 
            start_addr, count, ctx->data_map.coils, ctx->data_map.coils_size);
            
            if (count < 1 || count > 2000) {
                exception = MODBUS_EXCEPTION_ILLEGAL_DATA_VALUE;
                break;
            }
            if (!ctx->data_map.coils || start_addr + count > ctx->data_map.coils_size * 8) {
                exception = MODBUS_EXCEPTION_ILLEGAL_DATA_ADDR;
                break;
            }
            
            ctx->tx_buf[0] = addr;
            ctx->tx_buf[1] = func;
            uint8_t byte_count = (count + 7) / 8;
            ctx->tx_buf[2] = byte_count;
            memset(ctx->tx_buf + 3, 0, byte_count);
            
            for (uint16_t i = 0; i < count; i++) {
                uint16_t bit_addr = start_addr + i;
                uint8_t byte_idx = bit_addr / 8;
                uint8_t bit_idx = bit_addr % 8;
                if (ctx->data_map.coils[byte_idx] & (1 << bit_idx)) {
                    ctx->tx_buf[3 + (i / 8)] |= (1 << (i % 8));
                }
            }
            resp_len = 3 + byte_count;
            break;
        }
        
        case MODBUS_FC_READ_HOLDING_REGS: {
            uint16_t start_addr = (ctx->rx_buf[2] << 8) | ctx->rx_buf[3];
            uint16_t count = (ctx->rx_buf[4] << 8) | ctx->rx_buf[5];
            
            if (count < 1 || count > 125) {
                exception = MODBUS_EXCEPTION_ILLEGAL_DATA_VALUE;
                break;
            }
            if (!ctx->data_map.holding_regs || start_addr + count > ctx->data_map.holding_size) {
                MODBUS_LOG("HR 0x02: start=%u cnt=%u map=%u regs=%p",
                           start_addr, count, ctx->data_map.holding_size,
                           (void*)ctx->data_map.holding_regs);
                exception = MODBUS_EXCEPTION_ILLEGAL_DATA_ADDR;
                break;
            }

            ctx->tx_buf[0] = addr;
            ctx->tx_buf[1] = func;
            ctx->tx_buf[2] = count * 2;
            for (uint16_t i = 0; i < count; i++) {
                uint16_t val = ctx->data_map.holding_regs[start_addr + i];
                ctx->tx_buf[3 + i * 2] = (val >> 8) & 0xFF;
                ctx->tx_buf[3 + i * 2 + 1] = val & 0xFF;
            }
            resp_len = 3 + count * 2;
            break;
        }
        
        case MODBUS_FC_WRITE_SINGLE_COIL: {
            uint16_t addr_w = (ctx->rx_buf[2] << 8) | ctx->rx_buf[3];
            uint16_t value = (ctx->rx_buf[4] << 8) | ctx->rx_buf[5];
            
            if (!ctx->data_map.coils || addr_w >= ctx->data_map.coils_size * 8) {
                exception = MODBUS_EXCEPTION_ILLEGAL_DATA_ADDR;
                break;
            }
            if (value != 0x0000 && value != 0xFF00) {
                exception = MODBUS_EXCEPTION_ILLEGAL_DATA_VALUE;
                break;
            }
            
            uint16_t old_val = ctx->data_map.coils[addr_w / 8];
            bool old_bit = (old_val >> (addr_w % 8)) & 0x01;
            bool new_bit = (value == 0xFF00);
            
            if (new_bit) {
                ctx->data_map.coils[addr_w / 8] |= (1 << (addr_w % 8));
            } else {
                ctx->data_map.coils[addr_w / 8] &= ~(1 << (addr_w % 8));
            }
            
            // 触发从机变化回调
            if (old_bit != new_bit && ctx->on_master_coil_change) {
                ctx->on_master_coil_change(addr_w, old_bit, new_bit);
            }
            
            if (!is_broadcast) {
                memcpy(ctx->tx_buf, ctx->rx_buf, 6);
                resp_len = 6;
            }
            break;
        }
        
        case MODBUS_FC_WRITE_SINGLE_REG: {
            uint16_t addr_w = (ctx->rx_buf[2] << 8) | ctx->rx_buf[3];
            uint16_t value = (ctx->rx_buf[4] << 8) | ctx->rx_buf[5];
            
            if (!ctx->data_map.holding_regs || addr_w >= ctx->data_map.holding_size) {
                exception = MODBUS_EXCEPTION_ILLEGAL_DATA_ADDR;
                break;
            }
            
            uint16_t old_val = ctx->data_map.holding_regs[addr_w];
            ctx->data_map.holding_regs[addr_w] = value;
            
            if (old_val != value && ctx->on_master_reg_change) {
                ctx->on_master_reg_change(addr_w, old_val, value);
            }
            
            if (!is_broadcast) {
                memcpy(ctx->tx_buf, ctx->rx_buf, 6);
                resp_len = 6;
            }
            break;
        }
        
        case MODBUS_FC_WRITE_MULTIPLE_REGS: {
            uint16_t start_addr = (ctx->rx_buf[2] << 8) | ctx->rx_buf[3];
            uint16_t count = (ctx->rx_buf[4] << 8) | ctx->rx_buf[5];
            uint8_t byte_count = ctx->rx_buf[6];
            
            if (!ctx->data_map.holding_regs || start_addr + count > ctx->data_map.holding_size) {
                exception = MODBUS_EXCEPTION_ILLEGAL_DATA_ADDR;
                break;
            }
            if (byte_count != count * 2) {
                exception = MODBUS_EXCEPTION_ILLEGAL_DATA_VALUE;
                break;
            }
            
            for (uint16_t i = 0; i < count; i++) {
                uint16_t val = (ctx->rx_buf[7 + i * 2] << 8) | ctx->rx_buf[7 + i * 2 + 1];
                uint16_t old_val = ctx->data_map.holding_regs[start_addr + i];
                ctx->data_map.holding_regs[start_addr + i] = val;
                if (old_val != val && ctx->on_master_reg_change) {
                    ctx->on_master_reg_change(start_addr + i, old_val, val);
                }
            }
            
            if (!is_broadcast) {
                ctx->tx_buf[0] = addr;
                ctx->tx_buf[1] = func;
                ctx->tx_buf[2] = ctx->rx_buf[2];
                ctx->tx_buf[3] = ctx->rx_buf[3];
                ctx->tx_buf[4] = ctx->rx_buf[4];
                ctx->tx_buf[5] = ctx->rx_buf[5];
                resp_len = 6;
            }
            break;
        }
        
        default:
            exception = MODBUS_EXCEPTION_ILLEGAL_FUNCTION;
            break;
    }
    
    if (exception != 0) {
        MODBUS_LOG("Slave exception: 0x%02X", exception);
        if (!is_broadcast) {
            resp_len = build_exception_response(addr, func, exception, ctx->tx_buf);
        }
    }
    
    if (!is_broadcast && resp_len > 0) {
        MODBUS_LOG("Slave response len=%d", resp_len);
        HEX_LOG("TX: ", ctx->tx_buf, resp_len);
        uint8_t *send_buf = ctx->tx_buf;
        uint16_t send_len = resp_len;
        if (ctx->transport.frame_tx) {
            send_len = ctx->transport.frame_tx(ctx->transport.ctx,
                                               ctx->tx_buf, resp_len,
                                               ctx->tx_frame, MODBUS_BUF_SIZE);
            send_buf = ctx->tx_frame;
        }
        ctx->transport.send(ctx->transport.ctx, send_buf, send_len);
        ctx->state = MODBUS_STATE_SENDING;
    }
}

// ===========================
// modbus_process - 核心主循环
// ===========================
void modbus_process(modbus_t *ctx) {
    if (!ctx) return;
    if (!ctx->transport.get_tick) return;
    
    uint32_t now = ctx->transport.get_tick();
    
    /* TCP 端口驱动：accept/connect/重连/非阻塞收包+帧重组由端口层自行驱动。
     * RTU 端口注册 port_poll=NULL，此调用为零开销空判。 */
    if (ctx->transport.port_poll) {
        ctx->transport.port_poll(ctx->transport.ctx);
    }
    
    // 断线检测
    check_line_status(ctx);
    
    /* ★ 主机轮询已移到本函数末尾（接收处理之后）——原因见文末注释。
     *   在此处判定会导致轮询永久饿死：此刻 state 必为 WAITING（本次应答
     *   还没解析），轮询排不上；而应答解析后腾出的 IDLE 又会被同一轮
     *   主循环里后续任务的后台写（波形写出等）抢走。 */

    // 检查接收数据
    uint16_t available = ctx->transport.peek(ctx->transport.ctx);
    if (available > 0) {
        uint16_t read_len = ctx->transport.recv(ctx->transport.ctx, 
                                                 ctx->rx_buf,
                                                 MODBUS_BUF_SIZE);
        if (read_len > 0) {
            ctx->rx_len = read_len;
            ctx->last_activity_tick = now;
            
            if (ctx->role == MODBUS_ROLE_SLAVE && ctx->line_state == MODBUS_LINE_DISCONNECTED) {
                ctx->line_state = MODBUS_LINE_OK;
                MODBUS_LOG("Slave line recovered (data received)");
                if (ctx->on_line_recover) ctx->on_line_recover(ctx);
            }
            
            if (ctx->line_state == MODBUS_LINE_DISCONNECTED) {
                ctx->line_state = MODBUS_LINE_OK;
                MODBUS_LOG("Line recovered");
                if (ctx->on_line_recover) ctx->on_line_recover(ctx);
            }
            
            MODBUS_LOG("Received %d bytes", ctx->rx_len);
            HEX_LOG("RX: ", ctx->rx_buf, ctx->rx_len);
            
            if (ctx->role == MODBUS_ROLE_MASTER) {
                if (ctx->state == MODBUS_STATE_WAITING_RESPONSE && ctx->rx_len >= 4) {
                    if (ctx->transport.frame_rx == NULL ||
                        ctx->transport.frame_rx(ctx->transport.ctx, ctx->rx_buf, &ctx->rx_len) == 0) {
                        MODBUS_LOG("Master recv valid response");
                        if (ctx->rx_buf[1] & 0x80) {
                            MODBUS_LOG("Exception: 0x%02X", ctx->rx_buf[2]);
                            ctx->state = MODBUS_STATE_IDLE;
                            ctx->transaction.pending = false;
                            ctx->transaction.completed = true;
                            ctx->transaction.result = -2;
                            if (ctx->transaction.resp_len) *ctx->transaction.resp_len = 0;
                            /* 通知 master 层：当前作业以异常失败收尾 */
                            if (ctx->on_master_response) ctx->on_master_response(ctx);
                        } else {
                            ctx->state = MODBUS_STATE_IDLE;
                            ctx->timeout_count = 0;
                            ctx->transaction.result = 0;
                            /* 数据落地移入 master 层钩子：按 transaction.job 路由到
                             * 各段 shadow / 一次性作业 / 写作业（多段轮询依赖此路由）。
                             * 无钩子时退化为旧版单缓冲 data_map 路径（兼容未挂载仲裁器的用法）。 */
                            if (ctx->on_master_response) {
                                ctx->on_master_response(ctx);
                            } else {
                                uint8_t fc = ctx->transaction.func_code;
                                if (fc == MODBUS_FC_READ_HOLDING_REGS &&
                                    ctx->data_map.holding_regs && ctx->rx_len >= 3) {
                                    uint16_t n = ctx->rx_buf[2];
                                    uint16_t maxb = ctx->data_map.holding_size * 2;
                                    if (n > maxb) n = maxb;
                                    /* Modbus 保持寄存器为大端字节序：必须逐寄存器拼装，
                                     * 不能直接 memcpy 到 uint16_t 缓冲——小端机下会把
                                     * 线上的 00 02 存成 0x0200=512，导致 STATE 等字段越界。 */
                                    {
                                        uint16_t cnt = n / 2;
                                        for (uint16_t i = 0; i < cnt; i++) {
                                            ctx->data_map.holding_regs[i] =
                                                (uint16_t)((ctx->rx_buf[3 + i * 2] << 8) |
                                                            ctx->rx_buf[3 + i * 2 + 1]);
                                        }
                                    }
                                    if (ctx->on_master_reg_change && ctx->last_regs) {
                                        uint16_t cnt = n / 2;
                                        if (cnt > ctx->last_regs_count) cnt = ctx->last_regs_count;
                                        if (!ctx->reg_baseline_done) {
                                            for (uint16_t i = 0; i < cnt; i++)
                                                ctx->last_regs[i] = ctx->data_map.holding_regs[i];
                                            ctx->reg_baseline_done = true;
                                        } else {
                                            for (uint16_t i = 0; i < cnt; i++) {
                                                if (ctx->data_map.holding_regs[i] != ctx->last_regs[i]) {
                                                    ctx->on_master_reg_change(i, ctx->last_regs[i],
                                                                             ctx->data_map.holding_regs[i]);
                                                    ctx->last_regs[i] = ctx->data_map.holding_regs[i];
                                                }
                                            }
                                        }
                                    } else {
                                        ctx->reg_baseline_done = true;
                                    }
                                } else if (fc == MODBUS_FC_READ_COILS &&
                                           ctx->data_map.coils && ctx->rx_len >= 3) {
                                    uint16_t n = ctx->rx_buf[2];
                                    uint16_t maxb = (ctx->data_map.coils_size + 7) / 8;
                                    if (n > maxb) n = maxb;
                                    memcpy(ctx->data_map.coils, ctx->rx_buf + 3, n);
                                    if (ctx->on_master_coil_change && ctx->last_coils) {
                                        uint16_t cnt = ctx->last_coils_count;
                                        if (!ctx->coil_baseline_done) {
                                            for (uint16_t i = 0; i < cnt; i++) {
                                                uint8_t bi = (uint8_t)(i / 8);
                                                uint8_t bit = (uint8_t)(i % 8);
                                                if ((ctx->data_map.coils[bi] >> bit) & 0x01)
                                                    ctx->last_coils[bi] |= (uint8_t)(1u << bit);
                                                else
                                                    ctx->last_coils[bi] &= (uint8_t)~(1u << bit);
                                            }
                                            ctx->coil_baseline_done = true;
                                        } else {
                                            for (uint16_t i = 0; i < cnt; i++) {
                                                uint8_t bi = (uint8_t)(i / 8);
                                                uint8_t bit = (uint8_t)(i % 8);
                                                bool nv = (ctx->data_map.coils[bi] >> bit) & 0x01;
                                                bool ov = (ctx->last_coils[bi] >> bit) & 0x01;
                                                if (nv != ov) {
                                                    ctx->on_master_coil_change(i, ov, nv);
                                                    if (nv) ctx->last_coils[bi] |= (uint8_t)(1u << bit);
                                                    else   ctx->last_coils[bi] &= (uint8_t)~(1u << bit);
                                                }
                                            }
                                        }
                                    } else {
                                        ctx->coil_baseline_done = true;
                                    }
                                }
                            }
                            /* 兼容：仍有调用方显式提供 resp 缓冲时写入原始帧 */
                            if (ctx->transaction.resp_data && ctx->transaction.resp_len) {
                                uint16_t data_len = ctx->rx_len;
                                if (data_len > *ctx->transaction.resp_len)
                                    data_len = *ctx->transaction.resp_len;
                                memcpy(ctx->transaction.resp_data, ctx->rx_buf, data_len);
                                *ctx->transaction.resp_len = data_len;
                            }
                            ctx->transaction.pending = false;
                            ctx->transaction.completed = true;
                        }
                    } else {
                        MODBUS_LOG("Frame error");
                    }
                    ctx->rx_len = 0;
                }
            } else {
                if (ctx->rx_len >= 4) {
                    /* 先帧校验、再查地址：TCP 的 frame_rx 会把线上帧重排为
                     * [unit][func][data...]（unit 从 MBAP 取出），地址必须在重排后读；
                     * RTU 的 frame_rx 只验 CRC 不改缓冲区，因此顺序调整对 RTU 无影响。 */
                    if (ctx->transport.frame_rx == NULL ||
                        ctx->transport.frame_rx(ctx->transport.ctx, ctx->rx_buf, &ctx->rx_len) == 0) {
                        uint8_t addr = ctx->rx_buf[0];
                        if (addr == ctx->slave_addr || addr == MODBUS_BROADCAST_ADDR) {
                            ctx->state = MODBUS_STATE_PROCESSING;
                            process_slave_request(ctx);
                        } else {
                            MODBUS_LOG("Addr mismatch: 0x%02X != 0x%02X", addr, ctx->slave_addr);
                        }
                    } else {
                        MODBUS_LOG("Frame error");
                    }
                    ctx->rx_len = 0;
                }
            }
        }
    }

    /* ===== 主机轮询：必须放在“接收处理”之后 =====
     * 此刻本次应答刚被解析完、state 已回到 IDLE，轮询可以立即抢占总线。
     *
     * 为什么不能放在函数开头（2026-08-17 修复的实机故障）：
     *   主循环顺序是 TaskModbus_M_Process(→本函数) → ... → TaskWave_Process。
     *   放开头时，每一轮进来 state 都还是 WAITING（应答在本函数后半段才解析），
     *   轮询条件永远不成立；而后半段解析应答腾出的那个 IDLE，会被同一轮里
     *   随后执行的 TaskWave_Process（波形分帧写出）立刻抢走 → 总线被后台写
     *   100% 占满，func=0x03 读帧一次都发不出去。
     *   后果：屏端参数（如 W1 动作线设值）被用户改了，MCU 永远读不回来，
     *         on_master_reg_change 不触发 → 参考线不重写、日志无输出。
     * 放在末尾后，轮询到期时最多延迟一次事务（约一帧时间）即可拿到总线。
     *
     * 2026-08-18 起：间隔判定（poll_interval / last_poll_tick）移除，
     * 由 master 层仲裁器（poll_callback = mb_arbiter_tick）内部统一调度：
     * 帧节拍 min_frame_gap + 各作业独立周期/相位 + 优先级都在那里处理。 */
    if (ctx->role == MODBUS_ROLE_MASTER && ctx->poll_callback) {
        if (ctx->state == MODBUS_STATE_IDLE) {
            ctx->poll_callback(ctx);
        }
    }
}

// ===========================
// 核心API实现
// ===========================
void modbus_init(modbus_t *ctx) {    
    memset(ctx, 0, sizeof(modbus_t));
    ctx->slave_addr = 1;
    ctx->role = MODBUS_ROLE_SLAVE;
    ctx->mode = MODBUS_MODE_RTU;
    ctx->state = MODBUS_STATE_IDLE;
    ctx->line_state = MODBUS_LINE_OK;
    ctx->response_timeout = 1000;
    ctx->slave_timeout_ms = 5000;
    ctx->max_timeout_count = 3;
    ctx->poll_interval = 100;
    ctx->reconnect_interval = 10000;
    ctx->last_poll_tick = 0;
    ctx->timeout_count = 0;
    ctx->reconnect_tick = 0;
    ctx->on_line_break = NULL;
    ctx->on_line_recover = NULL;
    if (ctx->transport.get_tick) {
        ctx->last_activity_tick = ctx->transport.get_tick();
    }
    modbus_register_instance(ctx);
    MODBUS_LOG("Modbus instance initialized");
}

void modbus_set_transport(modbus_t *ctx, const modbus_transport_t *transport) {
    if (ctx && transport) {
        memcpy(&ctx->transport, transport, sizeof(modbus_transport_t));
        MODBUS_LOG("Transport set");
    }
}

void modbus_set_role(modbus_t *ctx, modbus_role_t role) {
    if (ctx) {
        ctx->role = role;
        MODBUS_LOG("Set role: %s", role == MODBUS_ROLE_MASTER ? "MASTER" : "SLAVE");
    }
}

void modbus_set_slave_addr(modbus_t *ctx, uint8_t addr) {
    if (ctx) {
        ctx->slave_addr = addr;
        MODBUS_LOG("Set slave address: 0x%02X", addr);
    }
}

void modbus_set_timeouts(modbus_t *ctx, uint32_t response_timeout_ms, 
                         uint32_t slave_timeout_ms, uint8_t max_retries) {
    if (ctx) {
        ctx->response_timeout = response_timeout_ms;
        ctx->slave_timeout_ms = slave_timeout_ms;
        ctx->max_timeout_count = max_retries;
        MODBUS_LOG("Timeouts: resp=%lu, slave=%lu, retries=%d", 
                   response_timeout_ms, slave_timeout_ms, max_retries);
    }
}

void modbus_set_reconnect_interval(modbus_t *ctx, uint32_t interval_ms) {
    if (ctx) {
        ctx->reconnect_interval = interval_ms > 0 ? interval_ms : 10000;
        MODBUS_LOG("Reconnect interval: %lu ms", ctx->reconnect_interval);
    }
}

void modbus_set_line_callbacks(modbus_t *ctx, 
    void (*on_break)(modbus_t *), void (*on_recover)(modbus_t *)) {
    if (ctx) {
        ctx->on_line_break = on_break;
        ctx->on_line_recover = on_recover;
        MODBUS_LOG("Line callbacks set");
    }
}

modbus_state_t modbus_get_state(modbus_t *ctx) {
    return ctx ? ctx->state : MODBUS_STATE_ERROR;
}

modbus_line_state_t modbus_get_line_state(modbus_t *ctx) {
    return ctx ? ctx->line_state : MODBUS_LINE_DISCONNECTED;
}

uint32_t modbus_get_last_activity(modbus_t *ctx) {
    return ctx ? ctx->last_activity_tick : 0;
}