/**
 * @file    dip_switch.c
 * @brief   拨码开关硬件抽象（Devices 层）
 *
 * SDK 版：由工程 Hardware/dip_switch 迁入。所有 GPIO 访问统一经
 * oop_gpio：DIP_Init(map) 注入 8 路引脚映射并注册输入（active_low：
 * 引脚 RESET=位1），读引脚经 oop_gpio_read。RCC 时钟由板级负责，本模块
 * 不碰 HAL（符合"仅 chip 层用 HAL"分层铁律）。单实例，IO 注册封装在
 * 模块内部。
 */

#include "dip_switch.h"
#include "oop_gpio_drv.h"   /* oop_gpio_init_input / oop_gpio_read */

/* 拨码位 → 引脚 映射（由 DIP_Init 注入，单一来源） */
static const dip_pin_t *g_dip_map = NULL;
static gpio_dev_t g_dip_dev[8];
static uint8_t    g_current = 0;
static uint8_t    g_prev    = 0;
static void     (*g_notify)(uint8_t old_val, uint8_t new_val) = NULL;

void DIP_Init(const dip_pin_t map[8]) {
    g_dip_map = map;

    for (int i = 0; i < 8; i++) {
        /* active_low：引脚 RESET（闭合接地）→ 位1 */
        oop_gpio_init_input(&g_dip_dev[i], map[i].port, map[i].pin, false);
    }

    g_current = DIP_ReadRaw();
    g_prev    = g_current;
}

uint8_t DIP_ReadRaw(void) {
    uint8_t val = 0;
    for (int i = 0; i < 8; i++) {
        if (oop_gpio_read(&g_dip_dev[i]))   /* active_low → true 即位1 */
            val |= (uint8_t)(1u << i);
    }
    return val;
}

bool DIP_GetBit(uint8_t bit) {
    return (g_current & bit) != 0;
}

void DIP_Poll(void) {
    g_current = DIP_ReadRaw();
    if (g_current != g_prev) {
        if (g_notify)
            g_notify(g_prev, g_current);
        g_prev = g_current;
    }
}

void DIP_SetNotify(void (*cb)(uint8_t old_val, uint8_t new_val)) {
    g_notify = cb;
}
