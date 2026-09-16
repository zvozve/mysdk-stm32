/* ============================================================
 * hbridge.c - H bridge driver implementation
 *
 * Key design decisions:
 * 1. Application provides only {state, duty} points.
 *    H-bridge converts to 4-CH CCR data internally.
 *    Shoot-through is impossible by construction.
 * 2. Burst DMA via DMAR register (DCR = CCR1 base + 4 transfers)
 *    - One DMA channel (CC1) updates all 4 CCR per frame
 * 3. SetState writes CCR directly (no DMA, no Stop/Start cycle)
 * 4. Independent channel registration, sync auto-detected
 * 5. DMA completion callback with loop restart support
 * 6. Automatic dead-time insertion on state change (FORWARD <-> REVERSE)
 * 7. Each channel maintains its own current state for dead-time checking
 *
 * SDK 版（重构）：TIM 寄存器与 DMA 操作全部 funnel 到 chip/oop_tim 与
 * chip/oop_dma，本文件不再直戳 TIMx->* 或调用 HAL_DMA_* / HAL_TIM_*，
 * 满足 "oop_tim 是 HAL 唯一入口" 的设计铁律。公共 API 保持不变。
 * ============================================================ */

#include "hbridge.h"
#include <string.h>
#include "SEGGER_RTT_Log.h"
#include "oop_tim_drv.h"    /* TIM 寄存器封装（HAL 唯一入口） */
#include "oop_dma_drv.h"    /* DMA 封装（HAL_DMA_* 唯一入口） */

/* ========== CCR index mapping (internal) ==========
 *
 * Timer channel -> CCR register -> H-bridge arm:
 *   CH1 -> CCR1 -> HI1  (high side, arm 1)
 *   CH2 -> CCR2 -> HI2  (high side, arm 2)
 *   CH3 -> CCR3 -> LO1  (low side,  arm 1)
 *   CH4 -> CCR4 -> LO2  (low side,  arm 2)
 *
 * Frame layout for burst DMA (CCR sequential):
 *   frame[0] = CCR1 = HI1
 *   frame[1] = CCR2 = HI2
 *   frame[2] = CCR3 = LO1
 *   frame[3] = CCR4 = LO2
 * ========== */
#define IDX_HI1  0   /* CCR1 */
#define IDX_HI2  1   /* CCR2 */
#define IDX_LO1  2   /* CCR3 */
#define IDX_LO2  3   /* CCR4 */

/* ========== 主从同步拓扑（板无关） ==========
 *
 * 同步时不引用任何具体 TIMx 外设名，只用"注册槽位"表达：
 *   后注册槽位(ch1) = master，输出 TRGO（TIM_CR2_MMS_1 = Update）
 *   先注册槽位(ch0) = slave，由 master 的 TRGO 启动（ITR0 / 外部时钟模式1）
 *
 * ⚠ 工程侧约定：EMHelpers 注册顺序为 ch0=从、ch1=主（当前板 ch0=TIM8、ch1=TIM1，
 *   见 User/board_cfg.h 的 BOARD_EM_CHx_*）。换板只要保持"主在后注册"即可，
 *   不需改动本驱动。ITR 触发线到外设的映射由芯片参考手册决定（此处固定 ITR0）。
 */
#define HB_SYNC_TS  (0x04U << TIM_SMCR_TS_Pos)   /* ITR0 */
#define HB_SYNC_SMS (0x06U << TIM_SMCR_SMS_Pos)  /* External Clock Mode 1 */
#define HB_SYNC_MASTER_SLOT  1
#define HB_SYNC_SLAVE_SLOT   0

/* ========== Per-channel current state tracking ========== */
static hbridge_state_t g_ch_state[2] = {HBRIDGE_IDLE, HBRIDGE_IDLE};
static uint16_t g_ch_duty[2] = {0, 0};

/* ========== Fill frame: state + duty -> 4 CCR values (internal) ==========
 *
 * This is the ONLY function that writes bridge arm combinations.
 * By construction, HI1+LO1 and HI2+LO2 are never both on.
 * ========== */
