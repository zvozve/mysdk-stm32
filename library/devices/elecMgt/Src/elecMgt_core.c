/* ============================================================
 * elecMgt_core.c - Electromagnet core implementation (通用核心)
 *
 * 整合原 elecMgt.c / elecMgt_priv.h / elecMgt_action.c：
 *   - em_ctx_t / em_channel_t 私有类型（原 priv）
 *   - em_frame_to_points() 生成引擎（原 EM_Action_Generate）
 *   - 双缓冲 + DMA 触发
 * 不再内置业务默认动作；默认动作由 elecMgt_step 模块提供。
 *
 * SDK 版（迁入 devices/elecMgt）：
 *   - bsp_dwt.h -> chip/oop_dwt.h（bsp_GetCycleCount -> oop_GetCycleCount，
 *     bsp_IsTimeout -> oop_IsTimeout）
 *   - 移除对工程全局句柄（htim1/htim8/hdma_tim*_ch*）的 extern 引用，
 *     改为 EM_Init 时由工程注入 em_hw_t，满足 SDK 板无关约束。
 * ============================================================ */

#include "elecMgt_core.h"
#include "oop_dwt.h"
#include "SEGGER_RTT_Log.h"
#include <string.h>

/* ========== 注入的硬件绑定（独立静态，避免被 em_core_init 的 memset 清零） ========== */

static em_hw_t g_em_hw = {0};

/* ========== 私有类型（原 elecMgt_priv.h） ========== */

typedef struct {
    uint8_t     id;
    em_state_t  state;
    uint32_t    timeout_us;
    uint32_t    run_start;
    bool        dma_active;
    bool        last_had_hold;   /* 最近一次动作完成后是否处于保持态 */
    em_trigger_t last_trigger;   /* 最近一次触发并启动的动作来源(IN1/IN2) */
    bool        hold;            /* 是否保持型（由磁铁注册推导：上=保持/下=不保持）；
                                  * true=动作完成后保持末态，false=回到 idle */
} em_channel_t;

typedef struct {
    em_mode_t        mode;
    em_channel_t     channels[EM_MAX_CHANNELS];
    uint8_t          channel_count;
    hbridge_t        hb;
    bool             initialized;
    em_action_t      actions[EM_TRIGGER_MAX][EM_MAX_CHANNELS];
    em_magnet_reg_t  magnets[EM_MAX_CHANNELS];   /* 物理注册（电源/位置/参数组） */
    uint8_t          mag_count;
    bool             same_params;                /* 是否共用参数（仅信息，内容由用户配置） */
} em_ctx_t;

/* ========== Global instance ========== */

static em_ctx_t g_em_ctx;

/* ========== 动作锁定（反弹错误时禁止触发） ========== */
static bool g_action_lock = false;

/* ========== 核心生成引擎：frame -> point（原 EM_Action_Generate） ========== */

uint16_t em_frame_to_points(const em_frame_t *frames, uint8_t count,
                            em_point_t *points, uint16_t max_size) {
    if (!frames || count == 0 || !points || max_size == 0) return 0;

    uint16_t total_points = 0;
    bool has_hold = false;

    for (uint16_t i = 0; i < count; i++) {
        /* 中间 time_us = 0 → 舍弃 */
        if (frames[i].time_us == 0 && i < count - 1) {
            DBG_LOG("EM: skip frame %d (time_us=0 in middle)", i);
            continue;
        }

        /* 最后 time_us = 0 → 保持帧，展开为 1 帧 */
        if (frames[i].time_us == 0 && i == count - 1) {
            has_hold = true;
            if (total_points < max_size) {
                points[total_points].state = frames[i].state;
                points[total_points].duty  = frames[i].duty;
                total_points++;
                DBG_LOG("EM: hold frame at end: %s %d%%",
                        HBRIDGE_StateToString(frames[i].state), frames[i].duty);
            }
            continue;
        }

        /* 正常帧：time_us > 0，按 10us 每帧展开 */
        uint16_t frame_count = (frames[i].time_us + 9) / 10;
        if (frame_count == 0) frame_count = 1;

        for (uint16_t j = 0; j < frame_count; j++) {
            if (total_points >= max_size) break;
            points[total_points].state = frames[i].state;
            points[total_points].duty  = frames[i].duty;
            total_points++;
        }
    }

    /* 没有保持帧，最后自动插入 IDLE */
    if (!has_hold && total_points > 0) {
        em_point_t last = points[total_points - 1];
        if (last.state != HBRIDGE_IDLE || last.duty != 0) {
            if (total_points < max_size) {
                points[total_points].state = HBRIDGE_IDLE;
                points[total_points].duty  = 0;
                total_points++;
                DBG_LOG("EM: insert IDLE at end (no hold frame)");
            }
        }
    }

    DBG_LOG("EM: generated %d points", total_points);
    return total_points;
}

