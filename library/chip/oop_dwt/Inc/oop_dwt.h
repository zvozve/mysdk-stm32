/*
*********************************************************************************************************
*	模块名称 : 数据观察点与跟踪(DWT)模块
*	文件名称 : oop_dwt.h
*	版    本 : V2.0
*	说    明 : DWT延时模块，支持RTOS和裸机
*********************************************************************************************************
*/

#ifndef __OOP_DWT_H
#define __OOP_DWT_H

#include "hal_platform.h"   /* STM32 系列 HAL 统一入口：提供 SystemCoreClock / HAL_Delay 等，不依赖工程 main.h */

/* RTOS支持 */
#ifdef __RTOS__
    #include "cmsis_os2.h"
#endif

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
 */
void oop_DelayMS(uint32_t ms);

/**
 * @brief 获取当前CPU周期计数
 * @return 32位周期计数值
 */
uint32_t oop_GetCycleCount(void);

/**
 * @brief 获取系统节拍（毫秒）
 * @return 自启动以来的毫秒数（封装 HAL_GetTick / SysTick 时基）
 * @note  device 层统一走本接口，避免直调 HAL_GetTick。
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
 */
void HAL_Delay(uint32_t Delay);

#endif /* __OOP_DWT_H */