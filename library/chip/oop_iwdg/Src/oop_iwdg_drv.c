/**
 * @file    oop_iwdg_drv.c
 * @brief   OOP IWDG 独立看门狗封装实现
 * @version V1.0
 * @date    2026-08-29
 */

#include "oop_iwdg_drv.h"

void oop_iwdg_refresh(IWDG_HandleTypeDef *hiwdg)
{
#ifdef HAL_IWDG_MODULE_ENABLED
    if (hiwdg != NULL) {
        HAL_IWDG_Refresh(hiwdg);
    }
#else
    (void)hiwdg;   /* 模块未启用：整体编译为空操作 */
#endif
}
