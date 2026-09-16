/* ============================================================
 * hbridge.h - H bridge driver
 *
 * Independent channel registration, burst DMA waveform output,
 * single-step hold mode, DMA completion callback.
 *
 * Design principle:
 *   The application ONLY specifies {state, duty} per frame.
 *   The H-bridge layer internally converts to safe 4-channel
 *   CCR data. Shoot-through is impossible by construction —
 *   the app never touches individual bridge arms.
 *
 * Hardware mapping:
 *   TIM8 CH1-4 -> PC6-PC9 (INA1-4) -> Bridge A
 *   TIM1 CH1-4 -> PA8-PA11 (INB1-4) -> Bridge B
 *   TIM1 (master) triggers TIM8 (slave) via ITR0 for sync
 *
 * Burst DMA: DCR configured for 4 transfers from CCR1 base.
 * One DMA channel (CC1) writes to DMAR, timer distributes
 * to CCR1-4 automatically.
 *
 * State Management:
 *   Each channel maintains its current state (g_ch_state[2]).
 *   Dead-time is automatically inserted on FORWARD <-> REVERSE transitions.
 *   Both DMA start and SetState calls check for dead-time.
 *
 * SDK 版：原工程 Hardware/hbridge，TIM 寄存器与 DMA 操作已 funnel 到
 * chip/oop_tim 与 chip/oop_dma（HAL 唯一入口），本模块不再直戳 TIMx->*
 * 或调用 HAL_TIM_*/HAL_DMA_*。HBRIDGE_* 名称保持，工程调用点零改动。
 * ============================================================ */

#ifndef __HBRIDGE_H__
#define __HBRIDGE_H__

#include <stdint.h>
#include <stdbool.h>
#include "hal_platform.h"   /* STM32 系列 HAL 统一入口（TIM/DMA 句柄类型） */

