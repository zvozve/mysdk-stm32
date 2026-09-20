/*
*********************************************************************************************************
*	模块名称 : 数据观察点与跟踪(DWT)模块
*	文件名称 : oop_dwt.c
*	版    本 : V2.1
*	说    明 : µs 延时走 DWT CYCCNT；ms 时基与延时按 BOARD_USE_RTOS 自动切 RTOS / 裸机
*
*	RTOS 分支只在本文件内出现（本文件是 chip 层，允许直接接触 RTOS 接口）；
*	对外只暴露 oop_GetTickMS / oop_DelayMS / HAL_Delay，上层不感知宿主是否跑 RTOS。
*********************************************************************************************************
*/

#include "oop_dwt.h"

#if BOARD_USE_RTOS
    /* 仅 chip 层允许 include RTOS 头；MODE 开关来自 board_cfg.h(BOARD_USE_RTOS) */
    #include "FreeRTOS.h"
    #include "task.h"

    /* oop_GetTickMS() 要把 tick 换算成 ms，故要求 1000 能被 tick 频率整除。
     * 常见配置 1000 / 500 / 250 / 200 / 100 都满足。注意：configTICK_RATE_HZ 常是
     * ((TickType_t)1000) 这种带 typedef 强转的写法，在 #if 预处理期解析不了，
     * 所以校验放在 C 常量表达式层（负数组长度 = 编译期断言），失败即编译报错，
     * 而不是安静地给出错误的时间戳。 */
    typedef char oop_dwt_tick_rate_must_divide_1000[
        ((1000u % configTICK_RATE_HZ) == 0) ? 1 : -1 ];
#endif

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
#if BOARD_USE_RTOS
    /* RTOS 且调度器已起：让出 CPU（忙等会饿死同优先级/低优先级任务）。
     * 调度器未启动（main 初始化期、osKernelStart 之前）不能调 vTaskDelay → 退回忙等。 */
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        vTaskDelay(pdMS_TO_TICKS(ms));
        return;
    }
#endif
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
#if BOARD_USE_RTOS
    /* 取调度器 tick 而非 HAL_GetTick：与调度器的时基严格一致，且不依赖「工程已把
     * CubeMX 的 HAL timebase 从 SysTick 挪到 TIM」这一不可见设置（忘了改就会静默停摆）。
     * configTICK_RATE_HZ 整除 1000 由文件头 #error 保证，故这里整数换算无损。 */
    return (uint32_t)(xTaskGetTickCount() * (1000u / configTICK_RATE_HZ));
#else
    return HAL_GetTick();
#endif
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
#if BOARD_USE_RTOS
    /* RTOS模式：调度器已起时让出CPU，避免 HAL 内部延时饿死其他任务 */
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        vTaskDelay(pdMS_TO_TICKS(Delay));
        return;
    }
#endif

    /* 裸机模式或RTOS未启动：使用DWT阻塞延时 */
    oop_DelayMS(Delay);
}