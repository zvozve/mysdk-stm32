/**
 * @file    ir_tx.h
 * @brief   红外发射驱动（TIM 硬件 38kHz 载波 + 软件门控时序）
 * @version V1.0
 * @date    2026-08-27
 *
 * 原理：
 *   - 定时器产生精确的 38kHz PWM 载波（硬件保证频率/占空比，无抖动）
 *   - 发射“mark”段时使能载波输出，发射“space”段时关闭输出
 *   - 各段时长用 DWT 微秒延时门控，从而原样重放学习到的波形
 *
 * 电平语义（与 ir_1838b 接收侧一致）：
 *   - VS1838B 输出 低电平 = 检测到 38kHz 载波（mark）
 *   - 因此重放时，段电平为 0 → 点亮红外发射管
 */

#ifndef __IR_TX_H
#define __IR_TX_H

#include <stdint.h>
#include <stdbool.h>
#include "main.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========== 硬件配置（换引脚/定时器时只改这里） ========== */

/* 发射引脚：PE6 = TIM9_CH2 (AF3)，紧邻 VS1838B 的 PE5 便于走线 */
#define IR_TX_GPIO_PORT         GPIOE
#define IR_TX_GPIO_PIN          GPIO_PIN_6
#define IR_TX_GPIO_AF           GPIO_AF3_TIM9

/* 载波定时器：TIM9 挂 APB2（定时器时钟 168MHz） */
#define IR_TX_TIM               TIM9
#define IR_TX_TIM_CLK_ENABLE()  __HAL_RCC_TIM9_CLK_ENABLE()
#define IR_TX_TIM_CHANNEL       TIM_CHANNEL_2

/* 载波参数（默认 168MHz 定时器时钟）：
 *   PSC = 168-1  →  计数频率 1MHz（1 tick = 1us）
 *   ARR = 26-1   →  周期 26us  ≈ 38.46kHz 载波
 *   CCR = 9      →  占空比 9/26 ≈ 34.6%（常用 1/3 附近）
 */
#define IR_TX_TIM_PSC           (168 - 1)
#define IR_TX_TIM_ARR           (26 - 1)
#define IR_TX_TIM_CCR           9

/* ========== API ========== */

/**
 * @brief  初始化红外发射（GPIO + TIM9 PWM，初始化后输出关闭）
 * @retval true 成功
 */
bool IR_TX_Init(void);

/**
 * @brief  反初始化（关闭载波，停定时器）
 */
void IR_TX_DeInit(void);

/**
 * @brief  重放一帧原始时序（学习码回放的核心接口）
 * @param  dur_us       各段时长数组（us），即 ir_raw_frame_t.timing_us
 * @param  count        段数，即 ir_raw_frame_t.edges
 * @param  level_start  第一段电平：0=载波段（发射）1=静默段，即 ir_raw_frame_t.level_start
 * @param  repeats      重复次数（空调单次发射传 1；NEC 重复码场景可 >1）
 * @param  repeat_gap_ms 重复帧之间的间隔（ms），repeats=1 时无效
 * @retval true 发送完成
 * @note   阻塞调用：空调帧约持续 50~100ms；发送期间红外接收中断不受影响。
 */
bool IR_TX_SendRaw(const uint16_t *dur_us, uint16_t count, uint8_t level_start,
                   uint8_t repeats, uint16_t repeat_gap_ms);

/**
 * @brief  是否正在发送
 */
bool IR_TX_IsBusy(void);

#ifdef __cplusplus
}
#endif

#endif /* __IR_TX_H */
