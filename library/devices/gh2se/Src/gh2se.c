/**
 * @file    gh2se.c
 * @brief   GH-2SE 接近传感器状态机实现
 * @see     gh2se.h
 *
 * SDK 版：由工程 Hardware/gh2se 迁入 devices 层。bsp_dwt → oop_dwt
 * （bsp_GetCycleCount → oop_GetCycleCount，bsp_IsTimeout → oop_IsTimeout），
 * 纯逻辑不碰 MCU 寄存器，计时走 oop_dwt 封装。
 */
#include "gh2se.h"
#include "oop_dwt.h"   /* oop_GetCycleCount / oop_IsTimeout：动作窗口计时 */
#include "hal_platform.h"   /* __get_PRIMASK/__set_PRIMASK：窗口开关临界区 */

void GH2SE_Init(gh2se_ch_t *ch, uint8_t adc_ch,
                uint16_t action_th_mv, uint16_t bounce_th_mv) {
    ch->adc_ch         = adc_ch;
    ch->action_th_mv   = action_th_mv;
    ch->bounce_th_mv   = bounce_th_mv;
    ch->sel            = false;
    ch->value_mv       = 0;
    ch->win_open       = false;
    ch->win_t0         = 0;
    ch->reach = ch->bounce = ch->bounce_alarm = false;
    ch->_reach_done = ch->_bounce_flag = ch->_alarm_done = false;
    ch->baseline_ok    = false;
}

void GH2SE_SetThresholds(gh2se_ch_t *ch, uint16_t action_th_mv, uint16_t bounce_th_mv) {
    ch->action_th_mv = action_th_mv;
    ch->bounce_th_mv = bounce_th_mv;
}

void GH2SE_SetSel(gh2se_ch_t *ch, bool sel) {
    ch->sel = sel;
}

void GH2SE_DefaultPair(bool sel, uint16_t *act_p, uint16_t *bnc_p) {
    if (sel) { *act_p = 3200; *bnc_p = 3190; }
    else     { *act_p = 100;  *bnc_p = 110;  }
}

bool GH2SE_DirOk(bool sel, uint16_t act_mv, uint16_t bnc_mv) {
    return sel ? (act_mv > bnc_mv) : (bnc_mv > act_mv);
}

void GH2SE_LinkBounce(bool sel, uint16_t *act_p, uint16_t *bnc_p) {
    uint16_t act = *act_p;
    if (!sel) {
        uint16_t b = (uint16_t)(act + GH2SE_GAP_MV);
        if (b > GH2SE_MV_MAX) {
            *act_p = (uint16_t)(GH2SE_MV_MAX - GH2SE_GAP_MV);
            *bnc_p = GH2SE_MV_MAX;
        } else {
            *bnc_p = b;
        }
    } else {
        if (act < GH2SE_GAP_MV) {
            *act_p = GH2SE_GAP_MV;
            *bnc_p = 0;
        } else {
            *bnc_p = (uint16_t)(act - GH2SE_GAP_MV);
        }
    }
}

void GH2SE_MirrorPair(uint16_t act_mv, uint16_t bnc_mv,
                      uint16_t *na_p, uint16_t *nb_p) {
    *na_p = (uint16_t)(GH2SE_MV_MAX - act_mv);
    *nb_p = (uint16_t)(GH2SE_MV_MAX - bnc_mv);
}

void GH2SE_Reset(gh2se_ch_t *ch) {
    ch->reach = ch->bounce = ch->bounce_alarm = false;
    ch->_reach_done = ch->_bounce_flag = ch->_alarm_done = false;
    ch->baseline_ok = false;
}

