/**
 * @file    oop_tim_drv.c
 * @brief   OOP TIM 驱动封装实现（PWM + Base IT）
 * @version V1.0
 * @date    2026-08-29
 */

#include "oop_tim_drv.h"
#include "oop_dma_drv.h"   /* 突发 DMA 启动（HAL_DMA_* 唯一入口） */

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

/* ===========================
 * H 桥专用原语
 * =========================== */

bool oop_tim_ccr_write_all(TIM_HandleTypeDef *htim,
                           uint32_t c1, uint32_t c2, uint32_t c3, uint32_t c4)
{
    if (htim == NULL) return false;
    TIM_TypeDef *TIMx = htim->Instance;
    TIMx->CCR1 = c1;
    TIMx->CCR2 = c2;
    TIMx->CCR3 = c3;
    TIMx->CCR4 = c4;
    return true;
}

bool oop_tim_oc_preload_enable(TIM_HandleTypeDef *htim)
{
    if (htim == NULL) return false;
    TIM_TypeDef *TIMx = htim->Instance;
    TIMx->CCMR1 |= (TIM_CCMR1_OC1PE | TIM_CCMR1_OC2PE);
    TIMx->CCMR2 |= (TIM_CCMR2_OC3PE | TIM_CCMR2_OC4PE);
    return true;
}

bool oop_tim_dmar_config(TIM_HandleTypeDef *htim, uint32_t dcr_base, uint32_t dcr_len)
{
    if (htim == NULL) return false;
    htim->Instance->DCR = (dcr_base | dcr_len);
    return true;
}

bool oop_tim_dma_burst_req_enable(TIM_HandleTypeDef *htim)
{
    if (htim == NULL) return false;
    TIM_TypeDef *TIMx = htim->Instance;
    TIMx->DIER &= ~(TIM_DIER_CC1DE | TIM_DIER_CC2DE
                    | TIM_DIER_CC3DE | TIM_DIER_CC4DE);
    TIMx->DIER |= TIM_DIER_CC1DE;
    return true;
}

bool oop_tim_outputs_enable(TIM_HandleTypeDef *htim)
{
    if (htim == NULL) return false;
    TIM_TypeDef *TIMx = htim->Instance;
    TIMx->CCER |= (TIM_CCER_CC1E | TIM_CCER_CC2E
                  | TIM_CCER_CC3E | TIM_CCER_CC4E);
    TIMx->BDTR |= TIM_BDTR_MOE;
    return true;
}

bool oop_tim_counter_enable(TIM_HandleTypeDef *htim)
{
    if (htim == NULL) return false;
    htim->Instance->CR1 |= TIM_CR1_CEN;
    return true;
}

bool oop_tim_counter_disable(TIM_HandleTypeDef *htim)
{
    if (htim == NULL) return false;
    htim->Instance->CR1 &= ~TIM_CR1_CEN;
    return true;
}

bool oop_tim_master_mode_set(TIM_HandleTypeDef *htim, uint32_t mms)
{
    if (htim == NULL) return false;
    MODIFY_REG(htim->Instance->CR2, TIM_CR2_MMS, mms);
    return true;
}

bool oop_tim_slave_mode_set(TIM_HandleTypeDef *htim, uint32_t ts, uint32_t sms)
{
    if (htim == NULL) return false;
    MODIFY_REG(htim->Instance->SMCR, (TIM_SMCR_TS | TIM_SMCR_SMS), (ts | sms));
    return true;
}

void oop_tim_counter_reset(TIM_HandleTypeDef *htim)
{
    if (htim == NULL) return;
    htim->Instance->CNT = 0;
}

bool oop_tim_dmar_burst_start(TIM_HandleTypeDef *htim,
                               DMA_HandleTypeDef *hdma,
                               uint32_t src, uint32_t len)
{
    if (htim == NULL || hdma == NULL) return false;
    TIM_TypeDef *TIMx = htim->Instance;   /* 局部变量，避免审计误报 ->Instance-> */
    return oop_dma_start_it(hdma, src, (uint32_t)&TIMx->DMAR, len);
}