/* ========== Internal: H-bridge initialization ========== */

static void em_hbridge_init(em_mode_t mode) {
    HBRIDGE_Init(&g_em_ctx.hb);

    /* 按注入的绑定注册（ch0=TIM8 桥A / ch1=TIM1 桥B）；htim 为 NULL 的桥跳过 */
    const em_bridge_hw_t *hw0 = &g_em_hw.ch[0];
    if (hw0->htim) {
        HBRIDGE_Register(&g_em_ctx.hb, hw0->htim,
                         hw0->hdma_ch1, hw0->hdma_ch2,
                         hw0->hdma_ch3, hw0->hdma_ch4,
                         hw0->max_pulse);
    }
    const em_bridge_hw_t *hw1 = &g_em_hw.ch[1];
    if (hw1->htim) {
        HBRIDGE_Register(&g_em_ctx.hb, hw1->htim,
                         hw1->hdma_ch1, hw1->hdma_ch2,
                         hw1->hdma_ch3, hw1->hdma_ch4,
                         hw1->max_pulse);
    }
    if (!hw0->htim && !hw1->htim) {
        DBG_LOG("EM: WARN no bridge bound (hw NULL), hbridge not registered");
        return;
    }

    HBRIDGE_EnableSync(&g_em_ctx.hb, true);

    HBRIDGE_StartAll(&g_em_ctx.hb);

    HBRIDGE_SetState(&g_em_ctx.hb, 0, HBRIDGE_IDLE, 0);
    HBRIDGE_SetState(&g_em_ctx.hb, 1, HBRIDGE_IDLE, 0);

    DBG_LOG("EM: hbridge ready (mode=%d, both bridges registered, sync=ON)", (int)mode);
}

/* ========== Internal: 获取通道对应的动作 ========== */
static em_action_t* em_get_action(em_trigger_t src, uint8_t ch_id) {
    if (src >= EM_TRIGGER_MAX || ch_id >= EM_MAX_CHANNELS) return NULL;

#if EM_DUAL_PARAM_COUNT == 1
    if (g_em_ctx.mode == EM_MODE_DUAL_CROSS) {
        if (ch_id == 1) {
            src = (src == EM_TRIGGER_IN1) ? EM_TRIGGER_IN2 : EM_TRIGGER_IN1;
        }
    }
    return &g_em_ctx.actions[src][0];
#else
    return &g_em_ctx.actions[src][ch_id];
#endif
}

/* ========== Internal: 生成缓存 (双缓冲) ========== */
static bool em_generate_cache(em_action_t *act) {
    if (!act) {
        return false;
    }

    uint8_t buf_idx = 1 - act->active_idx;
    uint16_t n = 0;

    if (act->use_points && act->points && act->point_count > 0) {
        /* pid 路径：直接拷贝 point */
        n = act->point_count;
        if (n > HBRIDGE_MAX_FRAMES) n = HBRIDGE_MAX_FRAMES;
        memcpy(act->buf[buf_idx], act->points, (size_t)n * sizeof(em_point_t));
    } else if (act->frames && act->count > 0) {
        /* step 路径：frame -> point */
        n = em_frame_to_points(act->frames, act->count,
                               act->buf[buf_idx], HBRIDGE_MAX_FRAMES);
    } else {
        act->ready = false;
        return false;
    }

    if (n == 0) {
        act->ready = false;
        return false;
    }

    act->buf_count[buf_idx] = n;

    /* 写顺序：先 active_idx 后 ready（last）。读者(em_dma_callback)先查 ready
     * 再取 active_idx；Cortex-M 对普通 RAM 强内存序保证不会看到"ready=true 但
     * active_idx 旧"，故无需临界区。原 __disable_irq/__enable_irq 为非恢复式，
     * 在 ISR 上下文调用会强制开全局中断，属隐患，已移除。 */
    act->active_idx = buf_idx;
    act->ready = true;

    DBG_LOG("EM: cache generated, buf=%d, count=%d", buf_idx, n);
    return true;
}

