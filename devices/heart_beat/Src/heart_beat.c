/**
 * @file    heart_beat.c
 * @brief   心跳模块 - 极简版实现
 */

#include "heart_beat.h"
#include "SEGGER_RTT_Log.h"
#include "gpio.h"

// 单个静态心跳 LED
static gpio_dev_t g_heart_beat_led = {0};
static bool g_initialized = false;

/**
 * @brief   初始化心跳
 */
void heart_beat_init(void) {
    if (g_initialized) {
        return;
    }
    
    // 1. 初始化看门狗
#if USE_IWDG
    extern void MX_IWDG_Init(void);
    MX_IWDG_Init();
    SYS_LOG("IWDG initialized");
#endif

    // 2. 初始化 LED
#ifdef CPU_STA_GPIO_Port
    if (bsp_gpio_init_output(&g_heart_beat_led, CPU_STA_GPIO_Port, CPU_STA_Pin, true)) {
        g_initialized = true;
        SYS_LOG("Heartbeat initialized on CPU_STA, watchdog: %s", 
                USE_IWDG ? "enabled" : "disabled");
    } else {
        ERR_LOG("Heartbeat initialization failed!");
    }
#else
    #error "CPU_STA_GPIO_Port not defined, please define a default LED"
#endif
}

/**
 * @brief   翻转心跳 LED
 */
void heart_beat_run(void) {
    // 1. 翻转 LED
    if (g_initialized) {
        bsp_gpio_toggle(&g_heart_beat_led);
        // SYS_LOG("bsp_gpio_toggle");
    }

    // 2. 喂看门狗
#if USE_IWDG
    extern IWDG_HandleTypeDef hiwdg;
    HAL_IWDG_Refresh(&hiwdg);
#endif
}

/**
 * @brief   获取 GPIO 设备指针
 */
gpio_dev_t* heart_beat_get_gpio(void) {
    return g_initialized ? &g_heart_beat_led : NULL;
}