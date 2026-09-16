/* ============================================================
 * elecMgt_pid.c - PID module implementation (预留)
 *
 * 当前仅占位，接口与 step 对称，便于后续直接接入 PID 闭环。
 * 生成逻辑待实现后替换 em_pid_build_points 内部即可。
 * ============================================================ */

#include "elecMgt_pid.h"
#include "SEGGER_RTT_Log.h"

uint16_t em_pid_build_points(const em_pid_cfg_t *cfg,
                             em_point_t *out, uint16_t max_points) {
    (void)cfg; (void)out; (void)max_points;
    /* TODO: 实现 PID 线性/闭环 point 生成 */
    DBG_LOG("EM_PID: build_points not implemented yet");
    return 0;
}

void em_pid_register(em_trigger_t src, uint8_t ch_id, const em_pid_cfg_t *cfg) {
    static em_point_t s_buf[EM_TRIGGER_MAX][EM_MAX_CHANNELS][HBRIDGE_MAX_FRAMES];

    uint16_t n = em_pid_build_points(cfg, s_buf[src][ch_id], HBRIDGE_MAX_FRAMES);
    if (n > 0) {
        EM_UpdateActionPoints(src, ch_id, s_buf[src][ch_id], n);
    }
}

void EM_Pid_Init(void) {
    /* 预留：暂无默认 PID 动作 */
    DBG_LOG("EM_PID: reserved module initialized");
}