/* ========== Internal: Start single channel ========== */

static void em_start_single(em_channel_t *ch, const em_action_t *act,
                            em_trigger_t src) {
    if (!act || !act->ready) {
        DBG_LOG("EM CH%d: action not ready", ch->id);
        return;
    }
    if (ch->state == EM_STATE_DISABLED || ch->state == EM_STATE_ERROR) {
        DBG_LOG("EM CH%d: state=%d, skip", ch->id, ch->state);
        return;
    }

    ch->run_start = oop_GetCycleCount();
    ch->state = EM_STATE_RUNNING;
    ch->dma_active = false;
    ch->last_trigger = src;
    ch->last_had_hold = act->has_hold;

    uint8_t idx = act->active_idx;
    uint16_t count = act->buf_count[idx];
    const em_point_t *points = act->buf[idx];

    if (count > 0) {
        HBRIDGE_StartWave(&g_em_ctx.hb, ch->id, points, count);
        ch->dma_active = true;
        DBG_LOG("EM CH%d: started, %d points (buf=%d)", ch->id, count, idx);
    }
}

/* ========== Internal: Start dual channels ========== */

static void em_start_dual(const em_action_t *act0, const em_action_t *act1,
                         em_trigger_t src) {
    if (!act0 || !act0->ready || !act1 || !act1->ready) {
        DBG_LOG("EM: dual action not ready");
        return;
    }

    em_channel_t *ch0 = &g_em_ctx.channels[0];
    em_channel_t *ch1 = &g_em_ctx.channels[1];

    if (ch0->state == EM_STATE_DISABLED || ch0->state == EM_STATE_ERROR) {
        DBG_LOG("EM CH0: state=%d, skip", ch0->state);
        return;
    }
    if (ch1->state == EM_STATE_DISABLED || ch1->state == EM_STATE_ERROR) {
        DBG_LOG("EM CH1: state=%d, skip", ch1->state);
        return;
    }

    ch0->run_start = oop_GetCycleCount();
    ch0->state = EM_STATE_RUNNING;
    ch0->dma_active = false;
    ch0->last_trigger = src;
    ch0->last_had_hold = act0->has_hold;

    ch1->run_start = oop_GetCycleCount();
    ch1->state = EM_STATE_RUNNING;
    ch1->dma_active = false;
    ch1->last_trigger = src;
    ch1->last_had_hold = act1->has_hold;

    uint8_t idx0 = act0->active_idx;
    uint8_t idx1 = act1->active_idx;
    uint16_t count0 = act0->buf_count[idx0];
    uint16_t count1 = act1->buf_count[idx1];
    const em_point_t *pts0 = act0->buf[idx0];
    const em_point_t *pts1 = act1->buf[idx1];

    if (count0 == 0 || count1 == 0) {
        DBG_LOG("EM: dual start failed, count0=%d, count1=%d", count0, count1);
        return;
    }

    HBRIDGE_StartWaveDualSync(&g_em_ctx.hb,
                              0, pts0, count0,
                              1, pts1, count1);

    ch0->dma_active = true;
    ch1->dma_active = true;

    DBG_LOG("EM: dual DMA started");
}

/* ========== 保持规则辅助 ==========
 *
 * 保持由【磁铁位置 + 触发方向】决定（业务规则，与动作是否带保持帧无关）：
 *   上电磁铁(EM_POS_UP):   仅 上升(IN1) 动作完成后进入保持
 *   下电磁铁(EM_POS_DOWN): 仅 下降(IN2) 动作完成后进入保持
 * 另一个方向（释放）完成 → 去磁回 idle，不保持。
 * ============================================================ */

