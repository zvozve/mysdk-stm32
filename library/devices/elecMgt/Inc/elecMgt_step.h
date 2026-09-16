/* ============================================================
 * elecMgt_step.h - Step module (参数 -> 帧)
 *
 * 职责：把高级「参数」（分段：极性/时间/占空比）转换为
 *       核心契约 em_frame_t（参数 -> 帧）。
 *
 * 与 core 的关系：step 不碰 DMA，只产出帧；
 *                 帧交给 EM_UpdateAction 由 core 生成 point。
 *
 * pid 模块（预留）则跳过帧，直接生成 point。
 * ============================================================ */

#ifndef __ELECMGT_STEP_H__
#define __ELECMGT_STEP_H__

#include "elecMgt_core.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EM_MAX_STEP_SEGMENTS  5

/* 单段参数（高级参数，未展开为帧） */
typedef struct {
    hbridge_state_t state;   /**< 复用 hbridge_state_t，不重定义 */
    uint16_t        time_us; /**< 段时长(微秒) */
    uint8_t         duty;    /**< 占空比 0-100 */
} em_step_seg_t;

/* 一个动作：若干段 */
typedef struct {
    em_step_seg_t seg[EM_MAX_STEP_SEGMENTS];
    uint8_t       seg_count;
} em_step_cfg_t;

/* ============================================================
 * 动作块标识（= 核心的 4 个参数槽，与 HMI Modbus 块一一对应）
 * ============================================================ */
typedef enum {
    EM_ACT_A_S1 = 0,   /* 参数组1 吸合动作 (S1) */
    EM_ACT_A_S2,       /* 参数组1 释放动作 (S2) */
    EM_ACT_B_S1,       /* 参数组2 吸合动作 (S1) */
    EM_ACT_B_S2,       /* 参数组2 释放动作 (S2) */
    EM_ACT_COUNT
} em_act_id_t;

/* 参数组（grp 索引）：组1 -> A 块，组2 -> B 块 */
#define EM_GRP_1        0
#define EM_GRP_2        1
#define EM_MAX_GROUPS   2

/* 可注册磁铁的最大数量（= 通道数） */
#define EM_MAX_MAGNETS  EM_MAX_CHANNELS

/**
 * @brief 参数 -> 帧
 *
 * 直接按段映射为帧输出（不展开 time_us）。
 * 保持帧（time_us=0）放在最后一段即可。
 *
 * @return 生成的帧数量
 */
uint8_t em_step_build_frames(const em_step_cfg_t *cfg,
                             em_frame_t *out, uint8_t max_frames);

/**
 * @brief 注册一个 step 配置到核心（自动 build 帧再 EM_UpdateAction）
 *
 * 内部用静态缓冲保存帧，生命周期与 core 动作等长，安全。
 */
void em_step_register(em_trigger_t src, uint8_t ch_id, const em_step_cfg_t *cfg);

/**
 * @brief 载入内置默认 step 配置（S1->IN1, S2->IN2）
 *
 * 在 EM_Init 之后调用一次即可获得可用默认动作。
 */
void EM_Step_Init(void);

/* ============================================================
 * 高层注册 API（业务层只需调用这几个）
 *
 * 用法：
 *   EM_Step_SetGroup(EM_GRP_1, &s1_attract, &s2_release);  // 注册参数组
 *   EM_Step_SetGroup(EM_GRP_2, &s1_attract, &s2_release);
 *   EM_Step_RegisterMagnet(0, EM_POS_UP,   EM_GRP_1);       // 注册电磁铁
 *   EM_Step_RegisterMagnet(1, EM_POS_DOWN, EM_GRP_2);
 *   EM_Step_Apply();                                       // 应用（自动选模式+映射+注册）
 * ============================================================ */

/**
 * @brief 设置某参数组的 吸合(S1)/释放(S2) 配置
 *
 * grp=EM_GRP_1 -> 写入 A 块 (EM_ACT_A_S1 / EM_ACT_A_S2)
 * grp=EM_GRP_2 -> 写入 B 块 (EM_ACT_B_S1 / EM_ACT_B_S2)
 * 调用顺序任意；EM_Step_Apply 之前至少设置一次。
 */
void EM_Step_SetGroup(uint8_t grp, const em_step_cfg_t *s1, const em_step_cfg_t *s2);

/**
 * @brief 注册一颗电磁铁（顺序即物理编号，最多 EM_MAX_MAGNETS 颗）
 *
 *  @param ch  通道 0=TIM8, 1=TIM1
 *  @param pos 上 / 下
 *  @param grp 参数组（对应 SetGroup 的 grp）
 */
void EM_Step_RegisterMagnet(uint8_t ch, em_pos_t pos, uint8_t grp);

/**
 * @brief 应用配置：自动选择模式(SINGLE/DUAL_SAME/DUAL_CROSS)、
 *        计算 (触发,通道) 映射、把所有动作注册进核心。
 *        须在 SetGroup / RegisterMagnet 之后调用一次。
 *        运行时修改参数后无需再调用，直接 EM_Step_SetConfig 即可。
 */
void EM_Step_Apply(void);

/**
 * @brief 读取某动作块当前配置（供 EEPROM 持久化与 HMI 读）
 */
const em_step_cfg_t* EM_Step_GetConfig(em_act_id_t act);

/**
 * @brief 写入某动作块配置并立即重新注册到其所有映射目标
 *        （HMI 修改 / EEPROM 载入后调用，下次触发即生效）
 */
void EM_Step_SetConfig(em_act_id_t act, const em_step_cfg_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* __ELECMGT_STEP_H__ */
