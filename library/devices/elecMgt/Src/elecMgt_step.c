/* ============================================================
 * elecMgt_step.c - Step module implementation (参数 -> 帧)
 * ============================================================ */

#include "elecMgt_step.h"
#include "SEGGER_RTT_Log.h"
#include <string.h>

/* ========== 内置默认配置 ========== */

/* S1 - 吸合动作 */
static const em_step_cfg_t g_step_s1 = {
    .seg_count = 5,
    .seg = {
        { HBRIDGE_FORWARD, 1000, 100 },
        { HBRIDGE_FORWARD,  500,  80 },
        { HBRIDGE_FORWARD,  500, 100 },
        { HBRIDGE_REVERSE,   50, 100 },
        { HBRIDGE_FORWARD,    0,  10 },   /* 保持帧 */
    },
};

/* S2 - 释放动作 */
static const em_step_cfg_t g_step_s2 = {
    .seg_count = 3,
    .seg = {
        { HBRIDGE_REVERSE,   50, 100 },
        { HBRIDGE_FORWARD,  1000, 100 },
        { HBRIDGE_REVERSE,   20, 100 },
    },
};

/* ========== 参数 -> 帧 ========== */

uint8_t em_step_build_frames(const em_step_cfg_t *cfg,
                             em_frame_t *out, uint8_t max_frames) {
    if (!cfg || !out || max_frames == 0 || cfg->seg_count == 0) return 0;

    uint8_t n = cfg->seg_count;
    if (n > max_frames) n = max_frames;

    for (uint8_t i = 0; i < n; i++) {
        out[i].time_us = cfg->seg[i].time_us;
        out[i].duty    = cfg->seg[i].duty;
        out[i].state   = cfg->seg[i].state;
    }
    return n;
}

/* ========== 注册到核心 ========== */

void em_step_register(em_trigger_t src, uint8_t ch_id, const em_step_cfg_t *cfg) {
    /* 静态缓冲按 (src,ch) 分槽，生命周期与动作等长 */
    static em_frame_t s_buf[EM_TRIGGER_MAX][EM_MAX_CHANNELS][EM_MAX_STEP_SEGMENTS];

    uint8_t n = em_step_build_frames(cfg, s_buf[src][ch_id], EM_MAX_STEP_SEGMENTS);
    if (n > 0) {
        EM_UpdateAction(src, ch_id, s_buf[src][ch_id], n);
    }
}

/* ========== 高层注册：参数组 + 磁铁 + 自动映射 ========== */

/* 4 个动作块的当前配置（驱动内部权威存储，eeprom/HMI 读写都经此） */
static em_step_cfg_t g_actions[EM_ACT_COUNT];

/* 已注册磁铁 + 数量 */
static em_magnet_reg_t g_mag[EM_MAX_MAGNETS];
static uint8_t         g_mag_count = 0;

/* (触发,通道) 目标映射：每个动作块可映射到多个目标 */
typedef struct { em_trigger_t trig; uint8_t ch; } em_step_target_t;
static em_step_target_t g_targets[EM_ACT_COUNT][2];
static uint8_t          g_target_n[EM_ACT_COUNT];

/* 参数组 grp -> 动作块（G1->A, G2->B）；is_s1 决定取 S1/S2 */
static em_act_id_t em_step_slot_for(uint8_t grp, bool is_s1) {
    if (grp == EM_GRP_1) return is_s1 ? EM_ACT_A_S1 : EM_ACT_A_S2;
    return is_s1 ? EM_ACT_B_S1 : EM_ACT_B_S2;
}

static void em_step_add_target(em_act_id_t act, em_trigger_t trig, uint8_t ch) {
    for (uint8_t i = 0; i < g_target_n[act]; i++) {
        if (g_targets[act][i].trig == trig && g_targets[act][i].ch == ch) return;
    }
    if (g_target_n[act] < 2) {
        g_targets[act][g_target_n[act]].trig = trig;
        g_targets[act][g_target_n[act]].ch   = ch;
        g_target_n[act]++;
    }
}