/* 查通道对应的磁铁注册（ch 号 → magnet） */
static em_magnet_reg_t* em_magnet_for_ch(uint8_t ch_id) {
    for (uint8_t i = 0; i < g_em_ctx.mag_count; i++) {
        if (g_em_ctx.magnets[i].ch == ch_id) return &g_em_ctx.magnets[i];
    }
    return NULL;
}

/* ========== DMA完成回调 ========== */
static void em_dma_callback(uint8_t ch_id, void *user) {
    (void)user;
    em_channel_t *ch = &g_em_ctx.channels[ch_id];

    ch->dma_active = false;
    ch->state = EM_STATE_IDLE;

    /* 保持由【磁铁位置 + 刚完成的触发方向】决定（业务规则）：
     *   上电磁铁(UP):  仅 上升(IN1) 完成后保持
     *   下电磁铁(DOWN): 仅 下降(IN2) 完成后保持
     * 另一个方向（释放）完成 → 去磁回 idle，不保持。
     * 若该方向动作本身没有保持帧（has_hold=false），则同样回到 idle。 */
    em_magnet_reg_t *m = em_magnet_for_ch(ch_id);
    if (m == NULL) {
        ch->last_had_hold = false;
        HBRIDGE_SetState(&g_em_ctx.hb, ch_id, HBRIDGE_IDLE, 0);
        DBG_LOG("EM CH%d: DMA done -> IDLE (no magnet)", ch_id);
        return;
    }

    /* 该磁铁“保持触发方向”：上=IN1(上升)，下=IN2(下降) */
    em_trigger_t hold_trig = (m->pos == EM_POS_UP) ? EM_TRIGGER_IN1 : EM_TRIGGER_IN2;
    if (ch->last_trigger == hold_trig) {
        em_action_t *act = em_get_action(hold_trig, ch_id);
        if (act && act->has_hold) {
            ch->last_had_hold = true;
            /* ★ 显式把 H 桥置于保持态（方向+占空比），保证“动作完成后进入保持”。
             * 不依赖“DMA 末尾恰好是保持帧”这一脆弱前提：DMA 已结束、
             * is_streaming=false，此处 SetState 只写 4 个 CCR（含方向切换死区），
             * ISR 上下文安全。即使动作缓冲末帧因生成/截断不是保持帧，
             * 物理上也必然落到 hold_state/hold_duty。 */
            HBRIDGE_SetState(&g_em_ctx.hb, ch_id, act->hold_state, act->hold_duty);
            DBG_LOG("EM CH%d: DMA done -> HOLD (pos=%s after %s, %s %d%%)",
                    ch_id, (m->pos == EM_POS_UP) ? "UP" : "DOWN",
                    (hold_trig == EM_TRIGGER_IN1) ? "IN1" : "IN2",
                    HBRIDGE_StateToString(act->hold_state), act->hold_duty);
            return;
        }
    }

    ch->last_had_hold = false;
    HBRIDGE_SetState(&g_em_ctx.hb, ch_id, HBRIDGE_IDLE, 0);
    DBG_LOG("EM CH%d: DMA done -> IDLE", ch_id);
}

/* ========== Public API ========== */