static void hbridge_fill_frame(uint32_t frame[4], hbridge_state_t state,
                                uint32_t pulse, uint32_t max_pulse) {
    switch (state) {
        case HBRIDGE_FORWARD:
            /* HI1 = pulse (PWM), LO2 = max (always on, current return path) */
            frame[IDX_HI1] = pulse;
            frame[IDX_HI2] = 0;
            frame[IDX_LO1] = 0;
            frame[IDX_LO2] = max_pulse;
            break;
        case HBRIDGE_REVERSE:
            /* HI2 = pulse (PWM), LO1 = max (always on, current return path) */
            frame[IDX_HI1] = 0;
            frame[IDX_HI2] = pulse;
            frame[IDX_LO1] = max_pulse;
            frame[IDX_LO2] = 0;
            break;
        case HBRIDGE_BRAKE:
            /* LO1 = max, LO2 = max (both sides to ground, no HI on) */
            frame[IDX_HI1] = 0;
            frame[IDX_HI2] = 0;
            frame[IDX_LO1] = max_pulse;
            frame[IDX_LO2] = max_pulse;
            break;
        case HBRIDGE_IDLE:
        default:
            frame[IDX_HI1] = 0;
            frame[IDX_HI2] = 0;
            frame[IDX_LO1] = 0;
            frame[IDX_LO2] = 0;
            break;
    }
}

/* ========== Debug assert: verify no shoot-through ========== */
static bool hbridge_verify_safe(const uint32_t frame[4]) {
    /* Side 1: HI1 + LO1 */
    if (frame[IDX_HI1] > 0 && frame[IDX_LO1] > 0) {
        DBG_LOG("HB ASSERT: HI1(%lu) & LO1(%lu) both on!",
                frame[IDX_HI1], frame[IDX_LO1]);
        return false;
    }
    /* Side 2: HI2 + LO2 */
    if (frame[IDX_HI2] > 0 && frame[IDX_LO2] > 0) {
        DBG_LOG("HB ASSERT: HI2(%lu) & LO2(%lu) both on!",
                frame[IDX_HI2], frame[IDX_LO2]);
        return false;
    }
    return true;
}

/* ========== 检测状态是否改变（需要死区） ========== */
static bool hbridge_state_changed(hbridge_state_t prev, hbridge_state_t curr) {
    /* 只有 FORWARD <-> REVERSE 需要死区 */
    if (prev == HBRIDGE_FORWARD && curr == HBRIDGE_REVERSE) return true;
    if (prev == HBRIDGE_REVERSE && curr == HBRIDGE_FORWARD) return true;
    return false;
}

/* ========== 生成过渡帧（所有臂都写0） ========== */
static void hbridge_fill_transition_frame(uint32_t frame[4]) {
    frame[IDX_HI1] = 0;
    frame[IDX_HI2] = 0;
    frame[IDX_LO1] = 0;
    frame[IDX_LO2] = 0;
}

/* ========== 更新通道状态记录 ========== */
static void hbridge_update_state(uint8_t ch_id, hbridge_state_t state, uint16_t duty) {
    if (ch_id >= 2) return;
    g_ch_state[ch_id] = state;
    g_ch_duty[ch_id] = duty;
    DBG_LOG("HB CH%d: state updated to %s %d%%",
            ch_id, HBRIDGE_StateToString(state), duty);
}

/* ========== Convert points -> internal DMA buffer ==========
 *
 * Converts app-facing {state, duty} points to 4-CH CCR frames.
 * Automatically inserts dead-time frames when state changes.
 * Uses per-channel current state for first frame dead-time check.
 * Updates channel state to last frame after conversion.
 * ========== */
