/**
 * @file    oop_tim_drv.h
 * @brief   OOP TIM 驱动封装（PWM 载波 + 通用时基）
 * @version V1.0
 * @date    2026-08-29
 *
 * 设计：TIM 句柄由调用方（工程 board_cfg / device cfg）注入，本层只做
 * 板无关的 OOP 封装，HAL_TIM_* 是唯一的 vendor 边界（与 oop_gpio_drv_hal.c 同理）。
 * device 层（ir_transmitter / ir_receiver）不再直调 HAL_TIM_*，统一走本封装。
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

/* ========== H 桥专用原语（突发 PWM / CCR 直写 / 主从同步） ==========
 *
 * 以下原语供 chip/hbridge 等需要精细控制 TIM 寄存器/突发 DMA 的模块使用。
 * 它们仍属 chip 层（HAL 唯一入口层），device 层禁止调用。
 */

/**
 * @brief   一次性直写 4 路 CCR（H 桥 4 臂）
 */
bool oop_tim_ccr_write_all(TIM_HandleTypeDef *htim,
                           uint32_t c1, uint32_t c2, uint32_t c3, uint32_t c4);

/**
 * @brief   使能 4 路通道 CCR 预装载（CCMRx OCxPE）
 */
bool oop_tim_oc_preload_enable(TIM_HandleTypeDef *htim);

/**
 * @brief   配置 DMAR 突发（DCR = base | len），用于突发 DMA 连写 CCR
 * @param   dcr_base  TIM_DMABASE_CCR1 等
 * @param   dcr_len   TIM_DMABURSTLENGTH_xTRANSFERS
 */
bool oop_tim_dmar_config(TIM_HandleTypeDef *htim, uint32_t dcr_base, uint32_t dcr_len);

/**
 * @brief   使能突发 DMA 请求（仅 CC1DE，并清掉 CC2/3/4 DE）
 */
bool oop_tim_dma_burst_req_enable(TIM_HandleTypeDef *htim);

/**
 * @brief   使能所有 4 路比较输出 + BDTR MOE（H 桥桥臂门控全开）
 */
bool oop_tim_outputs_enable(TIM_HandleTypeDef *htim);

/**
 * @brief   启动计数器（CR1 CEN）
 */
bool oop_tim_counter_enable(TIM_HandleTypeDef *htim);

/**
 * @brief   停止计数器（CR1 ~CEN）
 */
bool oop_tim_counter_disable(TIM_HandleTypeDef *htim);

/**
 * @brief   设置主模式选择（CR2 MMS）
 */
bool oop_tim_master_mode_set(TIM_HandleTypeDef *htim, uint32_t mms);

/**
 * @brief   设置从模式（SMCR TS+SMS）
 */
bool oop_tim_slave_mode_set(TIM_HandleTypeDef *htim, uint32_t ts, uint32_t sms);

/**
 * @brief   复位计数（CNT = 0）
 */
void oop_tim_counter_reset(TIM_HandleTypeDef *htim);

/**
 * @brief   突发 DMA 写 DMAR（H 桥连写 CCR 用）
 *
 * 内部取 TIMx->DMAR 作为 DMA 目标地址并启动 IT 传输，调用方无需知道
 * DMAR 寄存器位置。等价于 HAL_TIM_DMABurst_WriteStart 的轻量版。
 *
 * @param   htim   TIM 句柄（Instance 已注入）
 * @param   hdma   突发 DMA 句柄（CC1）
 * @param   src    源地址（CCR 帧缓冲）
 * @param   len    传输字数（帧数 * 4）
 */
bool oop_tim_dmar_burst_start(TIM_HandleTypeDef *htim,
                               DMA_HandleTypeDef *hdma,
                               uint32_t src, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* __OOP_TIM_DRV_H */