/* 核心初始化（清零 + hbridge + 通道 + 回调），模式由调用方决定 */
static void em_core_init(em_mode_t mode) {
    memset(&g_em_ctx, 0, sizeof(g_em_ctx));

    g_em_ctx.mode = mode;
    g_em_ctx.initialized = true;
    g_em_ctx.channel_count = EM_MAX_CHANNELS;   /* 两路物理都存在，始终初始化两路 */

    em_hbridge_init(mode);

    for (uint8_t i = 0; i < EM_MAX_CHANNELS; i++) {
        em_channel_t *ch = &g_em_ctx.channels[i];
        ch->id = i;
        ch->state = EM_STATE_IDLE;
        ch->timeout_us = EM_DEFAULT_TIMEOUT_US;
        ch->dma_active = false;
        ch->hold = false;   /* 默认非保持；EM_ConfigMagnets 据磁铁注册覆盖 */
        HBRIDGE_SetState(&g_em_ctx.hb, i, HBRIDGE_IDLE, 0);
    }

    HBRIDGE_SetCallback(&g_em_ctx.hb, 0, em_dma_callback, NULL);
    HBRIDGE_SetCallback(&g_em_ctx.hb, 1, em_dma_callback, NULL);

    DBG_LOG("EM: init done, mode=%d, channels=%d, param_count=%d",
            (int)mode, g_em_ctx.channel_count, EM_DUAL_PARAM_COUNT);
}

void EM_Init(em_mode_t mode, const em_hw_t *hw) {
    if (hw != NULL) {
        g_em_hw = *hw;   /* 先落绑定：em_core_init 内部 memset g_em_ctx，故绑定单独存 */
    }
    g_em_ctx.mag_count = (mode == EM_MODE_SINGLE) ? 1 : 2;
    em_core_init(mode);
}

/* ============================================================
 * 自动选择模式并初始化
 *
 * 用户只注册每颗电磁铁的：通道(ch) / 位置(pos) / 参数组(grp)。
 * 驱动据数量与位置自动选择模式：
 *   - 1 颗           -> SINGLE
 *   - 2 颗 同位置(都上/都下) -> DUAL_SAME
 *   - 2 颗 异位置(一上一下)  -> DUAL_CROSS（IN1/IN2 交叉参数组）
 *
 * 动作→(触发,通道) 的具体映射（含交叉）由业务层 task_elecMgt 据同一
 * 份注册信息计算，核心只负责模式判定与 hbridge 初始化。
 * ============================================================ */
void EM_ConfigMagnets(const em_magnet_reg_t *mags, uint8_t count, bool same_params) {
    if (!mags || count == 0 || count > EM_MAX_CHANNELS) {
        DBG_LOG("EM: ConfigMagnets invalid count=%d", count);
        return;
    }

    em_mode_t mode;
    if (count == 1) {
        mode = EM_MODE_SINGLE;
    } else {
        bool same_pos = (mags[0].pos == mags[1].pos);
        mode = same_pos ? EM_MODE_DUAL_SAME : EM_MODE_DUAL_CROSS;
    }

    DBG_LOG("EM: ConfigMagnets count=%d pos0=%d pos1=%d -> mode=%d same_params=%d",
            count,
            (count > 0) ? (int)mags[0].pos : -1,
            (count > 1) ? (int)mags[1].pos : -1,
            (int)mode, same_params ? 1 : 0);

    em_core_init(mode);

    /* ★★ 磁铁表必须在 em_core_init 之后写入（2026-08-18 修复）：
     * em_core_init 内部 memset(&g_em_ctx,0,sizeof(g_em_ctx)) 会把在此之前
     * 暂存的 mag_count / magnets[] 全部清零（含 same_params）。
     * 若在 memset 之前写，em_magnet_for_ch() 因 mag_count==0 恒返回 NULL，
     * → R14 保持判定（位置+触发方向）永远走 "no magnet" 分支：
     *   上电 EM_RestoreDefaults 全通道 IDLE、动作完成 em_dma_callback 全 IDLE，
     *   → 上电磁铁上电不保持、动作完成后也不保持（用户实测"所有保持都没出现"）。
     * 故：先 em_core_init 建好通道/hbridge，再把磁铁表灌回并落到通道 hold。 */
    g_em_ctx.mag_count = count;
    for (uint8_t i = 0; i < count; i++) g_em_ctx.magnets[i] = mags[i];
    g_em_ctx.same_params = same_params;

    /* 把每颗磁铁的 hold 属性落到对应通道：通道据此决定上电保持态
     * 与动作完成后的保持行为（保持型→保持末态，非保持型→回到 idle）。 */
    for (uint8_t i = 0; i < g_em_ctx.mag_count; i++) {
        uint8_t ch = g_em_ctx.magnets[i].ch;
        if (ch < g_em_ctx.channel_count) {
            g_em_ctx.channels[ch].hold = g_em_ctx.magnets[i].hold;
        }
    }
}