static uint16_t hbridge_convert_points(hbridge_channel_t *ch,
                                       const hbridge_point_t *points,
                                       uint16_t count) {
    uint16_t n = (count > HBRIDGE_MAX_FRAMES) ? HBRIDGE_MAX_FRAMES : count;

    uint16_t write_idx = 0;
    uint8_t ch_id = ch->id;

    /* ★ 从记录的当前状态开始检查首帧死区 */
    hbridge_state_t prev_state = g_ch_state[ch_id];

    for (uint16_t i = 0; i < n && write_idx < HBRIDGE_MAX_FRAMES; i++) {
        /* ★ 检查状态变化，插入死区 */
        if (hbridge_state_changed(prev_state, points[i].state)) {
            if (write_idx < HBRIDGE_MAX_FRAMES) {
                hbridge_fill_transition_frame(ch->dma_buffer[write_idx]);
                write_idx++;
            }
        }

        if (write_idx >= HBRIDGE_MAX_FRAMES) break;

        uint32_t pulse = HBRIDGE_DutyToPulse(points[i].duty, ch->max_pulse);
        hbridge_fill_frame(ch->dma_buffer[write_idx], points[i].state,
                          pulse, ch->max_pulse);

        /* 安全检查 */
        if (!hbridge_verify_safe(ch->dma_buffer[write_idx])) {
            DBG_LOG("HB CH%d: point %d unsafe, forced IDLE", ch_id, i);
            hbridge_fill_transition_frame(ch->dma_buffer[write_idx]);
        }

        write_idx++;
        prev_state = points[i].state;
    }

    /* ★ 记录最后一帧状态（DMA结束后会保持这个状态） */
    if (n > 0) {
        hbridge_update_state(ch_id, points[n-1].state, points[n-1].duty);
    }

    return write_idx;
}

/* ========== DMA completion callback (internal) ========== */
static void hbridge_dma_callback(void *user) {
    hbridge_channel_t *ch = (hbridge_channel_t *)user;
    if (ch == NULL) return;

    if (ch->wave_loop && ch->wave_frame_count > 0) {
        /* Loop mode: restart DMA on internal buffer immediately */
        oop_tim_dmar_burst_start(ch->htim, ch->hdma,
                                 (uint32_t)ch->dma_buffer,
                                 (uint32_t)ch->wave_frame_count * 4);
        return;
    }

    /* Single-shot mode: streaming done */
    ch->is_streaming = false;
    ch->wave_frame_count = 0;

    if (ch->callback) {
        ch->callback(ch->id, ch->callback_user);
    }
}

/* ========== Write CCR registers directly (via oop_tim) ========== */
static void hbridge_write_ccr(hbridge_channel_t *ch, const uint32_t frame[4]) {
    oop_tim_ccr_write_all(ch->htim,
                          frame[IDX_HI1], frame[IDX_HI2],
                          frame[IDX_LO1], frame[IDX_LO2]);
}

/* ========== Channel init ========== */
static void hbridge_ch_init(hbridge_channel_t *ch, uint8_t id,
                            TIM_HandleTypeDef *htim,
                            DMA_HandleTypeDef *hdma_ch1,
                            uint32_t max_pulse) {
    ch->id = id;
    ch->htim = htim;
    ch->hdma = hdma_ch1;
    ch->max_pulse = max_pulse;
    ch->registered = true;
    ch->is_streaming = false;
    ch->wave_frame_count = 0;
    ch->wave_loop = false;
    ch->callback = NULL;
    ch->callback_user = NULL;

    /* --- Configure DCR for burst DMA (via oop_tim) --- */
    oop_tim_dmar_config(htim, TIM_DMABASE_CCR1, TIM_DMABURSTLENGTH_4TRANSFERS);

    /* --- Enable CCR preload (via oop_tim) --- */
    oop_tim_oc_preload_enable(htim);

    /* --- DMA request setup: only CC1DE for burst DMA (via oop_tim) --- */
    oop_tim_dma_burst_req_enable(htim);

    /* --- Register DMA callback (user = channel, via oop_dma) --- */
    oop_dma_register_callback(hdma_ch1, hbridge_dma_callback, ch);

    /* --- Safe initial state: all CCR = 0 (via oop_tim) --- */
    oop_tim_ccr_write_all(htim, 0, 0, 0, 0);

    /* ★ 初始化状态记录 */
    g_ch_state[id] = HBRIDGE_IDLE;
    g_ch_duty[id] = 0;

    DBG_LOG("HB CH%d: registered (TIM%d, max_pulse=%lu)",
            id, (id == 0) ? 8 : 1, max_pulse);
}

/* ========== Public API ========== */

