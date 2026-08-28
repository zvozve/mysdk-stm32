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
#include "hal_platform.h"   /* IWDG_HandleTypeDef / HAL_IWDG_Refresh（不依赖工程 gpio.h） */

/**
 * @brief   初始化心跳（注入 LED 引脚与看门狗句柄，SDK 不记录任何具体 IO/句柄）
 * @param  led_port, led_pin  心跳指示灯 GPIO（由工程 board_cfg 注入）
 * @param  hiwdg              独立看门狗句柄；传 NULL 表示不喂狗（由工程自行处理）
 * @note   看门狗的 MX 初始化由工程完成，SDK 只负责在 run() 里刷新传入的句柄。
 */
void heart_beat_init(GPIO_TypeDef *led_port, uint16_t led_pin, IWDG_HandleTypeDef *hiwdg);

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