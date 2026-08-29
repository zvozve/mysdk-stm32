/* ============================================================
 * hal_platform.h - STM32 系列 HAL 统一入口（移植层）
 *
 * 所有 User 驱动/任务统一 include 本头，而不是直接 include
 * "stm32g4xx_hal.h"，避免换芯片系列时改多处。
 *
 * 系列选择：按编译宏（CubeMX 生成的具体型号，如 STM32G474xx）
 * 自动展开对应系列 HAL 头，并导出平台宏供功能差异条件编译：
 *   HAL_PLATFORM_G4 / HAL_PLATFORM_F4（=1 表示当前系列）
 *
 * 新增系列步骤：
 *   1. 在此补充 #elif defined(STM32xxx) 分支 + include 对应系列 HAL
 *   2. 定义 HAL_PLATFORM_xxx 平台宏
 *   3. 有功能差异的代码用平台宏条件编译（当前唯一差异点：
 *      oop_uart_drv.c 的 UART 空闲事件收帧，G4 有 HAL_UARTEx_*，
 *      F4 需用 DMA 循环 + 自行空闲检测）
 * ============================================================ */
#ifndef __HAL_PLATFORM_H__
#define __HAL_PLATFORM_H__

#if defined(STM32G431xx) || defined(STM32G441xx) || defined(STM32G471xx) || \
    defined(STM32G473xx) || defined(STM32G474xx) || defined(STM32G483xx) || \
    defined(STM32G484xx) || defined(STM32G491xx) || defined(STM32G4A1xx) || \
    defined(STM32G4xx)
    #include "stm32g4xx_hal.h"
    #define HAL_PLATFORM_G4   1
    #define HAL_PLATFORM_F4   0
#elif defined(STM32F401xx) || defined(STM32F405xx) || defined(STM32F407xx) || \
    defined(STM32F411xx) || defined(STM32F429xx) || defined(STM32F446xx) || \
    defined(STM32F4xx)
    #include "stm32f4xx_hal.h"
    #define HAL_PLATFORM_G4   0
    #define HAL_PLATFORM_F4   1
#else
    #error "hal_platform.h: unknown STM32 series (define e.g. STM32G474xx / STM32F407xx in build options)"
#endif

#endif /* __HAL_PLATFORM_H__ */