void HBRIDGE_Init(hbridge_t *hb) {
    memset(hb, 0, sizeof(hbridge_t));
    hb->registered_count = 0;
    hb->sync_enabled = false;

    /* ★ 初始化状态 */
    g_ch_state[0] = HBRIDGE_IDLE;
    g_ch_state[1] = HBRIDGE_IDLE;
    g_ch_duty[0] = 0;
    g_ch_duty[1] = 0;
}

int HBRIDGE_Register(hbridge_t *hb, TIM_HandleTypeDef *htim,
                     DMA_HandleTypeDef *hdma_ch1,
                     DMA_HandleTypeDef *hdma_ch2,
                     DMA_HandleTypeDef *hdma_ch3,
                     DMA_HandleTypeDef *hdma_ch4,
                     uint32_t max_pulse) {
    if (!hb || !htim || !hdma_ch1) return -1;

    uint8_t ch_id;
    if (htim->Instance == TIM8) {
        ch_id = 0;
    } else if (htim->Instance == TIM1) {
        ch_id = 1;
    } else {
        DBG_LOG("HB: unsupported timer instance");
        return -1;
    }

    if (hb->ch[ch_id].registered) {
        DBG_LOG("HB CH%d: already registered", ch_id);
        return -1;
    }

    (void)hdma_ch2;
    (void)hdma_ch3;
    (void)hdma_ch4;

    hbridge_ch_init(&hb->ch[ch_id], ch_id, htim, hdma_ch1, max_pulse);
    hb->registered_count++;

    DBG_LOG("HB: %d channel(s) registered", hb->registered_count);
    return ch_id;
}

bool HBRIDGE_EnableSync(hbridge_t *hb, bool enable) {
    if (!hb) return false;

    if (enable && hb->registered_count < 2) {
        DBG_LOG("HB: sync requires 2 channels, have %d", hb->registered_count);
        return false;
    }

    hb->sync_enabled = enable;

    /* ★ 同步拓扑：后注册槽位作 master（发 TRGO），先注册槽位作 slave（触发启动）。
     * 具体是哪个 TIM 由工程注册时注入，本模块不引用任何 TIMx 外设名。
     * 注：TS/SMS 取值按 ST 参考手册的 ITR 触发映射（见 HB_SYNC_TS 定义处注释）。 */
    TIM_HandleTypeDef *hm = hb->ch[HB_SYNC_MASTER_SLOT].htim;
    TIM_HandleTypeDef *hs = hb->ch[HB_SYNC_SLAVE_SLOT].htim;

    if (enable) {
        oop_tim_master_mode_set(hm, TIM_CR2_MMS_1);
        oop_tim_slave_mode_set(hs, HB_SYNC_TS, HB_SYNC_SMS);
        DBG_LOG("HB: sync ON (ch%d -> ch%d)", HB_SYNC_MASTER_SLOT, HB_SYNC_SLAVE_SLOT);
    } else {
        oop_tim_slave_mode_set(hs, 0, 0);
        DBG_LOG("HB: sync OFF");
    }

    return true;
}

void HBRIDGE_Start(hbridge_t *hb, uint8_t ch_id) {
    if (ch_id >= 2 || !hb->ch[ch_id].registered) return;

    hbridge_channel_t *ch = &hb->ch[ch_id];

    oop_tim_outputs_enable(ch->htim);

    /* 单独启动时若该槽位是同步拓扑的 slave，先解除从模式，否则等不到 TRGO */
    if (ch_id == HB_SYNC_SLAVE_SLOT) {
        oop_tim_slave_mode_set(ch->htim, 0, 0);
    }

    oop_tim_counter_enable(ch->htim);

    DBG_LOG("HB CH%d: started", ch_id);
}

void HBRIDGE_Stop(hbridge_t *hb, uint8_t ch_id) {
    if (ch_id >= 2 || !hb->ch[ch_id].registered) return;

    hbridge_channel_t *ch = &hb->ch[ch_id];

    if (ch->is_streaming && ch->hdma) {
        oop_dma_abort(ch->hdma);
    }
    ch->is_streaming = false;
    ch->wave_frame_count = 0;
    ch->wave_loop = false;

    oop_tim_counter_disable(ch->htim);
    oop_tim_ccr_write_all(ch->htim, 0, 0, 0, 0);

    /* ★ 停止时状态变为 IDLE */
    g_ch_state[ch_id] = HBRIDGE_IDLE;
    g_ch_duty[ch_id] = 0;

    DBG_LOG("HB CH%d: stopped", ch_id);
}

