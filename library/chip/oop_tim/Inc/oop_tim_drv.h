/**
 * @file    oop_tim_drv.h
 * @brief   OOP TIM 驱动封装（时基 / 输入捕获 / PWM 载波 / 输出门控）
 * @version V1.1
 * @date    2026-09-18
 *
 * 设计：TIM 句柄由调用方（工程 board_cfg / device cfg）注入，本层只做
 * 板无关的 OOP 封装，HAL_TIM_* 是唯一的 vendor 边界（与 oop_gpio_drv_hal.c 同理）。
 * device 层（ir_receiver / ir_transmitter）不再直调 HAL_TIM_*，统一走本封装。
 *
 * 分层约定：本文件里的原语对 device 层同样开放（device 只调 oop_*，不碰 HAL_*）。
 * 注：外设时钟（__HAL_RCC_TIMx_CLK_ENABLE）由工程 CubeMX 的 MspInit 开启，
 *     本层不接管时钟使能。
 */

#ifndef __OOP_TIM_DRV_H
#define __OOP_TIM_DRV_H

#include <stdint.h>
#include <stdbool.h>
#include "hal_platform.h"   /* TIM_HandleTypeDef / HAL_TIM_*（不依赖工程 tim.h） */

#ifdef __cplusplus
extern "C" {
#endif

/* ========== 通用时基（Base） ========== */

/**
 * @brief   按注入参数初始化通用时基（无输出、无中断）
 * @note    初始化后不启动计数器；htim->Instance 须由调用方先赋值。
 *          典型用途：把一路空闲 TIM 配成自由运行微秒计数器，
 *          PSC = TIM_CLK/1000000 - 1、Period = 0xFFFF（16 位定时器上限）。
 * @param   htim      TIM 句柄（Instance 已注入）
 * @param   prescaler 预分频：计数频率 = TIM_CLK/(PSC+1)
 * @param   period    AutoReload：模数 = Period+1
 * @retval  true 成功
 */
bool oop_tim_base_init(TIM_HandleTypeDef *htim, uint32_t prescaler, uint32_t period);

/**
 * @brief   反初始化通用时基（释放句柄配置）
 */
bool oop_tim_base_deinit(TIM_HandleTypeDef *htim);

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

/* ========== 计数器 / 周期（寄存器语义，无副作用） ========== */

/**
 * @brief   读当前计数值（CNT）
 * @note    自由运行时基上做「环形减法」测时的唯一取数口。
 */
uint32_t oop_tim_counter_get(TIM_HandleTypeDef *htim);

/**
 * @brief   写当前计数值（CNT）
 */
void oop_tim_counter_set(TIM_HandleTypeDef *htim, uint32_t cnt);

/**
 * @brief   读自动重装值（ARR）
 * @note    ARR+1 即计数器模数；驱动据此补偿回绕（16 位定时器通吃）。
 */
uint32_t oop_tim_period_get(TIM_HandleTypeDef *htim);

/**
 * @brief   写自动重装值（ARR）
 * @note    仅写寄存器，需配合 oop_tim_generate_update() 才立即生效。
 */
bool oop_tim_period_set(TIM_HandleTypeDef *htim, uint32_t period);

/**
 * @brief   产生一次更新事件（EGR UG）
 * @note    强制把 PSC/ARR/CCR 影子寄存器装载进活动寄存器并复位 CNT。
 *          载波频率这类「运行时改参数」靠它立即生效。
 */
void oop_tim_generate_update(TIM_HandleTypeDef *htim);

/* ========== 输入捕获 ========== */

/**
 * @brief   配置输入捕获通道（直连 TI，不启动）
 * @param   htim      TIM 句柄
 * @param   channel   TIM_CHANNEL_x
 * @param   polarity  TIM_INPUTCHANNELPOLARITY_RISING / _FALLING
 * @param   prescaler TIM_ICPSC_DIV1..
 * @param   filter    输入滤波 0..15
 * @retval  true 成功
 */
bool oop_tim_ic_init(TIM_HandleTypeDef *htim, uint32_t channel,
                     uint32_t polarity, uint32_t prescaler, uint32_t filter);

/**
 * @brief   启动输入捕获中断（通道 + 计数器）
 */
bool oop_tim_ic_start_it(TIM_HandleTypeDef *htim, uint32_t channel);

/**
 * @brief   停止输入捕获中断（只关通道/中断，计数器继续跑）
 */
bool oop_tim_ic_stop_it(TIM_HandleTypeDef *htim, uint32_t channel);

/**
 * @brief   读捕获值（CCRx）
 */
uint32_t oop_tim_ic_read_capture(TIM_HandleTypeDef *htim, uint32_t channel);

/**
 * @brief   翻转捕获边沿极性（CCxP）
 * @note    F1 的通用定时器 CCER 只有 1 位极性位、硬件不支持双沿捕获，
 *          驱动靠「每捕获一个边沿就翻转一次极性」用软件模拟双沿。
 */
void oop_tim_ic_toggle_polarity(TIM_HandleTypeDef *htim, uint32_t channel);

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

/**
 * @brief   写比较值（CCRx），立即生效
 * @note    载波「常开 + CCR 门控」的核心原语：mark 写占空比、space 写 0。
 *          前提是该通道已关闭输出比较预装载（见 oop_tim_oc_preload_disable），
 *          否则 CCR 要等下一个更新事件才生效，最坏引入一个载波周期抖动。
 */
bool oop_tim_ccr_write(TIM_HandleTypeDef *htim, uint32_t channel, uint32_t ccr);

/**
 * @brief   关闭某通道输出比较预装载（CCMRx OCxPE = 0）
 * @note    关掉后 CCR 直写直生效。HAL 默认行为由 Init.AutoReloadPreload 决定，
 *          驱动显式再关一次，避免 CubeMX 重新生成时被改回。
 */
bool oop_tim_oc_preload_disable(TIM_HandleTypeDef *htim, uint32_t channel);

/**
 * @brief   设置输出极性（含高级定时器的互补极性位 CCxNP）
 * @param   inverted false=高有效（mark 输出有效高电平），true=低有效
 * @note    同时决定「空号电平」：CCR=0 时输出为无效电平，
 *          高有效 -> 空号为低；低有效 -> 空号为高。
 */
bool oop_tim_oc_polarity_set(TIM_HandleTypeDef *htim, uint32_t channel, bool inverted);

/* ========== H 桥专用原语（突发 PWM / CCR 直写 / 主从同步） ==========
 *
 * 以下原语供 chip/hbridge 等需要精细控制 TIM 寄存器/突发 DMA 的模块使用。
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
