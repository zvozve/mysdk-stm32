/**
 * @file    oop_tim_drv.c
 * @brief   OOP TIM 驱动封装实现（PWM + Base IT）
 * @version V1.0
 * @date    2026-08-29
 */

#include "oop_tim_drv.h"

/* ===========================
 * 通用时基（Base）
 * =========================== */

bool oop_tim_base_start_it(TIM_HandleTypeDef *htim)
{
    if (htim == NULL) {
        return false;
    }
    return (HAL_TIM_Base_Start_IT(htim) == HAL_OK);
}

bool oop_tim_base_stop_it(TIM_HandleTypeDef *htim)
{
    if (htim == NULL) {
        return false;
    }
    return (HAL_TIM_Base_Stop_IT(htim) == HAL_OK);
}

/* ===========================
 * PWM 载波 / 输出
 * =========================== */

bool oop_tim_pwm_init(TIM_HandleTypeDef *htim, uint32_t channel,
                      uint32_t prescaler, uint32_t period, uint32_t pulse)
{
    if (htim == NULL) {
        return false;
    }

    /* 时基（Instance 由调用方注入；外设时钟由工程 CubeMX 开启） */
    htim->Init.Prescaler         = prescaler;
    htim->Init.CounterMode       = TIM_COUNTERMODE_UP;
    htim->Init.Period            = period;
    htim->Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    htim->Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    if (HAL_TIM_PWM_Init(htim) != HAL_OK) {
        return false;
    }

    TIM_OC_InitTypeDef oc = {0};
    oc.OCMode     = TIM_OCMODE_PWM1;
    oc.Pulse      = pulse;
    oc.OCPolarity = TIM_OCPOLARITY_HIGH;
    oc.OCFastMode = TIM_OCFAST_DISABLE;
    if (HAL_TIM_PWM_ConfigChannel(htim, &oc, channel) != HAL_OK) {
        return false;
    }

    return true;
}

bool oop_tim_pwm_start(TIM_HandleTypeDef *htim, uint32_t channel)
{
    if (htim == NULL) {
        return false;
    }
    return (HAL_TIM_PWM_Start(htim, channel) == HAL_OK);
}

bool oop_tim_pwm_stop(TIM_HandleTypeDef *htim, uint32_t channel)
{
    if (htim == NULL) {
        return false;
    }
    return (HAL_TIM_PWM_Stop(htim, channel) == HAL_OK);
}

bool oop_tim_pwm_deinit(TIM_HandleTypeDef *htim)
{
    if (htim == NULL) {
        return false;
    }
    return (HAL_TIM_PWM_DeInit(htim) == HAL_OK);
}
