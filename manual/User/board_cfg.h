#ifndef __BOARD_CFG_H
#define __BOARD_CFG_H

/*
 * board_cfg.h —— 工程侧硬件绑定（类 devicetree，由 sdk-pull.py 生成模板）
 *
 * 规则：
 *   - 全工程唯一允许 include CubeMX 生成头（main.h / usart.h / tim.h / gpio.h ...）
 *     的地方就是本文件；SDK（library/）不做任何绑定。
 *   - app / tasks 只引用本文件的绑定宏，不直接引用 MX 符号。
 *   - 换板只改本文件（引脚、句柄、时钟），SDK 与业务代码不动。
 *   - 本文件不得 include 任何 SDK 头：hal_platform.h 会反过来 include 本文件
 *     （为了拿 BOARD_USE_RTOS），反向再引用会形成循环包含。
 */

#include "main.h"
#include "usart.h"
#include "tim.h"

/* ========== 功能开关（SDK 读取；与绑定区解耦） ========== */
/* 这些宏在编译期定义 SDK_BOARD_CFG 时被 SDK 头读取（hal_platform.h / modbus_core.h /
 * heart_beat.h），也是 SDK 构建模式的唯一真相源；也可被 CMake -D 显式覆盖（优先级更高）。 */

/* BOARD_USE_RTOS：宿主是 RTOS(1) 还是裸机(0)。SDK 据此归一化为 MB_USE_RTOS，且只在
 * chip/ 层使用（时基/延时自动切换）；devices/protocols/services/middleware/app 一律
 * 不得出现 RTOS 分支。改这一行即可在有/无 FreeRTOS 之间切换，不必再动 CMakeLists。 */
#define BOARD_USE_RTOS            0      /* 1 = FreeRTOS / CMSIS-RTOS2 宿主；0 = 裸机 */
#define BOARD_MODBUS_RTU_ENABLE   1      /* RTU 串行传输（仅依赖 UART） */
#define BOARD_MODBUS_TCP_ENABLE   0      /* TCP 传输：需要 LwIP 栈；无网口板保持 0 */
#define BOARD_HEART_IWDG_ENABLE   1      /* 心跳喂狗：无独立看门狗设 0 */

#ifdef __cplusplus
extern "C" {
#endif

/* ========== 绑定区（按实际板卡填写） ========== */

/* TODO: 示例——红外接收（解调接收头 OUT 接到捕获定时器通道引脚，1MHz 计数） */
/* #define BOARD_IR_RX_TIM          (&htim2)          */
/* #define BOARD_IR_RX_TIM_CHANNEL  TIM_CHANNEL_2     */
/* #define BOARD_IR_RX_TIM_CLK_HZ   1000000UL         */

/* TODO: 示例——心跳 LED + 看门狗句柄（无 IWDG 传 NULL） */
/* #define BOARD_HEART_LED_PORT    CPU_STA_GPIO_Port     */
/* #define BOARD_HEART_LED_PIN     CPU_STA_Pin           */
/* #define BOARD_HEART_IWDG        NULL                  */

/* TODO: 示例——红外发射：载波 PWM 通道 + 一路空闲 TIM 作 µs 时基 */
/* #define BOARD_IR_TX_TIM            (&htim3)        */
/* #define BOARD_IR_TX_TIM_CHANNEL    TIM_CHANNEL_2   */
/* #define BOARD_IR_TX_TIM_CLK_HZ     72000000UL      */
/* #define BOARD_IR_TX_TICK_TIM       (&htim4)        */
/* #define BOARD_IR_TX_TICK_CLK_HZ    72000000UL      */

/* TODO: 示例——非 CubeMX 管理的外设时钟（驱动不再接管 RCC，由工程开启） */
/* static inline void board_io_init(void) */
/* { */
/*     __HAL_RCC_TIM9_CLK_ENABLE(); */
/* } */

#ifdef __cplusplus
}
#endif

#endif /* __BOARD_CFG_H */