/* 据已注册磁铁，计算每个动作块 -> (触发,通道) 的映射 */
static void em_step_compute_slots(void) {
    memset(g_targets, 0, sizeof(g_targets));
    memset(g_target_n, 0, sizeof(g_target_n));

    for (uint8_t k = 0; k < g_mag_count; k++) {
        const em_magnet_reg_t *m = &g_mag[k];
        /* 动作块 = 本磁铁参数组；IN1/IN2 都用本磁铁自己的块；
         * 位置决定取 S1 还是 S2：上->吸合(S1)于IN1、释放(S2)于IN2，
         *                       下->释放(S2)于IN1、吸合(S1)于IN2。 */
        em_act_id_t s1 = em_step_slot_for(m->grp, (m->pos == EM_POS_UP));    /* IN1 */
        em_act_id_t s2 = em_step_slot_for(m->grp, (m->pos == EM_POS_DOWN));  /* IN2 */
        em_step_add_target(s1, EM_TRIGGER_IN1, m->ch);
        em_step_add_target(s2, EM_TRIGGER_IN2, m->ch);
    }
}

/* 把单个动作块注册到它的所有映射目标 */
static void em_step_register_act(em_act_id_t act) {
    for (uint8_t t = 0; t < g_target_n[act]; t++) {
        em_step_register(g_targets[act][t].trig, g_targets[act][t].ch, &g_actions[act]);
    }
}

static void em_step_register_all(void) {
    for (int i = 0; i < EM_ACT_COUNT; i++) {
        if (g_actions[i].seg_count == 0) continue;   /* 未使用的块跳过 */
        em_step_register_act((em_act_id_t)i);
    }
}

void EM_Step_SetGroup(uint8_t grp, const em_step_cfg_t *s1, const em_step_cfg_t *s2) {
    if (!s1 || !s2 || grp >= EM_MAX_GROUPS) return;
    em_act_id_t a = em_step_slot_for(grp, true);
    em_act_id_t b = em_step_slot_for(grp, false);
    g_actions[a] = *s1;
    g_actions[b] = *s2;
    DBG_LOG("EM_Step: group %d set (S1->act%d, S2->act%d)", grp, a, b);
}

void EM_Step_RegisterMagnet(uint8_t ch, em_pos_t pos, uint8_t grp) {
    if (g_mag_count >= EM_MAX_MAGNETS) {
        DBG_LOG("EM_Step: magnet full (%d)", EM_MAX_MAGNETS);
        return;
    }
    g_mag[g_mag_count].ch  = ch;
    g_mag[g_mag_count].pos = pos;
    g_mag[g_mag_count].grp = grp;
    /* 保持型由位置推导：上(EM_POS_UP)=保持，下(EM_POS_DOWN)=不保持。
     * 上电磁铁上电停在上方，下电磁铁非保持、上电回 idle。 */
    g_mag[g_mag_count].hold = (pos == EM_POS_UP);
    g_mag_count++;
    DBG_LOG("EM_Step: magnet#%d ch=%d pos=%d grp=%d hold=%d",
            g_mag_count - 1, ch, (int)pos, grp, g_mag[g_mag_count - 1].hold);
}

void EM_Step_Apply(void) {
    EM_ConfigMagnets(g_mag, g_mag_count, false);  /* 模式自动选 + hbridge 初始化 */
    em_step_compute_slots();
    em_step_register_all();
    DBG_LOG("EM_Step: applied (%d magnets, mode auto)", g_mag_count);
}

const em_step_cfg_t* EM_Step_GetConfig(em_act_id_t act) {
    if (act >= EM_ACT_COUNT) return NULL;
    return &g_actions[act];
}

void EM_Step_SetConfig(em_act_id_t act, const em_step_cfg_t *cfg) {
    if (act >= EM_ACT_COUNT || !cfg) return;
    g_actions[act] = *cfg;
    em_step_register_act(act);   /* 立即重注册到映射目标，下次触发即生效 */
    DBG_LOG("EM_Step: config updated act%d", act);
}

/* ========== 默认载入（兼容旧用法，业务层一般用 SetGroup 替代） ========== */

void EM_Step_Init(void) {
    em_step_register(EM_TRIGGER_IN1, 0, &g_step_s1);
    em_step_register(EM_TRIGGER_IN2, 0, &g_step_s2);
    DBG_LOG("EM_Step: default actions loaded (S1->IN1, S2->IN2)");
}