void HBRIDGE_StartAll(hbridge_t *hb) {
    if (!hb || hb->registered_count == 0) return;

    if (hb->sync_enabled && hb->registered_count == 2) {
        for (uint8_t i = 0; i < 2; i++) {
            oop_tim_outputs_enable(hb->ch[i].htim);
        }

        TIM_HandleTypeDef *hm = hb->ch[HB_SYNC_MASTER_SLOT].htim;
        TIM_HandleTypeDef *hs = hb->ch[HB_SYNC_SLAVE_SLOT].htim;

        oop_tim_master_mode_set(hm, TIM_CR2_MMS_1);
        oop_tim_slave_mode_set(hs, HB_SYNC_TS, HB_SYNC_SMS);

        /* 先 slave 后 master：slave 已在等 TRGO，master 使能后立即同步起跑 */
        oop_tim_counter_enable(hs);
        oop_tim_counter_enable(hm);

        /* 复位 CNT 保证同相位（master 最后一次复位重发 TRGO） */
        oop_tim_counter_reset(hs);
        oop_tim_counter_reset(hm);

        DBG_LOG("HB: sync start (master=ch%d, slave=ch%d)",
                HB_SYNC_MASTER_SLOT, HB_SYNC_SLAVE_SLOT);
    } else {
        for (uint8_t i = 0; i < 2; i++) {
            if (hb->ch[i].registered) {
                HBRIDGE_Start(hb, i);
            }
        }
    }
}

void HBRIDGE_StopAll(hbridge_t *hb) {
    if (!hb) return;
    for (uint8_t i = 0; i < 2; i++) {
        if (hb->ch[i].registered) {
            HBRIDGE_Stop(hb, i);
        }
    }
    DBG_LOG("HB: all stopped");
}

void HBRIDGE_SetState(hbridge_t *hb, uint8_t ch_id,
                      hbridge_state_t state, uint16_t duty) {
    if (ch_id >= 2 || !hb->ch[ch_id].registered) return;

    hbridge_channel_t *ch = &hb->ch[ch_id];

    if (ch->is_streaming && ch->hdma) {
        oop_dma_abort(ch->hdma);
    }
    ch->is_streaming = false;
    ch->wave_frame_count = 0;
    ch->wave_loop = false;

    /* ★ 检查从当前状态到目标状态是否需要死区 */
    if (hbridge_state_changed(g_ch_state[ch_id], state)) {
        uint32_t idle_frame[4] = {0, 0, 0, 0};
        hbridge_write_ccr(ch, idle_frame);
    }

    uint32_t pulse = HBRIDGE_DutyToPulse(duty, ch->max_pulse);
    uint32_t frame[4];
    hbridge_fill_frame(frame, state, pulse, ch->max_pulse);

    if (!hbridge_verify_safe(frame)) {
        DBG_LOG("HB CH%d: SetState unsafe, forced IDLE", ch_id);
        hbridge_fill_transition_frame(frame);
    }

    hbridge_write_ccr(ch, frame);

    /* ★ 更新状态记录 */
    g_ch_state[ch_id] = state;
    g_ch_duty[ch_id] = duty;

    DBG_LOG("HB CH%d: SetState %s %d%%",
            ch_id, HBRIDGE_StateToString(state), duty);
}

