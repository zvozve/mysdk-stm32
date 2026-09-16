/* ============================================================
 * dip_switch.h - 拨码开关硬件抽象（devices 层）
 *
 * SDK 版：由工程 Hardware/dip_switch 迁入 devices 层。去掉 CubeMX GPIO 头
 * 与 RCC 硬编码，改为 DIP_Init(map[8]) 注入 8 路引脚映射
 * （板级绑定，单一来源）；GPIO 访问统一走 oop_gpio。RCC 时钟由板级
 * （CubeMX MX_GPIO_Init 或 board_cfg 初始化）负责使能，本模块不碰 HAL。
 *
 * 位定义：物理拨码位 → 掩码（与 task_dip_switch 共享，单一来源）。
 * map[0] 对应 bit0，map[7] 对应 bit7。
 * ============================================================ */

#ifndef __DIP_SWITCH_H__
#define __DIP_SWITCH_H__

#include <stdint.h>
#include <stdbool.h>
#include "hal_platform.h"   /* GPIO_TypeDef / uint16_t 等类型 */

/* 位定义：物理拨码位 → 掩码（与 task_dip_switch 共享，单一来源） */
#define DIP_BIT_1   (1 << 0)
#define DIP_BIT_2   (1 << 1)
#define DIP_BIT_3   (1 << 2)
#define DIP_BIT_4   (1 << 3)
#define DIP_BIT_5   (1 << 4)
#define DIP_BIT_6   (1 << 5)
#define DIP_BIT_7   (1 << 6)
#define DIP_BIT_8   (1 << 7)

/* 单路引脚定义（板级注入） */
typedef struct {
    GPIO_TypeDef *port;
    uint16_t      pin;
} dip_pin_t;

/*
 * 拨码开关硬件抽象（原 bsp_dip_switch 已移除，下沉到 Hardware/Devices 层）。
 * IO 入口统一走 oop_gpio：本模块在 DIP_Init 内通过 oop_gpio_init_input
 * 注册 8 路输入（active_low：引脚 RESET=位1），读引脚经 oop_gpio_read。
 * 单实例硬件，IO 注册封装在模块内部，不对外暴露通用注册 API。
 * 轮询由本模块自行提供（DIP_Poll），上层只挂一个变化回调即可。
 *
 * 注意：本模块不使能 GPIO 时钟（devices 层不碰 HAL）。调用方须保证
 * map[] 涉及的 GPIO 端口时钟已由板级（MX_GPIO_Init / board_cfg）使能。
 */

/* 初始化：注入 8 路引脚映射 + 经 oop_gpio 注册输入 */
void DIP_Init(const dip_pin_t map[8]);

/* 读取当前 8 位原始值（每次实时读） */
uint8_t DIP_ReadRaw(void);

/* 查询某一位（基于最近一次读值） */
bool DIP_GetBit(uint8_t bit);

/* 轮询：刷新读值；相对上次变化则触发 notify 回调 */
void DIP_Poll(void);

/* 注册变化回调（old_val/new_val）；不注册则仅刷新读值 */
void DIP_SetNotify(void (*cb)(uint8_t old_val, uint8_t new_val));

#endif
