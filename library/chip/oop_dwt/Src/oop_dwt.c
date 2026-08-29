/*
*********************************************************************************************************
*	模块名称 : 数据观察点与跟踪(DWT)模块
*	文件名称 : oop_dwt.c
*	版    本 : V2.0
*	说    明 : DWT延时实现，支持RTOS和裸机
*********************************************************************************************************
*/

#include "oop_dwt.h"

/* 运行状态标志 */
static uint8_t s_dwt_inited = 0;

/* ===========================
 * 初始化
 * =========================== */
void oop_InitDWT(void)
{
    /* 使能DWT跟踪 */
    DEM_CR |= (unsigned int)DEM_CR_TRCENA;
    
    /* 复位计数器 */
    DWT_CYCCNT = (unsigned int)0u;
    
    /* 使能周期计数 */
    DWT_CR |= (unsigned int)DWT_CR_CYCCNTENA;
    
    s_dwt_inited = 1;
    
    /* 给DWT一点稳定时间 */
    for (volatile int i = 0; i < 100; i++) {
        __NOP();
    }
}

/* ===========================
 * 核心延时函数
 * =========================== */
void oop_DelayUS(uint32_t us)
{
    if (!s_dwt_inited) {
        oop_InitDWT();
    }
    
    /* 防止溢出：最大延时限制在1秒内 */
    if (us > 1000000) {
        us = 1000000;
    }
    
    uint32_t tStart = DWT_CYCCNT;
    uint32_t tDelay = us * (SystemCoreClock / 1000000);
    uint32_t tCnt = 0;
    
    while (tCnt < tDelay) {
        tCnt = DWT_CYCCNT - tStart;
    }
}

void oop_DelayMS(uint32_t ms)
{
    /* 毫秒转微秒，防止溢出 */
    while (ms > 0) {
        uint32_t delay_us = (ms >= 1000) ? 1000000 : (ms * 1000);
        oop_DelayUS(delay_us);
        ms -= (ms >= 1000) ? 1000 : ms;
    }
}

/* ===========================
 * 辅助函数
 * =========================== */
uint32_t oop_GetCycleCount(void)
{
    if (!s_dwt_inited) {
        oop_InitDWT();
    }
    return DWT_CYCCNT;
}

uint32_t oop_GetTickMS(void)
{
    return HAL_GetTick();
}

uint32_t oop_GetElapsedUS(uint32_t start, uint32_t end)
{
    uint32_t diff = end - start;  /* 处理32位回绕 */
    return diff / (SystemCoreClock / 1000000);
}

uint8_t oop_IsTimeout(uint32_t start, uint32_t us)
{
    uint32_t now = oop_GetCycleCount();
    uint32_t elapsed_us = oop_GetElapsedUS(start, now);
    return (elapsed_us >= us) ? 1 : 0;
}

/* ===========================
 * HAL_Delay 重定义（自动适配RTOS/裸机）
 * =========================== */
void HAL_Delay(uint32_t Delay)
{
#ifdef __RTOS__
    /* RTOS模式：使用osDelay，让出CPU */
    if (osKernelGetState() == osKernelRunning) {
        osDelay(Delay);
        return;
    }
#endif
    
    /* 裸机模式或RTOS未启动：使用DWT阻塞延时 */
    oop_DelayMS(Delay);
}