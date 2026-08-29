#ifndef __BOARD_CFG_H
#define __BOARD_CFG_H

/*
 * board_cfg.h —— 工程侧硬件绑定（类 devicetree，由 sync_lib.py 生成模板）
 *
 * 规则：
 *   - 全工程唯一允许 include CubeMX 生成头（main.h / usart.h / tim.h / gpio.h ...）
 *     的地方就是本文件；SDK（library/）不做任何绑定。
 *   - app / tasks 只引用本文件的绑定宏，不直接引用 MX 符号。
 *   - 换板只改本文件（引脚、句柄、时钟），SDK 与业务代码不动。
 */

#include "main.h"
#include "usart.h"
#include "tim.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========== 绑定区（按实际板卡填写） ========== */

/* TODO: 示例——红外接收 1838B：引脚 + 1ms 节拍 TIM 句柄 */
/* #define BOARD_IR_RX_PORT        VS1838B_DAT_GPIO_Port  */
/* #define BOARD_IR_RX_PIN         VS1838B_DAT_Pin       */
/* #define BOARD_IR_RX_TIM         (&htim6)              */

/* TODO: 示例——心跳 LED + 看门狗句柄（无 IWDG 传 NULL） */
/* #define BOARD_HEART_LED_PORT    CPU_STA_GPIO_Port     */
/* #define BOARD_HEART_LED_PIN     CPU_STA_Pin           */
/* #define BOARD_HEART_IWDG        NULL                  */

/* TODO: 示例——红外发射：复合配置（引脚/定时器/载波参数） */
/* #define BOARD_IR_TX_CFG         { GPIOE, GPIO_PIN_6, GPIO_AF3_TIM9, \
                                    TIM9, TIM_CHANNEL_2, 167, 25, 9 } */

/* TODO: 示例——非 CubeMX 管理的外设时钟（驱动不再接管 RCC，由工程开启） */
/* static inline void board_io_init(void) */
/* { */
/*     __HAL_RCC_TIM9_CLK_ENABLE(); */
/* } */

#ifdef __cplusplus
}
#endif

#endif /* __BOARD_CFG_H */