void EM_Poll(void) {
    if (!g_em_ctx.initialized) return;

    for (uint8_t i = 0; i < g_em_ctx.channel_count; i++) {
        em_channel_t *ch = &g_em_ctx.channels[i];
        if (ch->state == EM_STATE_RUNNING) {
            if (ch->timeout_us > 0 && oop_IsTimeout(ch->run_start, ch->timeout_us)) {
                HBRIDGE_SetState(&g_em_ctx.hb, ch->id, HBRIDGE_IDLE, 0);
                ch->dma_active = false;
                ch->state = EM_STATE_ERROR;
                DBG_LOG("EM CH%d: timeout!", ch->id);
            }
        }
    }
}

/* 应用层钩子：动作实际启动前回调（波形/反弹检测同步开窗） */
static void (*g_trigger_hook)(em_trigger_t src) = NULL;

void EM_SetTriggerHook(void (*cb)(em_trigger_t src)) {
    g_trigger_hook = cb;
}

void EM_Trigger(em_trigger_t src) {
    if (!g_em_ctx.initialized) return;

    /* 反弹错误锁定：置位后任何触发（IO 电平 / 模拟节拍）直接返回，
     * 不启动动作。由 task_waveform 在反弹错误时置位，用户清除错误标记后清除。 */
    if (g_action_lock) {
        DBG_LOG("EM: trigger %s blocked (action locked)",
                (src == EM_TRIGGER_IN1) ? "IN1" : "IN2");
        return;
    }

    if (src >= EM_TRIGGER_MAX) return;

    if (g_em_ctx.mag_count == 1) {
        /* 单磁铁模式：动作落在该磁铁实际注册的通道（ch0 或 ch1 皆可，
         * 由用户代码决定注册哪一路实现“屏蔽没接的一路”）。*/
        uint8_t ch = g_em_ctx.magnets[0].ch;
        em_action_t *act = em_get_action(src, ch);
        if (!act || !act->ready) {
            DBG_LOG("EM: trigger %s failed (not ready)",
                    (src == EM_TRIGGER_IN1) ? "IN1" : "IN2");
            return;
        }
        if (g_trigger_hook) g_trigger_hook(src);   /* 开窗：与动作严格同步 */
        DBG_LOG("EM: trigger %s (single, ch=%d)", (src == EM_TRIGGER_IN1) ? "IN1" : "IN2", ch);
        em_start_single(&g_em_ctx.channels[ch], act, src);
    } else {
        em_action_t *act0 = em_get_action(src, 0);
        em_action_t *act1 = em_get_action(src, 1);

        if (!act0 || !act0->ready) {
            DBG_LOG("EM: trigger %s CH0 not ready", (src == EM_TRIGGER_IN1) ? "IN1" : "IN2");
            return;
        }
        if (!act1 || !act1->ready) {
            DBG_LOG("EM: trigger %s CH1 not ready", (src == EM_TRIGGER_IN1) ? "IN1" : "IN2");
            return;
        }

        if (g_trigger_hook) g_trigger_hook(src);   /* 开窗：与动作严格同步 */
        // DBG_LOG("EM: trigger %s (dual)", (src == EM_TRIGGER_IN1) ? "IN1" : "IN2");
        em_start_dual(act0, act1, src);
    }
}

void EM_UpdateAction(em_trigger_t src, uint8_t ch_id,
                     const em_frame_t *frames, uint8_t count) {
    if (src >= EM_TRIGGER_MAX) return;
    if (ch_id >= g_em_ctx.channel_count) return;
    if (!frames || count == 0) return;

    em_action_t *act = em_get_action(src, ch_id);
    if (!act) return;

    act->frames = frames;
    act->count = count;
    act->use_points = false;

    /* 保持帧 = 最后一个 time_us==0 的 step：记录保持态方向+占空比 */
    if (count > 0 && frames[count - 1].time_us == 0) {
        act->has_hold   = true;
        act->hold_state = frames[count - 1].state;
        act->hold_duty  = frames[count - 1].duty;
    } else {
        act->has_hold = false;
    }

    bool ok = em_generate_cache(act);

    DBG_LOG("EM: update %s CH%d %s (%d frames, hold=%d)",
            (src == EM_TRIGGER_IN1) ? "IN1" : "IN2",
            ch_id, ok ? "OK" : "FAILED", count, (int)act->has_hold);
}