void GH2SE_BeginAction(gh2se_ch_t *ch) {
    if (!ch) return;
    /* ★ 临界区：本函数可在 EXTI 中断（EM_Trigger 钩子）或主循环（sim_tick）
     * 上下文调用，而窗口状态在 ADC 批次中断（100us）里被读——复位+开窗必须是
     * 原子顺序，避免批次中断读到"复位了一半"的中间锁存。PRIMASK 保存/恢复，
     * 不破坏调用方中断使能状态。 */
    uint32_t pm = __get_PRIMASK();
    __disable_irq();
    GH2SE_Reset(ch);
    ch->win_open = true;
    ch->win_t0   = oop_GetCycleCount();
    __set_PRIMASK(pm);
}

void GH2SE_CloseAction(gh2se_ch_t *ch) {
    if (!ch) return;
    /* 与 BeginAction 同构临界区：窗口状态在 ADC 批次中断（100us）被读，
     * 本函数可在 EXTI / 主循环上下文调用，原子关窗避免读到半态。 */
    uint32_t pm = __get_PRIMASK();
    __disable_irq();
    ch->win_open = false;
    __set_PRIMASK(pm);
}

void GH2SE_WindowTick(gh2se_ch_t *ch, uint32_t now_us) {
    if (!ch) return;
    if (ch->win_open && oop_IsTimeout(ch->win_t0, GH2SE_ACTION_WIN_US)) {
        ch->win_open = false;
    }
}

bool GH2SE_IsActionWindow(const gh2se_ch_t *ch) {
    return ch ? ch->win_open : false;
}

gh2se_event_t GH2SE_Update(gh2se_ch_t *ch, uint16_t mv) {
    ch->value_mv = mv;
    gh2se_event_t evt = GH2SE_EVT_NONE;

    /* 极性由 sel 决定（软件层复现硬件方向），两套判定：
     *  - sel=0（接近→值变小）：到位 = 值低于到位线；反弹 = 先低于反弹线、再回升高于反弹线
     *  - sel=1（接近→值变大）：到位 = 值高于到位线；反弹 = 先高于反弹线、再回落低于反弹线
     * 阈值合法性（屏设定值）同样按 sel：sel0 到位<反弹、sel1 到位>反弹（差10）。
     * ★ 远端基线前置：reach 只有在窗口内先观测到 far 侧（sel0: mv>bounce_th；sel1: mv<bounce_th）
     *   后才允许锁存——避免"窗口一开值就已在到位线内/悬空偏低"被误判为到位（进而误报反弹）。 */
    if (ch->sel) {
        if (mv < ch->bounce_th_mv) {
            ch->baseline_ok = true;   /* sel1 远端基线：接近→值变大，far = 值低于反弹线 */
        }
        if (!ch->_reach_done && ch->baseline_ok && mv > ch->action_th_mv && !ch->reach) {
            ch->reach       = true;
            ch->_reach_done = true;
            evt = GH2SE_EVT_REACH;
        }
        if (mv > ch->bounce_th_mv) {
            ch->_bounce_flag = true;
        }
        if (!ch->_alarm_done && ch->_reach_done && ch->_bounce_flag && mv < ch->bounce_th_mv) {
            ch->bounce       = true;
            ch->bounce_alarm = true;
            ch->_alarm_done  = true;
            evt = GH2SE_EVT_BOUNCE_ALARM;
        }
    } else {
        if (mv == 0) return evt;   /* 极性0：0 视为无效采样（保持原行为） */
        if (mv > ch->bounce_th_mv) {
            ch->baseline_ok = true;   /* sel0 远端基线：接近→值变小，far = 值高于反弹线 */
        }
        if (!ch->_reach_done && ch->baseline_ok && mv < ch->action_th_mv && !ch->reach) {
            ch->reach       = true;
            ch->_reach_done = true;
            evt = GH2SE_EVT_REACH;
        }
        if (mv < ch->bounce_th_mv) {
            ch->_bounce_flag = true;
        }
        if (!ch->_alarm_done && ch->_reach_done && ch->_bounce_flag && mv > ch->bounce_th_mv) {
            ch->bounce       = true;
            ch->bounce_alarm = true;
            ch->_alarm_done  = true;
            evt = GH2SE_EVT_BOUNCE_ALARM;
        }
    }
    return evt;
}
