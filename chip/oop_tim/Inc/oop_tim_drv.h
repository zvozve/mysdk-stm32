/**
 * @file    oop_tim_drv.h
 * @brief   OOP TIM 驱动封装（PWM 载波 + 通用时基）
 * @version V1.0
 * @date    2026-08-29
 *
 * 设计：TIM 句柄由调用方（工程 board_cfg / device cfg）注入，本层只做
 * 板无关的 OOP 封装，HAL_TIM_* 是唯一的 vendor 边界（与 oop_gpio_drv_hal.c 同理）。
 * device 层（ir_tx / ir_1838b）不再直调 HAL_TIM_*，统一走本封装。
 */

#ifndef __OOP_TIM_DRV_H
#define __OOP_TIM_DRV_H

#include <stdint.h>
#include <stdbool.h>
#include "hal_platform.h"   /* TIM_HandleTypeDef / HAL_TIM_*（不依赖工程 tim.h） */

#ifdef __cplusplus
extern "C" {
#endif

/* ========== 通用时基（Base，含中断） ========== */

/**
 * @brief   启动定时器更新中断（1ms 节拍等）
 * @retval  true 成功
 */
bool oop_tim_base_start_it(TIM_HandleTypeDef *htim);

/**
 * @brief   停止定时器更新中断
 * @retval  true 成功
 */
bool oop_tim_base_stop_it(TIM_HandleTypeDef *htim);

/* ========== PWM 载波 / 输出 ========== */

/**
 * @brief   按注入参数初始化 PWM 通道（OCMODE_PWM1 / 高有效）
 * @note    初始化后不启动计数器，发送时才开；htim->Instance 须由调用方先赋值。
 * @param   htim      TIM 句柄（Instance 已注入）
 * @param   channel   PWM 通道（如 TIM_CHANNEL_2）
 * @param   prescaler 预分频：计数频率 = TIM_CLK/(PSC+1)
 * @param   period    AutoReload：周期 = (ARR+1) 计数
 * @param   pulse     比较值：占空比 = CCR/(ARR+1)
 * @retval  true 成功
 */
bool oop_tim_pwm_init(TIM_HandleTypeDef *htim, uint32_t channel,
                      uint32_t prescaler, uint32_t period, uint32_t pulse);

/**
 * @brief   启动某通道 PWM 输出（载波门控：开）
 */
bool oop_tim_pwm_start(TIM_HandleTypeDef *htim, uint32_t channel);

/**
 * @brief   停止某通道 PWM 输出（载波门控：关）
 */
bool oop_tim_pwm_stop(TIM_HandleTypeDef *htim, uint32_t channel);

/**
 * @brief   PWM 反初始化（停定时器 + 释放句柄配置）
 */
bool oop_tim_pwm_deinit(TIM_HandleTypeDef *htim);

#ifdef __cplusplus
}
#endif

#endif /* __OOP_TIM_DRV_H */