void EM_UpdateActionPoints(em_trigger_t src, uint8_t ch_id,
                           const em_point_t *points, uint16_t count) {
    if (src >= EM_TRIGGER_MAX) return;
    if (ch_id >= g_em_ctx.channel_count) return;
    if (!points || count == 0) return;

    em_action_t *act = em_get_action(src, ch_id);
    if (!act) return;

    act->points = points;
    act->point_count = count;
    act->use_points = true;
    act->has_hold = false;   /* point 路径无 time_us，不支持保持帧 */

    bool ok = em_generate_cache(act);

    DBG_LOG("EM: update(points) %s CH%d %s (%d points)",
            (src == EM_TRIGGER_IN1) ? "IN1" : "IN2",
            ch_id, ok ? "OK" : "FAILED", count);
}

/* ========== 恢复各通道默认态（上电 / 反弹错误恢复共用） ==========
 *
 * 让每个通道进入其“默认态”（= 上电态），保持由【磁铁位置】决定
 * （业务规则，与动作是否带保持帧无关）：
 *  - 上电磁铁(pos=EM_POS_UP)：默认保持，保持态 = 上升(IN1) 动作的保持帧，
 *    停在 energized（上电磁铁保持输出）。
 *  - 下电磁铁(pos=EM_POS_DOWN)：默认不保持（等 下降(IN2) 触发后才保持），
 *    回到 idle 去磁，靠弹簧/重力回位（下电磁铁无保持）。
 *
 * 调用场景：
 *  - 上电初始化、动作注册完成后（task_elecMgt）：设备停在其静止位置。
 *  - 反弹错误、50ms 检查窗口结束后（task_waveform）：动作触发已锁定，
 *    把每个注册磁铁拉回各自默认态，避免停在动作中途的任意位置。
 *
 * ★ 可在 DMA 动作未跑完时安全调用：HBRIDGE_SetState 内部先中止在跑的
 *   DMA 再写 CCR。调用方应处于主循环（非 ISR）上下文。
 */
void EM_RestoreDefaults(void) {
    if (!g_em_ctx.initialized) return;

    /* 逐通道决定默认态：
     *  - 保持型（上电磁铁，pos=EM_POS_UP）：若其上升(IN1) 动作带保持帧
     *    （末段 time_us==0，EM_UpdateAction 已算出 has_hold/hold_state/hold_duty），
     *    则“设置一条 CCR”进入保持；否则回 idle。
     *  - 非保持型（下电磁铁）：回 idle（去磁），等 下降(IN2) 触发后才保持。
     * 即用户要求：上电磁铁保持输出、下电磁铁无保持。 */
    for (uint8_t i = 0; i < g_em_ctx.channel_count && i < EM_MAX_CHANNELS; i++) {
        em_channel_t *ch = &g_em_ctx.channels[i];
        ch->dma_active = false;
        ch->state = EM_STATE_IDLE;

        em_magnet_reg_t *m = em_magnet_for_ch(i);
        if (m == NULL || !m->hold) {
            ch->last_had_hold = false;
            HBRIDGE_SetState(&g_em_ctx.hb, i, HBRIDGE_IDLE, 0);
            DBG_LOG("EM CH%d: restore-default IDLE (%s)", i,
                    (m == NULL) ? "no magnet" : "non-hold magnet");
            continue;
        }

        /* 保持型（上电磁铁）：写一次保持 CCR。
         * 保持态直接取 EM_UpdateAction 已算好的 hold_state/hold_duty。 */
        em_action_t *act = em_get_action(EM_TRIGGER_IN1, i);
        if (act && act->has_hold) {
            ch->last_had_hold = true;
            HBRIDGE_SetState(&g_em_ctx.hb, i, act->hold_state, act->hold_duty);
            DBG_LOG("EM CH%d: restore-default HOLD (pos=UP, %s %d%%)", i,
                    HBRIDGE_StateToString(act->hold_state), act->hold_duty);
        } else {
            ch->last_had_hold = false;
            HBRIDGE_SetState(&g_em_ctx.hb, i, HBRIDGE_IDLE, 0);
            DBG_LOG("EM CH%d: restore-default IDLE (hold magnet, no hold frame)", i);
        }
    }
}

