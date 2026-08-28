/**
 * @file    oop_gpio_drv_hal.c
 * @brief   HAL GPIO 中断回调转发
 */

#include "oop_gpio_drv.h"

/**
 * @brief   HAL GPIO 中断回调
 * @note    由 stm32g4xx_it.c 中的 EXTI 中断调用
 */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin) {
    oop_gpio_irq_dispatch(GPIO_Pin);
}