void HBRIDGE_SetStateDualSync(hbridge_t *hb,
                              uint8_t ch0_id, hbridge_state_t state0, uint16_t duty0,
                              uint8_t ch1_id, hbridge_state_t state1, uint16_t duty1) {
    if (!hb) return;

    hbridge_channel_t *ch0 = &hb->ch[ch0_id];
    hbridge_channel_t *ch1 = &hb->ch[ch1_id];

    if (!ch0->registered || !ch1->registered) return;

    if (ch0->is_streaming && ch0->hdma) {
        oop_dma_abort(ch0->hdma);
        ch0->is_streaming = false;
    }
    if (ch1->is_streaming && ch1->hdma) {
        oop_dma_abort(ch1->hdma);
        ch1->is_streaming = false;
    }

    /* ★ 检查两个通道是否需要死区 */
    bool need_dead0 = hbridge_state_changed(g_ch_state[ch0_id], state0);
    bool need_dead1 = hbridge_state_changed(g_ch_state[ch1_id], state1);

    if (need_dead0 || need_dead1) {
        uint32_t idle_frame[4] = {0, 0, 0, 0};
        hbridge_write_ccr(ch0, idle_frame);
        hbridge_write_ccr(ch1, idle_frame);

        /* ★ 中间状态变为 IDLE */
        g_ch_state[ch0_id] = HBRIDGE_IDLE;
        g_ch_state[ch1_id] = HBRIDGE_IDLE;
    }

    uint32_t pulse0 = HBRIDGE_DutyToPulse(duty0, ch0->max_pulse);
    uint32_t pulse1 = HBRIDGE_DutyToPulse(duty1, ch1->max_pulse);

    uint32_t frame0[4], frame1[4];
    hbridge_fill_frame(frame0, state0, pulse0, ch0->max_pulse);
    hbridge_fill_frame(frame1, state1, pulse1, ch1->max_pulse);

    if (!hbridge_verify_safe(frame0)) {
        DBG_LOG("HB CH%d: SetStateDualSync unsafe, forced IDLE", ch0_id);
        hbridge_fill_transition_frame(frame0);
    }
    if (!hbridge_verify_safe(frame1)) {
        DBG_LOG("HB CH%d: SetStateDualSync unsafe, forced IDLE", ch1_id);
        hbridge_fill_transition_frame(frame1);
    }

    hbridge_write_ccr(ch0, frame0);
    hbridge_write_ccr(ch1, frame1);

    ch0->wave_frame_count = 0;
    ch0->wave_loop = false;
    ch1->wave_frame_count = 0;
    ch1->wave_loop = false;

    /* ★ 更新状态记录 */
    g_ch_state[ch0_id] = state0;
    g_ch_duty[ch0_id] = duty0;
    g_ch_state[ch1_id] = state1;
    g_ch_duty[ch1_id] = duty1;

    DBG_LOG("HB: dual state sync (CH0: %s %d%%, CH1: %s %d%%)",
            HBRIDGE_StateToString(state0), duty0,
            HBRIDGE_StateToString(state1), duty1);
}

/* ========== DMA waveform output ========== */

static void hbridge_start_wave_internal(hbridge_t *hb, uint8_t ch_id,
                                        const hbridge_point_t *points,
                                        uint16_t count, bool loop) {
    if (ch_id >= 2 || !hb->ch[ch_id].registered) return;
    if (!points || count == 0) {
        DBG_LOG("HB CH%d: invalid wave params", ch_id);
        return;
    }

    hbridge_channel_t *ch = &hb->ch[ch_id];

    if (count > HBRIDGE_MAX_FRAMES) {
        DBG_LOG("HB CH%d: count %d > max %d, truncated",
                ch_id, count, HBRIDGE_MAX_FRAMES);
        count = HBRIDGE_MAX_FRAMES;
    }

    if (ch->is_streaming && ch->hdma) {
        oop_dma_abort(ch->hdma);
    }

    /* ★ hbridge_convert_points 内部会处理死区并更新 g_ch_state */
    uint16_t n = hbridge_convert_points(ch, points, count);
    if (n == 0) {
        DBG_LOG("HB CH%d: no valid frames", ch_id);
        return;
    }

    ch->wave_frame_count = n;
    ch->wave_loop = loop;
    ch->is_streaming = true;

    oop_tim_dmar_burst_start(ch->htim, ch->hdma,
                             (uint32_t)ch->dma_buffer,
                             (uint32_t)n * 4);

    DBG_LOG("HB CH%d: wave %s, %d frames",
            ch_id, loop ? "LOOP" : "SINGLE", n);
}

void HBRIDGE_StartWave(hbridge_t *hb, uint8_t ch_id,
                       const hbridge_point_t *points, uint16_t count) {
    hbridge_start_wave_internal(hb, ch_id, points, count, false);
}