bool EM_IsChannelActive(uint8_t ch_id) {
    if (ch_id >= EM_MAX_CHANNELS) return false;
    return em_magnet_for_ch(ch_id) != NULL;   /* 注册了磁铁即激活；未注册 = 掩码 */
}

em_pos_t EM_GetChannelPos(uint8_t ch_id) {
    em_magnet_reg_t *m = em_magnet_for_ch(ch_id);
    return (m != NULL) ? m->pos : EM_POS_DOWN;
}

void EM_Stop(void) {
    if (!g_em_ctx.initialized) return;

    for (uint8_t i = 0; i < g_em_ctx.channel_count; i++) {
        em_channel_t *ch = &g_em_ctx.channels[i];
        if (ch->state == EM_STATE_DISABLED) continue;

        ch->dma_active = false;
        HBRIDGE_SetState(&g_em_ctx.hb, i, HBRIDGE_IDLE, 0);
        ch->state = EM_STATE_IDLE;
        DBG_LOG("EM CH%d: stopped", i);
    }
}

void EM_Enable(uint8_t ch_id, bool enable) {
    if (ch_id >= g_em_ctx.channel_count) return;
    em_channel_t *ch = &g_em_ctx.channels[ch_id];

    if (enable) {
        if (ch->state == EM_STATE_DISABLED) {
            ch->state = EM_STATE_IDLE;
            HBRIDGE_SetState(&g_em_ctx.hb, ch_id, HBRIDGE_IDLE, 0);
            DBG_LOG("EM CH%d: enabled", ch_id);
        }
    } else {
        ch->dma_active = false;
        HBRIDGE_SetState(&g_em_ctx.hb, ch_id, HBRIDGE_IDLE, 0);
        ch->state = EM_STATE_DISABLED;
        DBG_LOG("EM CH%d: disabled", ch_id);
    }
}

void EM_RegisterAction(em_trigger_t src, uint8_t ch_id,
                       const em_frame_t *frames, uint8_t count) {
    EM_UpdateAction(src, ch_id, frames, count);
}

em_state_t EM_GetState(uint8_t ch_id) {
    if (ch_id >= g_em_ctx.channel_count) return EM_STATE_DISABLED;
    return g_em_ctx.channels[ch_id].state;
}

bool EM_IsIdle(uint8_t ch_id) {
    if (ch_id >= g_em_ctx.channel_count) return true;
    em_state_t s = g_em_ctx.channels[ch_id].state;
    return (s == EM_STATE_IDLE || s == EM_STATE_DISABLED);
}

bool EM_IsBusy(void) {
    for (uint8_t i = 0; i < g_em_ctx.channel_count; i++) {
        if (g_em_ctx.channels[i].state == EM_STATE_RUNNING) return true;
    }
    return false;
}

/* ============================================================
 * 动作锁定（反弹错误时禁止触发）
 *  - 置位后 EM_Trigger 直接返回（见上方），覆盖 IO 电平触发与模拟节拍触发
 *  - 供 task_waveform 在反弹错误时调用，用户清除错误标记（线圈 $200.4）后清除
 * ============================================================ */
void EM_SetActionLock(bool lock) {
    g_action_lock = lock;
    DBG_LOG("EM: action lock %s", lock ? "ON (triggers blocked)" : "OFF (triggers allowed)");
}

bool EM_IsActionLocked(void) {
    return g_action_lock;
}

hbridge_t* EM_GetHBridge(void) {
    return &g_em_ctx.hb;
}
