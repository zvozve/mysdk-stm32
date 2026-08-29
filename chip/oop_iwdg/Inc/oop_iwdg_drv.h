/**
 * @file    oop_iwdg_drv.h
 * @brief   OOP IWDG 独立看门狗封装
 * @version V1.0
 * @date    2026-08-29
 *
 * 设计：看门狗句柄由调用方（工程 board_cfg）注入，本层只做板无关 OOP 封装，
 * HAL_IWDG_Refresh 是唯一的 vendor 边界。工程未启用 IWDG 时整体编译为空操作，
 * 因此 device 层（heart_beat）无需自行 #ifdef 守卫，直接调用即可。
 */

#ifndef __OOP_IWDG_DRV_H
#define __OOP_IWDG_DRV_H

#include <stdint.h>
#include <stdbool.h>
#include "hal_platform.h"   /* IWDG_HandleTypeDef / HAL_IWDG_Refresh（不依赖工程） */

/* 工程未启用 IWDG 模块时，HAL 不提供 IWDG_HandleTypeDef 真实定义，
 * 这里给出不透明前置声明，使 oop_iwdg_refresh(NULL) 仍可编译；
 * 模块启用时由 stm32f4xx_hal_iwdg.h 提供真实定义，此声明自动跳过。 */
#ifndef HAL_IWDG_MODULE_ENABLED
typedef struct __IWDG_HandleTypeDef IWDG_HandleTypeDef;
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief   刷新（喂）独立看门狗
 * @param   hiwdg  看门狗句柄；传 NULL 或模块未启用时不动作
 * @note    device 层统一调用本函数，无需自行 #ifdef HAL_IWDG_MODULE_ENABLED。
 */
void oop_iwdg_refresh(IWDG_HandleTypeDef *hiwdg);

#ifdef __cplusplus
}
#endif

#endif /* __OOP_IWDG_DRV_H */
