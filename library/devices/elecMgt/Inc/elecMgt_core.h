/* ============================================================
 * elecMgt_core.h - Electromagnet core (通用核心)
 *
 * 职责：负责把「动作源」生成为 H 桥可直接 DMA 的 point。
 *
 *  - em_frame_t   : step 与 core 之间的契约（参数→帧）
 *  - em_point_t   : = hbridge_point_t（复用，不重定义）
 *  - em_action_t  : 双缓冲，来源无关（step 走帧 / pid 直出点）
 *  - em_frame_to_points() : frame -> point 生成引擎（核心职责）
 *
 * 本模块只关心「生成 point + 触发 DMA」，不关心参数怎么来的。
 * 参数来源由 step（参数→帧）和 pid（预留，参数→点）两个模块提供。
 *
 * 典型用例:
 *   EM_Init(EM_MODE_DUAL_SAME, &hw);              // hw: TIM/DMA 句柄绑定
 *   em_step_register(EM_TRIGGER_IN1, 0, &cfg);   // step 路径
 *   EM_Trigger(EM_TRIGGER_IN1);                  // 零开销触发
 *
 * SDK 版（迁入 devices/elecMgt）：只依赖 chip.hbridge + chip.oop_dwt；
 * TIM/DMA 句柄由工程 EM_Init 时注入，模块内不 extern 任何全局句柄。
 * ============================================================ */

#ifndef __ELECMGT_CORE_H__
#define __ELECMGT_CORE_H__

#include "hbridge.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 配置宏
 * ============================================================ */

#ifndef EM_DUAL_PARAM_COUNT
#define EM_DUAL_PARAM_COUNT  2   /* 1=双通道共用参数, 2=双通道各自独立 */
#endif

#define EM_MAX_CHANNELS       2
#define EM_DEFAULT_TIMEOUT_US 100000

/* ============================================================
 * 枚举类型
 * ============================================================ */

typedef enum {
    EM_MODE_SINGLE = 0,          /**< 单通道模式: 仅使用通道0 (TIM8) */
    EM_MODE_DUAL_SAME,           /**< 双通道同向模式: 共用参数，同时启动 */
    EM_MODE_DUAL_CROSS,          /**< 双通道交叉模式: 共用参数，交叉映射，同时启动 */
} em_mode_t;

typedef enum {
    EM_TRIGGER_IN1 = 0,          /**< 触发源1 (上升/吸合动作) */
    EM_TRIGGER_IN2,              /**< 触发源2 (下降/释放动作) */
    EM_TRIGGER_MAX,
} em_trigger_t;

/**
 * @brief 电磁铁物理位置
 *
 * 同时决定该电磁铁是否为“保持型”：
 *   - 上(EM_POS_UP)  = 保持型：动作/上电后停在末态(energized)，
 *                      保持态由 IN1(吸合/上升) 的保持帧定义。
 *   - 下(EM_POS_DOWN) = 非保持型：动作/上电后回到 idle(去磁)，
 *                      不锁存末态（弹簧/重力回位）。
 * （IN1/IN2 全局方向固定为“上升/下降”，位置改“谁保持/是否保持”）
 */
typedef enum {
    EM_POS_UP = 0,               /**< 电磁铁在上方：保持型（上电停在上方） */
    EM_POS_DOWN,                 /**< 电磁铁在下方：非保持型（上电回到 idle） */
} em_pos_t;

/**
 * @brief 单颗电磁铁的注册信息（用户只需填这个）
 *
 * 模式(单/双同向/双交叉)由驱动据本结构数组自动选择，用户无需指定。
 *   - ch  : 通道 0=TIM8, 1=TIM1
 *   - pos : 上 / 下（同时决定 hold：上=保持，下=不保持）
 *   - grp : 参数组 0=组1(A_S1/A_S2), 1=组2(B_S1/B_S2)
 *   - hold: 是否保持型。默认由 pos 推导（上=保持/下=不保持），
 *           注册时写入；上电保持态与动作完成后保持行为据此决定。
 */
typedef struct {
    uint8_t  ch;
    em_pos_t pos;
    uint8_t  grp;
    bool     hold;   /**< 保持型=true(锁存末态) / 非保持型=false(回 idle) */
} em_magnet_reg_t;

typedef enum {
    EM_STATE_DISABLED = 0,
    EM_STATE_IDLE,
    EM_STATE_RUNNING,
    EM_STATE_ERROR,
} em_state_t;

/* ============================================================
 * 硬件绑定（TIM/DMA 句柄由工程注入，SDK 不 extern 全局句柄）
 * ============================================================ */

/**
 * @brief 单路 H 桥的硬件绑定
 *
 *   - ch0 = TIM8（桥 A），ch1 = TIM1（桥 B）；未使用的桥把 htim 置 NULL 即可
 *   - hdma_ch2..4 为 HBRIDGE_Register 预留参数（突发 DMA 实际只用 ch1）
 *   - max_pulse = PWM 周期 - 1（如 100kHz @170MHz → 1699）
 */
typedef struct {
    TIM_HandleTypeDef *htim;
    DMA_HandleTypeDef *hdma_ch1;
    DMA_HandleTypeDef *hdma_ch2;
    DMA_HandleTypeDef *hdma_ch3;
    DMA_HandleTypeDef *hdma_ch4;
    uint32_t           max_pulse;
} em_bridge_hw_t;