bool HBRIDGE_StartWaveDualSync(hbridge_t *hb,
                               uint8_t ch0_id, const hbridge_point_t *pts0, uint16_t count0,
                               uint8_t ch1_id, const hbridge_point_t *pts1, uint16_t count1) {
    if (!hb || ch0_id >= 2 || ch1_id >= 2) return false;

    hbridge_channel_t *ch0 = &hb->ch[ch0_id];
    hbridge_channel_t *ch1 = &hb->ch[ch1_id];

    if (!ch0->registered || !ch1->registered) return false;
    if (!pts0 || !pts1 || count0 == 0 || count1 == 0) return false;

    if (count0 > HBRIDGE_MAX_FRAMES) count0 = HBRIDGE_MAX_FRAMES;
    if (count1 > HBRIDGE_MAX_FRAMES) count1 = HBRIDGE_MAX_FRAMES;

    /* ★ 各自转换，各自检查死区（使用各自的 g_ch_state） */
    uint16_t n0 = hbridge_convert_points(ch0, pts0, count0);
    uint16_t n1 = hbridge_convert_points(ch1, pts1, count1);
    if (n0 == 0 || n1 == 0) return false;

    /* 统一走 oop_tim_dmar_burst_start（HAL_DMA_Start_IT + DMAR 目标），不再裸写 DMA_Channel */
    oop_dma_abort(ch0->hdma);
    oop_tim_dmar_burst_start(ch0->htim, ch0->hdma,
                             (uint32_t)ch0->dma_buffer,
                             (uint32_t)n0 * 4);

    oop_dma_abort(ch1->hdma);
    oop_tim_dmar_burst_start(ch1->htim, ch1->hdma,
                             (uint32_t)ch1->dma_buffer,
                             (uint32_t)n1 * 4);

    ch0->wave_frame_count = n0;
    ch0->wave_loop = false;
    ch0->is_streaming = true;

    ch1->wave_frame_count = n1;
    ch1->wave_loop = false;
    ch1->is_streaming = true;

    DBG_LOG("HB: dual DMA sync started (CH0=%d, CH1=%d)", n0, n1);
    return true;
}

void HBRIDGE_StartWaveLoop(hbridge_t *hb, uint8_t ch_id,
                           const hbridge_point_t *points, uint16_t count) {
    hbridge_start_wave_internal(hb, ch_id, points, count, true);
}

void HBRIDGE_StopWave(hbridge_t *hb, uint8_t ch_id) {
    if (ch_id >= 2 || !hb->ch[ch_id].registered) return;

    hbridge_channel_t *ch = &hb->ch[ch_id];

    if (ch->is_streaming && ch->hdma) {
        oop_dma_abort(ch->hdma);
    }
    ch->is_streaming = false;
    ch->wave_frame_count = 0;
    ch->wave_loop = false;

    uint32_t frame[4] = {0, 0, 0, 0};
    hbridge_write_ccr(ch, frame);

    /* ★ 停止后状态变为 IDLE */
    g_ch_state[ch_id] = HBRIDGE_IDLE;
    g_ch_duty[ch_id] = 0;

    DBG_LOG("HB CH%d: wave stopped", ch_id);
}

void HBRIDGE_SetCallback(hbridge_t *hb, uint8_t ch_id,
                         hbridge_dma_done_t callback, void *user_data) {
    if (ch_id >= 2 || !hb->ch[ch_id].registered) return;
    hb->ch[ch_id].callback = callback;
    hb->ch[ch_id].callback_user = user_data;
}

bool HBRIDGE_IsStreaming(hbridge_t *hb, uint8_t ch_id) {
    if (ch_id >= 2 || !hb->ch[ch_id].registered) return false;
    return hb->ch[ch_id].is_streaming;
}

/* ========== Utilities ========== */

const char* HBRIDGE_StateToString(hbridge_state_t state) {
    switch (state) {
        case HBRIDGE_FORWARD: return "FWD";
        case HBRIDGE_REVERSE: return "REV";
        case HBRIDGE_BRAKE:   return "BRK";
        case HBRIDGE_IDLE:    return "IDL";
        default:              return "???";
    }
}