#ifdef __cplusplus
extern "C" {
#endif

/* ========== Config ========== */

#ifndef HBRIDGE_MAX_FRAMES
#define HBRIDGE_MAX_FRAMES  1000   /* max frames per DMA transfer */
#endif

/* ========== Bridge arm state ========== */
typedef enum {
    HBRIDGE_IDLE = 0,      /* all off */
    HBRIDGE_FORWARD,       /* HI1 + LO2 on, current forward */
    HBRIDGE_REVERSE,       /* HI2 + LO1 on, current reverse */
    HBRIDGE_BRAKE,         /* LO1 + LO2 on, both sides to ground */
} hbridge_state_t;

/* ========== Wave point (application-facing) ==========
 *
 * Each point = one PWM period (10us @ 100kHz).
 * The app fills an array of these and passes to StartWave.
 * The H-bridge converts to 4 CCR values internally.
 * ========== */
typedef struct {
    hbridge_state_t state;
    uint16_t        duty;   /* 0-100 (%) */
} hbridge_point_t;

/* ========== DMA completion callback ========== */
typedef void (*hbridge_dma_done_t)(uint8_t ch_id, void *user_data);

/* ========== Bridge channel (internal fields, do not touch) ========== */
typedef struct {
    uint8_t             id;                 /* 0 = TIM8 (bridge A), 1 = TIM1 (bridge B) */
    TIM_HandleTypeDef   *htim;
    DMA_HandleTypeDef   *hdma;              /* CC1 DMA, used for burst transfer */
    uint32_t            max_pulse;          /* PWM period - 1 */
    bool                registered;

    /* DMA waveform state */
    bool                is_streaming;
    uint16_t            wave_frame_count;   /* frames in current buffer */
    bool                wave_loop;          /* auto-restart on completion */

    /* Internal DMA buffer (converted from points, safe by construction) */
    uint32_t            dma_buffer[HBRIDGE_MAX_FRAMES][4];

    /* Callback */
    hbridge_dma_done_t  callback;
    void                *callback_user;
} hbridge_channel_t;

/* ========== Bridge controller ========== */
typedef struct {
    hbridge_channel_t   ch[2];              /* ch[0] = TIM8, ch[1] = TIM1 */
    uint8_t             registered_count;   /* 0, 1, or 2 */
    bool                sync_enabled;       /* hardware sync TIM1 -> TIM8 */
} hbridge_t;

/* ========== API ========== */

/**
 * @brief Initialize the bridge controller (empty, no channels)
 */
void HBRIDGE_Init(hbridge_t *hb);

/**
 * @brief Register a bridge channel independently
 *
 * Auto-detects TIM8 (ch_id=0) or TIM1 (ch_id=1) from htim.
 * Can be called once (single bridge) or twice (both bridges).
 * When both are registered, call HBRIDGE_EnableSync to bind them.
 *
 * @param hb          Bridge controller
 * @param htim        TIM handle (TIM1 or TIM8)
 * @param hdma_ch1    DMA for timer CH1 (used for burst DMA)
 * @param hdma_ch2    DMA for timer CH2 (reserved, not used in burst mode)
 * @param hdma_ch3    DMA for timer CH3 (reserved)
 * @param hdma_ch4    DMA for timer CH4 (reserved)
 * @param max_pulse   PWM period - 1 (e.g. 1699 for 100kHz @ 170MHz)
 * @return ch_id (0 or 1) on success, -1 on error
 */
int HBRIDGE_Register(hbridge_t *hb, TIM_HandleTypeDef *htim,
                     DMA_HandleTypeDef *hdma_ch1,
                     DMA_HandleTypeDef *hdma_ch2,
                     DMA_HandleTypeDef *hdma_ch3,
                     DMA_HandleTypeDef *hdma_ch4,
                     uint32_t max_pulse);

/**
 * @brief Enable/disable hardware sync (TIM1 master -> TIM8 slave)
 *
 * Only effective when both channels are registered.
 * When enabled, TIM1 update event triggers TIM8 start via ITR0.
 */
bool HBRIDGE_EnableSync(hbridge_t *hb, bool enable);

/**
 * @brief Start a single bridge (enable outputs + timer)
 */
void HBRIDGE_Start(hbridge_t *hb, uint8_t ch_id);

/**
 * @brief Stop a single bridge (disable timer, CCR = 0)
 */
void HBRIDGE_Stop(hbridge_t *hb, uint8_t ch_id);

/**
 * @brief Start all registered bridges
 *
 * If sync enabled and both registered: start with hardware sync.
 * Otherwise: start each independently.
 */
void HBRIDGE_StartAll(hbridge_t *hb);

/**
 * @brief Stop all registered bridges
 */
void HBRIDGE_StopAll(hbridge_t *hb);

/**
 * @brief Set constant output (single-step, no DMA, for hold mode)
 *
 * Writes 4 CCR registers directly. Aborts any running DMA first.
 * Automatically checks and inserts dead-time if state changes.
 *
 * @param hb      Bridge controller
 * @param ch_id   Channel ID (0 or 1)
 * @param state   Bridge arm state
 * @param duty    Duty cycle 0-100%
 */
void HBRIDGE_SetState(hbridge_t *hb, uint8_t ch_id,
                      hbridge_state_t state, uint16_t duty);

/**
 * @brief Synchronously set both channels' state
 *
 * Writes both timers' CCR registers simultaneously.
 * Uses synchronized CNT for <100ns precision.
 * Automatically checks and inserts dead-time for each channel.
 *
 * @param hb      Bridge controller
 * @param ch0_id  Channel 0 ID
 * @param state0  Channel 0 state
 * @param duty0   Channel 0 duty cycle 0-100%
 * @param ch1_id  Channel 1 ID
 * @param state1  Channel 1 state
 * @param duty1   Channel 1 duty cycle 0-100%
 */
void HBRIDGE_SetStateDualSync(hbridge_t *hb,
                              uint8_t ch0_id, hbridge_state_t state0, uint16_t duty0,
                              uint8_t ch1_id, hbridge_state_t state1, uint16_t duty1);

/**
 * @brief DMA waveform output (single-shot)
 *
 * Converts points to safe 4-CH CCR data internally, then streams
 * via burst DMA. Calls callback on completion.
 * Automatically checks and inserts dead-time from current state.
 *
 * @param hb      Bridge controller
 * @param ch_id   Channel ID
 * @param points  Wave point array (state + duty per frame)
 * @param count   Number of points (<= HBRIDGE_MAX_FRAMES)
 */
void HBRIDGE_StartWave(hbridge_t *hb, uint8_t ch_id,
                       const hbridge_point_t *points, uint16_t count);

/**
 * @brief Synchronously start both channels' DMA waveforms
 *
 * Enables both DMA channels simultaneously for <100ns sync precision.
 * Each channel converts its own points independently.
 * Each channel checks dead-time from its own current state.
 *
 * @param hb      Bridge controller
 * @param ch0_id  Channel 0 ID (usually 0)
 * @param pts0    Channel 0 wave points
 * @param count0  Channel 0 frame count
 * @param ch1_id  Channel 1 ID (usually 1)
 * @param pts1    Channel 1 wave points
 * @param count1  Channel 1 frame count
 * @return true   Success
 * @return false  Failure
 */
bool HBRIDGE_StartWaveDualSync(hbridge_t *hb,
                               uint8_t ch0_id, const hbridge_point_t *pts0, uint16_t count0,
                               uint8_t ch1_id, const hbridge_point_t *pts1, uint16_t count1);

/**
 * @brief DMA waveform output (loop, auto-restart on completion)
 */
void HBRIDGE_StartWaveLoop(hbridge_t *hb, uint8_t ch_id,
                           const hbridge_point_t *points, uint16_t count);

/**
 * @brief Stop DMA waveform, return to idle
 */
void HBRIDGE_StopWave(hbridge_t *hb, uint8_t ch_id);

/**
 * @brief Set DMA completion callback
 */
void HBRIDGE_SetCallback(hbridge_t *hb, uint8_t ch_id,
                         hbridge_dma_done_t callback, void *user_data);

/**
 * @brief Check if DMA is streaming
 */
bool HBRIDGE_IsStreaming(hbridge_t *hb, uint8_t ch_id);

/**
 * @brief Get current state of a channel (for debugging)
 */
hbridge_state_t HBRIDGE_GetState(uint8_t ch_id);

/**
 * @brief Get current duty of a channel (for debugging)
 */
uint16_t HBRIDGE_GetDuty(uint8_t ch_id);

/* ========== Utilities ========== */

/**
 * @brief Duty cycle (0-100%) to pulse value
 */
static inline uint32_t HBRIDGE_DutyToPulse(uint16_t duty, uint32_t max_pulse) {
    if (duty == 0) return 0;
    if (duty >= 100) return max_pulse;
    return (uint32_t)((uint64_t)duty * max_pulse / 100);
}

/**
 * @brief State to string
 */
const char* HBRIDGE_StateToString(hbridge_state_t state);

#ifdef __cplusplus
}
#endif

#endif /* __HBRIDGE_H__ */
