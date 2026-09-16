/**
 * @file    gh2se.h
 * @brief   GH-2SE 接近传感器硬件驱动
 * @note    由工程 Hardware/gh2se 迁入 devices 层。从旧 pot 模块中提取
 *          "阈值判定 / 到位 / 反弹"逻辑，重构成与 ADC、Modbus 完全解耦的
 *          独立传感器驱动。输入为 mV 电压，由调用方（波形任务）从 oop_adc
 *          读取后喂入。纯逻辑状态机，不碰 MCU 寄存器；计时走 oop_dwt。
 * @version V1.0
 * @date    2026-08-14
 */
#ifndef __GH2SE_H__
#define __GH2SE_H__

#include <stdint.h>
#include <stdbool.h>

/** 本周期产生的事件 */
typedef enum {
    GH2SE_EVT_NONE = 0,
    GH2SE_EVT_REACH,        /* 到位（动作触发） */
    GH2SE_EVT_BOUNCE_ALARM, /* 反弹报警 */
} gh2se_event_t;

/** 单路接近传感器状态机 */
typedef struct {
    uint8_t  adc_ch;          /* 绑定的 oop_adc 通道 */
    uint16_t action_th_mv;    /* 到位阈值(mV) */
    uint16_t bounce_th_mv;    /* 反弹阈值(mV) */
    bool     sel;             /* 选择位（外部/HMI 设置） */

    uint16_t value_mv;        /* 最新采样电压 */
    bool     reach;           /* 到位锁存 */
    bool     bounce;          /* 反弹锁存 */
    bool     bounce_alarm;    /* 反弹报警锁存 */

    /* 动作检测窗口：仅窗口内 GH2SE_Update 才做判定（与 EM 动作同步） */
    bool     win_open;
    uint32_t win_t0;

    /* 内部边沿跟踪 */
    bool     _reach_done;
    bool     _bounce_flag;
    bool     _alarm_done;

    /* 远端基线已确认：窗口内曾观测到"far 侧"（sel0: mv>bounce_th；sel1: mv<bounce_th）
     * 才允许 reach 锁存。杜绝"窗口一开值就已在到位线内"被误判为到位（进而误报反弹）。 */
    bool     baseline_ok;
} gh2se_ch_t;

/** 动作检测窗口时长（覆盖一次动作 + 反弹；窗口由 GH2SE_BeginAction 打开） */
#define GH2SE_ACTION_WIN_US  50000u

/* ============================================================
 * 阈值对管理（到位/反弹合法性 + 联动 + sel 镜像）
 * 屏上设定值约束与 sel 极性绑定，由本驱动统一裁决（算法层）：
 *  - 方向：sel0 → 到位<反弹；sel1 → 到位>反弹（只判方向，不看差值）
 *  - 联动：改到位值方向冲突时，反弹自动 = 到位±GH2SE_GAP_MV，越界 clamp
 *  - sel 反转：强制镜像 mv' = GH2SE_MV_MAX - mv，方向自动满足新极性
 * 调用方（波形任务）只负责 I/O：存生效值、回写屏、刷参考线、写电位器。
 * ============================================================ */
/** ADC 量程上限(mV)。与 hmi_bounds.h 的 HMI_MV_MAX 保持一致（3300）。 */
#define GH2SE_MV_MAX   3300u
/** 联动间距(mV)。与 hmi_bounds.h 的 WAVE_MV_QUANT_STEP 保持一致（10）。 */
#define GH2SE_GAP_MV   10u

/** 某 sel 极性下的默认阈值对（sel0: 100/110；sel1: 3200/3190） */
void GH2SE_DefaultPair(bool sel, uint16_t *act_p, uint16_t *bnc_p);

/** 方向判定：到位/反弹对是否匹配 sel 极性（纯判定，不修改） */
bool GH2SE_DirOk(bool sel, uint16_t act_mv, uint16_t bnc_mv);

/** 改到位值联动：仅在方向冲突时调用（调用方先 GH2SE_DirOk 判定）。
 *  - sel0：反弹 = 到位+10；越上限 → (3290, 3300)
 *  - sel1：反弹 = 到位-10；越下限 → (10, 0)
 * act/bnc 均为 in-out，调用方取 clamp 后的生效值回写/落盘。 */
void GH2SE_LinkBounce(bool sel, uint16_t *act_p, uint16_t *bnc_p);

/** sel 反转：强制镜像（act'=MV_MAX-act, bnc'=MV_MAX-bnc）。
 * 方向自动反转（a<b ⇔ MV_MAX-a>MV_MAX-b），天然满足新极性；差值不变。 */
void GH2SE_MirrorPair(uint16_t act_mv, uint16_t bnc_mv,
                      uint16_t *na_p, uint16_t *nb_p);

/** 初始化一路传感器（绑定 ADC 通道与默认阈值） */
void GH2SE_Init(gh2se_ch_t *ch, uint8_t adc_ch,
                uint16_t action_th_mv, uint16_t bounce_th_mv);

/** 运行时更新阈值 */
void GH2SE_SetThresholds(gh2se_ch_t *ch, uint16_t action_th_mv, uint16_t bounce_th_mv);
/** 更新选择位 */
void GH2SE_SetSel(gh2se_ch_t *ch, bool sel);
/** 清除所有锁存（新动作周期开始前调用） */
void GH2SE_Reset(gh2se_ch_t *ch);

/** 动作开始：复位锁存并打开检测窗口（EM 触发钩子里调用，与动作严格同步） */
void GH2SE_BeginAction(gh2se_ch_t *ch);

/** 立即关闭检测窗口（对方通道触发 = 本通道动作期结束；与 BeginAction 同临界区）。
 * 窗口归属：每通道只检测"本通道动作"期间，对方动作开始即关窗，
 * 防止残留窗口把对方动作期的机械耦合位移误判为反弹。 */
void GH2SE_CloseAction(gh2se_ch_t *ch);

/** 推进窗口超时（主循环每轮调用；窗口到点自动关闭） */
void GH2SE_WindowTick(gh2se_ch_t *ch, uint32_t now_us);

/** 当前检测窗口是否打开 */
bool GH2SE_IsActionWindow(const gh2se_ch_t *ch);

/**
 * @brief  喂入最新电压(mV)，推进状态机（仅窗口内生效）
 * @retval 本周期产生的事件（可被用于驱动到位/反弹 GPIO 输出）
 */
gh2se_event_t GH2SE_Update(gh2se_ch_t *ch, uint16_t mv);

#endif // __GH2SE_H__
