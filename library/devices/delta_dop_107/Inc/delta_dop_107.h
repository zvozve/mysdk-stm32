/* ============================================================
 * delta_dop_107.h - Delta DOP-107 系列 HMI 屏 波形显示驱动（功能封装层）
 *
 * 职责（只封装"功能"，不做 Modbus 点位认领）：
 *   - 刷新波形：把 2 路动作波形（300 点/路）按屏规则分帧（100 reg/帧）
 *     通过 Modbus 主机"一帧一帧"排队写出，收尾自动发 1002=1 刷新屏。
 *   - 参考线：4 条水平参考线（动作/反弹 × A/B），每条按屏规则用
 *     2 个寄存器（{mv,mv}）表示并排队写出。
 *   - 屏控制：刷新（1002=1）/ 清除（1002=256）。
 *   - 错误标记线圈（$200.4）。
 *
 * 边界（2026-08-18 用户拍板）：
 *   - Modbus 点位认领（W1..W4 / 线圈2/3/4 的回调注册、校验、量化、
 *     参数持有、eeprom）仍在 task_waveform 任务；
 *   - 本驱动只提供"封装功能"，由波形任务在相应时机调用。
 *   - 所有写出均非阻塞：只登记待发状态，由 DOP107_Poll 在主机空闲时
 *     每次补发一帧（Modbus 主机同一时刻只接受一个请求，见工程坑记录）。
 *
 * 屏协议地址统一在 hmi_modbus_addr.h（MB_REG_WAVE_A/B、MB_REG_LINE_*、
 * MB_REG_HMI_CTRL、MB_COIL_ERR_MARK），本驱动直接引用，不重复定义。
 * ============================================================ */
#ifndef __DELTA_DOP_107_H__
#define __DELTA_DOP_107_H__

#include <stdint.h>
#include <stdbool.h>
#include "modbus_core.h"   /* modbus_t */
#include "hmi_modbus_addr.h" /* HMI Modbus 地址总表（统一管理）
                              *   波形数据  = MB_REG_WAVE_A/B
                              *   参考线    = MB_REG_LINE_ACT/BNC_A/B
                              *   控制寄存器= MB_REG_HMI_CTRL（值 MB_HMI_CTRL_*）
                              *   错误标记  = MB_COIL_ERR_MARK */

/* ========== 波形/分帧常量（屏规则） ========== */
#define DOP107_WAVE_LEN        300    /* 单路波形点数 */
#define DOP107_WAVE_CHUNK      100    /* 单帧寄存器上限（Modbus 帧长留余量） */

/* ========== 参考线序号 ========== */
enum {
    DOP107_LINE_ID_ACT_A = 0,   /* 动作线A（W1 设值） */
    DOP107_LINE_ID_BNC_A,       /* 反弹线A（W2 设值） */
    DOP107_LINE_ID_ACT_B,       /* 动作线B（W3 设值） */
    DOP107_LINE_ID_BNC_B,       /* 反弹线B（W4 设值） */
    DOP107_LINE_N,
};

/* 波形数据源回调：驱动在分帧开始时向任务索取某路波形的线性快照
 * （最旧→最新 300 点）。任务负责把自身环形缓冲按时间序拷入 dst。 */
typedef void (*dop107_wave_source_t)(uint8_t ch, uint16_t *dst, uint16_t len);
/* 波形刷新完成回调（A/B 两路全部写毕 + 收尾刷新后触发一次）。
 * 用于"开机自测只跑一轮"这类场景：任务登记回调，flush 完成后恢复实时采集。 */
typedef void (*dop107_wave_flush_done_t)(void);
/* 通道激活判定回调：返回某通道是否应上传波形。未接(掩码)通道返回 false，
 * 驱动跳过该通道的分帧（不给没接的一路空输出波形）。NULL 回调 = 全部上传。 */
typedef bool (*dop107_wave_active_t)(uint8_t ch);

/* ========== 生命周期 ========== */
void DOP107_Init(void);
void DOP107_AttachMaster(modbus_t *m);
/** 主循环周期调用：主机空闲时逐帧补发 pending（控制/标记 → 参考线 → 波形） */
void DOP107_Poll(void);
/** 链路是否真活（出现过有效应答 且 未断线）。开机自测等需等链路活了再发。 */
bool DOP107_IsLinkUp(void);
/** 注册波形刷新完成回调（可选，默认 NULL）。 */
void DOP107_SetWaveFlushDoneCb(dop107_wave_flush_done_t cb);
/** 帧级 RTT 调试打印开关（默认关）：开启后每发出一帧波形打印目标地址+首尾值，
 * 便于对着 MB 日志核对屏端到底收了哪些寄存器块。 */
void DOP107_SetFrameDebug(bool en);

/* ========== 功能封装（波形任务调用） ========== */
/** 注册波形数据源回调（任务 Init 时调用一次） */
void DOP107_SetWaveSource(dop107_wave_source_t cb);
/** 注册通道激活判定回调（可选，NULL=全部上传）：未接(掩码)通道返回 false 时，
 *  驱动跳过该通道波形分帧，不给没接的一路空输出波形。 */
void DOP107_SetWaveActiveCb(dop107_wave_active_t cb);
/** 刷新波形：排队写 A/B 两路（内部 staging + 分帧 + 收尾自动刷新屏）。
 *  总开关关闭（DOP107_WaveEnable(false)）时不登记，释放总线给读轮询。 */
void DOP107_WaveFlush(void);
/** ADC 动作波形写出总开关（默认关） */
void DOP107_WaveEnable(bool en);
/** 写一条参考线：更新该线显示值并排队重写（屏规则：2 寄存器 {mv,mv}） */
void DOP107_WriteLine(uint8_t line_id, uint16_t mv);
/** 屏控制：刷新（1002=1） / 清除（1002=256，清除后自动补一次刷新） */
void DOP107_ScreenRefresh(void);
void DOP107_ScreenClear(void);
/** 反弹错误标记线圈（$200.4） */
void DOP107_SetErrMarker(bool on);

#endif /* __DELTA_DOP_107_H__ */
