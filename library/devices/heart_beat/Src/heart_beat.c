/**
 * @file    heart_beat.c
 * @brief   心跳模块 - 极简版实现
 */

#include "heart_beat.h"
#include "SEGGER_RTT_Log.h"

// 单个静态心跳 LED
static gpio_dev_t g_heart_beat_led = {0};
static IWDG_HandleTypeDef *g_hiwdg = NULL;   /* 看门狗句柄，由 heart_beat_init 注入（NULL=不喂狗） */
static bool g_initialized = false;

/**
 * @brief   初始化心跳
 */
void heart_beat_init(GPIO_TypeDef *led_port, uint16_t led_pin, IWDG_HandleTypeDef *hiwdg) {
    if (g_initialized) {
        return;
    }
    if (led_port == NULL) {
        ERR_LOG("Heartbeat init FAIL: led_port=NULL");
        return;
    }
    g_hiwdg = hiwdg;   /* 看门狗的 MX 初始化由工程完成，SDK 只刷新句柄 */

    // 初始化 LED（引脚由工程注入，SDK 不记录具体 IO）
    if (oop_gpio_init_output(&g_heart_beat_led, led_port, led_pin, true)) {
        g_initialized = true;
        SYS_LOG("Heartbeat initialized, watchdog: %s",
                (hiwdg != NULL) ? "enabled" : "disabled");
    } else {
        ERR_LOG("Heartbeat initialization failed!");
    }
}

/**
 * @brief   翻转心跳 LED
 */
void heart_beat_run(void) {
    // 1. 翻转 LED
    if (g_initialized) {
        oop_gpio_toggle(&g_heart_beat_led);
        // SYS_LOG("oop_gpio_toggle");
    }

    // 2. 喂看门狗（句柄由工程注入；NULL 或模块未启用时 oop_iwdg_refresh 为空操作）
#if HEART_BEAT_IWDG_ENABLE
    oop_iwdg_refresh(g_hiwdg);
#endif
}

/**
 * @brief   获取 GPIO 设备指针
 */
gpio_dev_t* heart_beat_get_gpio(void) {
    return g_initialized ? &g_heart_beat_led : NULL;
}