typedef struct {
    em_bridge_hw_t ch[EM_MAX_CHANNELS];
} em_hw_t;

/* ============================================================
 * 数据结构
 * ============================================================ */

/**
 * @brief 动作帧（step 与 core 之间的契约）
 *
 * time_us = 0 表示保持帧，展开为 1 帧 point，硬件自动保持。
 * 保持帧必须是动作序列的最后一帧。
 * state 直接复用 hbridge_state_t，不重复定义。
 */
typedef struct {
    uint16_t        time_us;     /**< 帧持续时间(微秒)，0表示保持帧 */
    uint8_t         duty;        /**< 占空比 0-100 (%) */
    hbridge_state_t state;       /**< H桥状态: FWD/REV/BRAKE/IDLE */
} em_frame_t;

/** point = hbridge 的点，复用其类型，不重定义 */
typedef hbridge_point_t em_point_t;

/**
 * @brief 动作定义（双缓冲 + 来源无关）
 *
 * 两种来源路径：
 *   use_points = false : step 路径，由 frames 经 em_frame_to_points 生成
 *   use_points = true  : pid 路径，由 points 直接注入（线性生成，跳过帧）
 */
typedef struct {
    /* step 路径 */
    const em_frame_t *frames;
    uint8_t           count;
    /* pid 路径 */
    const em_point_t *points;
    uint16_t          point_count;
    bool              use_points;

    /* 双缓冲（与来源无关） */
    bool              ready;
    uint8_t           active_idx;
    uint16_t          buf_count[2];
    em_point_t        buf[2][HBRIDGE_MAX_FRAMES];

    /* 保持帧（最后一个 time_us==0 的 step）：定义保持态的方向+占空比 */
    bool              has_hold;        /* 该动作是否带保持帧 */
    hbridge_state_t   hold_state;     /* 保持态方向 */
    uint8_t           hold_duty;      /* 保持态占空比 */
} em_action_t;

/* ============================================================
 * 核心 API
 * ============================================================ */

/**
 * @brief 初始化核心（清零 + 注册两路 H 桥 + 通道 + 回调）
 *
 * @param mode 通道模式（SINGLE / DUAL_SAME / DUAL_CROSS）
 * @param hw   TIM/DMA 句柄绑定（由工程 board_cfg 提供；NULL 则不注册 H 桥，
 *             仅供纯逻辑自测用）
 */
void EM_Init(em_mode_t mode, const em_hw_t *hw);
void EM_ConfigMagnets(const em_magnet_reg_t *mags, uint8_t count, bool same_params);
void EM_Poll(void);
void EM_Trigger(em_trigger_t src);
void EM_RestoreDefaults(void);    /**< 恢复各通道默认态（上电/反弹错误后调用） */

/** 注册 EM 触发钩子：在 EM_Trigger 实际启动动作前回调（用于波形/反弹检测同步开窗）。
 *  cb 收到触发源（IN1/IN2）；传 NULL 取消。 */
void EM_SetTriggerHook(void (*cb)(em_trigger_t src));

/* step 路径：注册帧序列 */
void EM_UpdateAction(em_trigger_t src, uint8_t ch_id,
                     const em_frame_t *frames, uint8_t count);
void EM_RegisterAction(em_trigger_t src, uint8_t ch_id,
                       const em_frame_t *frames, uint8_t count);

/* pid 路径：直接注册 point（线性生成时使用） */
void EM_UpdateActionPoints(em_trigger_t src, uint8_t ch_id,
                           const em_point_t *points, uint16_t count);

void EM_Stop(void);
void EM_Enable(uint8_t ch_id, bool enable);
em_state_t EM_GetState(uint8_t ch_id);
bool EM_IsIdle(uint8_t ch_id);
bool EM_IsBusy(void);

/* 通道激活判定：已注册磁铁（em_magnet_for_ch != NULL）即激活；
 * 未注册 = 掩码（无输出波形、不参与反弹检测、恢复默认态时跳过）。*/
bool EM_IsChannelActive(uint8_t ch_id);
/* 通道电磁铁位置（上/下）；未注册返回 EM_POS_DOWN。供反弹窗口按身份开窗使用。*/
em_pos_t EM_GetChannelPos(uint8_t ch_id);
hbridge_t* EM_GetHBridge(void);

/* ============================================================
 * 动作锁定：反弹错误时禁止一切触发（IO + 模拟）
 *  - EM_SetActionLock(true)  后 EM_Trigger 直接返回，不启动动作
 *  - EM_SetActionLock(false) 恢复正常触发
 *  - 供 task_waveform 在反弹错误时调用，用户清除错误标记后解锁
 * ============================================================ */
void EM_SetActionLock(bool lock);
bool EM_IsActionLocked(void);

/* ============================================================
 * 核心生成引擎：frame -> point
 *
 * 这是「通用核心负责生成 point」的唯一实现点。
 * step / pid 模块都可复用本函数（step 必走，pid 直出点时可跳过）。
 * ============================================================ */
uint16_t em_frame_to_points(const em_frame_t *frames, uint8_t count,
                            em_point_t *points, uint16_t max_size);

#ifdef __cplusplus
}
#endif

#endif /* __ELECMGT_CORE_H__ */
