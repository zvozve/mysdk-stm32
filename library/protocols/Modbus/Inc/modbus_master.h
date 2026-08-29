#ifndef __MODBUS_MASTER_H__
#define __MODBUS_MASTER_H__

#include "modbus_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * modbus_master - 主机统一总线仲裁器
 *
 * 所有发往总线的帧（周期读、手动读、一次性写）都必须是队列中的
 * 一个"作业"，由仲裁器逐个调度，串口层只有 master_request_async
 * 一个发送入口（不存在绕过队列直接发的路径）。
 *
 * 调度顺序（每 tick，总线空闲时挑一帧发出）：
 *   1. 高优一次性作业：写作业 → 手动读作业（FIFO）
 *   2. 周期读作业：按"最早到期"挑选；若某段错过最后期限
 *      （等待 > 2×period）则无条件提升优先发出（防饿死）
 * 帧节拍由 min_frame_gap_ms 全局约束（默认 = poll_interval_ms，
 * 即"查询指令 100ms 一条"）。
 *
 * 周期写：应用层在自有时钟里周期性入队一次性写作业即可，
 * 队列天然与读轮询错开、写优先于读（本工程波形/参考线均为此模式）。
 * ============================================================ */

typedef struct {
    uint8_t target_slave_addr;
    uint32_t poll_interval_ms;      /* 读作业默认周期（add_*_range 传 0 时用），默认 100 */
    uint32_t min_frame_gap_ms;      /* 帧最小间隔，默认 = poll_interval_ms；0=不限制 */
    uint32_t response_timeout_ms;
    uint8_t max_retries;
    uint32_t reconnect_interval_ms;
} modbus_master_config_t;

void modbus_master_init(modbus_t *ctx, const modbus_master_config_t *cfg);

/* ---- 多段轮询注册（可多次调用；返回 0=成功，<0=失败/表满） ----
 * period_ms：本段轮询周期（0 = 用 config 的 poll_interval_ms）
 * phase_ms ：相位偏移（与其它段错开；如寄存器 phase=0、线圈 phase=100
 *            → 指令间隔 100ms、类型交替）
 * shadow   ：回读镜像缓冲（同时用作"最近值"与变更检测基准，下标相对 start） */
int modbus_master_add_reg_range(modbus_t *ctx, uint16_t start, uint16_t count,
                                uint16_t *shadow, uint16_t shadow_cap,
                                uint32_t period_ms, uint32_t phase_ms);
int modbus_master_add_coil_range(modbus_t *ctx, uint16_t start, uint16_t count,
                                 uint8_t *shadow, uint16_t shadow_cap,
                                 uint32_t period_ms, uint32_t phase_ms);

/* ---- 作业完成回调（读/写共用；result: 0=成功, <0=失败/异常/超时） ---- */
typedef void (*mb_job_done_t)(int result);

/* ---- 一次性写作业（高优区 FIFO；regs 指针数据须在 done 回调前保持有效） ---- */
int modbus_master_write_reg_async(modbus_t *ctx, uint16_t addr, uint16_t val,
                                  mb_job_done_t done);
int modbus_master_write_regs_async(modbus_t *ctx, uint16_t addr, uint16_t count,
                                   const uint16_t *vals, mb_job_done_t done);
int modbus_master_write_coil_async(modbus_t *ctx, uint16_t addr, bool val,
                                   mb_job_done_t done);

/* ---- 一次性读作业（未注册地址也可读；应答填调用方缓冲，无变更检测） ---- */
int modbus_master_read_regs_async(modbus_t *ctx, uint16_t addr, uint16_t count,
                                  uint16_t *buf, uint16_t buf_cap, mb_job_done_t done);
int modbus_master_read_coils_async(modbus_t *ctx, uint16_t addr, uint16_t count,
                                   uint8_t *buf, uint16_t buf_cap, mb_job_done_t done);

/* 变更回调（addr 为【绝对 Modbus 地址】；由多段读作业应答驱动） */
void modbus_master_set_reg_change_callback(modbus_t *ctx,
    void (*cb)(uint16_t addr, uint16_t old_val, uint16_t new_val));
void modbus_master_set_coil_change_callback(modbus_t *ctx,
    void (*cb)(uint16_t addr, bool old_val, bool new_val));

void modbus_master_set_poll_interval(modbus_t *ctx, uint32_t interval_ms);
void modbus_master_set_min_frame_gap(modbus_t *ctx, uint32_t gap_ms);

/* 诊断：当前队列中的作业总数（写+手动读+周期读段数） */
int modbus_master_pending_jobs(modbus_t *ctx);

#ifdef __cplusplus
}
#endif

#endif // __MODBUS_MASTER_H__
