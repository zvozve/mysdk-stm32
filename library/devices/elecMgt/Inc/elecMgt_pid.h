/* ============================================================
 * elecMgt_pid.h - PID module (预留)
 *
 * 职责：线性/闭环生成 point（跳过帧），直接注入核心。
 *
 * 与 step 的区别：
 *   step -> 参数先展开为帧，再交给 core 生成 point
 *   pid  -> 直接计算 point，调用 EM_UpdateActionPoints 直出
 *
 * 当前为占位实现（TODO），保留与 step 对称的接口，
 * 后续接入 PID 闭环时在此填充生成逻辑即可。
 * ============================================================ */

#ifndef __ELECMGT_PID_H__
#define __ELECMGT_PID_H__

#include "elecMgt_core.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 占位 PID 参数（后续按实际需求扩展） */
typedef struct {
    int32_t          kp, ki, kd;     /**< PID 系数（占位） */
    uint16_t         duration_us;     /**< 动作总时长 */
    uint8_t          target_duty;    /**< 目标占空比 0-100 */
    hbridge_state_t  state;          /**< 方向 */
} em_pid_cfg_t;

/**
 * @brief 线性生成 point（TODO: 实现 PID 数学）
 *
 * @return 生成的 point 数量
 */
uint16_t em_pid_build_points(const em_pid_cfg_t *cfg,
                             em_point_t *out, uint16_t max_points);

/**
 * @brief 注册 PID 配置到核心（直出 point）
 */
void em_pid_register(em_trigger_t src, uint8_t ch_id, const em_pid_cfg_t *cfg);

/**
 * @brief 预留初始化入口
 */
void EM_Pid_Init(void);

#ifdef __cplusplus
}
#endif

#endif /* __ELECMGT_PID_H__ */
