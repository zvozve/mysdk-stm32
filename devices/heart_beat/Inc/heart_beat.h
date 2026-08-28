/**
 * @file    heart_beat.h
 * @brief   心跳模块 - 极简版
 * @note    自动注册一个 LED，APP 只需调用 toggle
 */

#ifndef HEART_BEAT_H
#define HEART_BEAT_H

#include <stdint.h>
#include <stdbool.h>
#include "bsp_gpio_drv.h"

// 看门狗使能宏（默认关闭，在编译选项中定义 USE_IWDG=1 开启）
#ifndef USE_IWDG
#define USE_IWDG    0
#endif

/**
 * @brief   初始化心跳（自动注册默认 LED）
 * @note    使用 CPU_STA 作为默认心跳指示灯
 */
void heart_beat_init(void);

/**
 * @brief   翻转心跳 LED
 * @note    APP 在定时器或主循环中调用
 */
void heart_beat_run(void);

/**
 * @brief   获取心跳 GPIO 设备指针（可选）
 */
gpio_dev_t* heart_beat_get_gpio(void);

#endif