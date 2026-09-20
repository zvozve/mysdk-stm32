/* ============================================================
 * hal_platform.h - STM32 系列 HAL 统一入口（移植层）
 *
 * 所有 User 驱动/任务统一 include 本头，而不是直接 include
 * "stm32g4xx_hal.h"，避免换芯片系列时改多处。
 *
 * 系列选择：按编译宏（CubeMX 生成的具体型号，如 STM32G474xx / STM32F103xE）
 * 自动展开对应系列 HAL 头，并导出平台宏供功能差异条件编译：
 *   HAL_PLATFORM_F1 / HAL_PLATFORM_F4 / HAL_PLATFORM_G4（=1 表示当前系列）
 *
 * 新增系列步骤：
 *   1. 在此补充 #elif defined(STM32xxx) 分支 + include 对应系列 HAL
 *   2. 定义 HAL_PLATFORM_xxx 平台宏
 *   3. 有功能差异的代码用平台宏条件编译（如 oop_uart_drv.c 的 UART 空闲事件收帧，
 *      G4 有 HAL_UARTEx_*，F1/F4 需用 DMA 循环 + 自行空闲检测）
 * ============================================================ */
#ifndef __HAL_PLATFORM_H__
#define __HAL_PLATFORM_H__

/* 默认非当前系列，各分支只把自己置 1，避免调用方遇到未定义宏 */
#define HAL_PLATFORM_F1   0
#define HAL_PLATFORM_F4   0
#define HAL_PLATFORM_G4   0

#if defined(STM32F100xB) || defined(STM32F100xE) || \
    defined(STM32F101x6) || defined(STM32F101xB) || \
    defined(STM32F101xE) || defined(STM32F101xG) || \
    defined(STM32F102x6) || defined(STM32F102xB) || \
    defined(STM32F103x6) || defined(STM32F103xB) || \
    defined(STM32F103xE) || defined(STM32F103xG) || \
    defined(STM32F105xC) || defined(STM32F107xC) || \
    defined(STM32F1xx)
    #include "stm32f1xx_hal.h"
    #undef  HAL_PLATFORM_F1
    #define HAL_PLATFORM_F1   1
#elif defined(STM32G431xx) || defined(STM32G441xx) || defined(STM32G471xx) || \
    defined(STM32G473xx) || defined(STM32G474xx) || defined(STM32G483xx) || \
    defined(STM32G484xx) || defined(STM32G491xx) || defined(STM32G4A1xx) || \
    defined(STM32G4xx)
    #include "stm32g4xx_hal.h"
    #undef  HAL_PLATFORM_G4
    #define HAL_PLATFORM_G4   1
#elif defined(STM32F401xx) || defined(STM32F405xx) || defined(STM32F407xx) || \
    defined(STM32F411xx) || defined(STM32F429xx) || defined(STM32F446xx) || \
    defined(STM32F4xx)
    #include "stm32f4xx_hal.h"
    #undef  HAL_PLATFORM_F4
    #define HAL_PLATFORM_F4   1
#else
    #error "hal_platform.h: unknown STM32 series (define e.g. STM32G474xx / STM32F407xx / STM32F103xE in build options)"
#endif

/* ============================================================
 * 运行模式：RTOS / 裸机 —— SDK 唯一开关，宏名就叫 BOARD_USE_RTOS
 *
 * 真相源 = 工程侧 User/board_cfg.h（与引脚绑定同一个文件，工程只维护一处）。
 * 这里只负责兜底：工程没填该宏时默认 0（裸机），不会另造第二个名字。
 * 优先级：-DBOARD_USE_RTOS > board_cfg.h > 默认 0（裸机）。
 *
 * 使用规则（红线）：
 *   - BOARD_USE_RTOS 的消费点只允许在 chip/ 层内部（如 oop_dwt 的时基/延时实现）；
 *   - devices / protocols / services / middleware / app 一律不得出现 RTOS 分支，
 *     也不得 include FreeRTOS 头 —— 裸机与 RTOS 的差异由 chip 层接口吸收。
 *   - 实现注记：这里刻意 include board_cfg.h（而不是让各层自己去拿开关），
 *     故 board_cfg.h 不得 include 任何 SDK 头，否则形成循环包含。
 * ============================================================ */
#ifdef SDK_BOARD_CFG
    #include "board_cfg.h"
#endif

#ifndef BOARD_USE_RTOS
    #define BOARD_USE_RTOS     0
#endif


#endif /* __HAL_PLATFORM_H__ */
