/*
*********************************************************************************************************
*	模块名称 : 数据观察点与跟踪(DWT)模块
*	文件名称 : oop_dwt.h
*	版    本 : V2.1
*	说    明 : 延时/计时模块——µs 走 DWT，ms 时基与延时自动适配 RTOS / 裸机
*
*	运行模式开关 BOARD_USE_RTOS 来自工程 User/board_cfg.h 的 BOARD_USE_RTOS
*	（经 hal_platform.h 归一化）。本头不 include 任何 RTOS 头：
*	RTOS 分支只存在于 oop_dwt.c 内部，上层一律用 oop_DelayMS / oop_GetTickMS，
*	不感知宿主是否跑 RTOS。
*********************************************************************************************************
*/

#ifndef __OOP_DWT_H
#define __OOP_DWT_H

#include "hal_platform.h"   /* STM32 系列 HAL 统一入口：提供 SystemCoreClock / HAL_Delay 等，
                             * 并由其中的 BOARD_USE_RTOS 给出 RTOS/裸机模式 */

/* 寄存器定义 */
#define DWT_CYCCNT      *(volatile unsigned int *)0xE0001004
#define DWT_CR          *(volatile unsigned int *)0xE0001000
#define DEM_CR          *(volatile unsigned int *)0xE000EDFC
#define DBGMCU_CR       *(volatile unsigned int *)0xE0042004

#define DEM_CR_TRCENA           (1 << 24)
#define DWT_CR_CYCCNTENA        (1 << 0)

/* ===========================
 * API
 * =========================== */

/**
 * @brief 初始化DWT（必须在使用前调用）
 */
void oop_InitDWT(void);

/**
 * @brief 微秒级阻塞延时
 * @param us 延时微秒数（最大约1秒，防止32位溢出）
 */
void oop_DelayUS(uint32_t us);

/**
 * @brief 毫秒级阻塞延时
 * @param ms 延时毫秒数
 * @note  RTOS 工程（BOARD_USE_RTOS=1）且调度器已启动时走 vTaskDelay 让出 CPU；
 *        调度器未启动（main 初始化期）或裸机工程走 DWT 忙等。
 */
void oop_DelayMS(uint32_t ms);

/**
 * @brief 获取当前CPU周期计数
 * @return 32位周期计数值
 */
uint32_t oop_GetCycleCount(void);

/**
 * @brief 获取系统节拍（毫秒）
 * @return 自启动以来的毫秒数
 * @note  全 SDK 唯一的毫秒时基出口：RTOS 工程取调度器 tick（configTICK_RATE_HZ
 *        必须整除 1000，编译期校验），裸机工程取 HAL 时基（SysTick / 工程配置的 TIM）。
 *        device / protocol / service / app 层统一走本接口，不得直调 HAL_GetTick 或
 *        xTaskGetTickCount —— 调用方不感知宿主是否跑 RTOS。
 */
uint32_t oop_GetTickMS(void);

/**
 * @brief 获取两次计数之间的时间差（微秒）
 * @param start 起始计数值
 * @param end   结束计数值
 * @return 微秒数
 */
uint32_t oop_GetElapsedUS(uint32_t start, uint32_t end);

/**
 * @brief 非阻塞延时（返回是否超时）
 * @param start  起始计数值（由 oop_GetCycleCount 获取）
 * @param us     延时微秒数
 * @return 0=未超时, 1=超时
 */
uint8_t oop_IsTimeout(uint32_t start, uint32_t us);

#ifdef HAL_Delay
    #undef HAL_Delay
#endif

/**
 * @brief 重定义HAL_Delay，自动适配RTOS/裸机
 * @note  RTOS 且调度器已启动 → 让出 CPU；否则 DWT 忙等。
 */
void HAL_Delay(uint32_t Delay);

#endif /* __OOP_DWT_H */