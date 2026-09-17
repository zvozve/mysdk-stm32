/* ============================================================
 * ch374_port.h - CH374（SPI USB Host 控制器）板级适配层
 *
 * 定位：CH374 是板载外挂芯片（非 STM32 片上外设），故归 devices 层，
 * 坐 chip/oop_spi、chip/oop_gpio、chip/oop_dwt 之上，不直调 HAL_*。
 *
 * ch374.c（芯片寄存器级驱动）只通过本文件暴露的少量接口访问硬件：
 *     CH374_SPI_ReadWriteByte / CH374T_CS_HIGH|LOW / CH374_INT_WIRE
 * 本层即这些接口的实现，SPI 句柄与 CS/INT/电源引脚由 CH374_Bind()
 * 经工程 board_cfg 注入 —— SDK 不 extern 任何全局句柄、不含板级引脚。
 *
 * 同时承载 HID 数据回调注册（驱动 → 任务层的单向事件出口）。
 * ============================================================ */

#ifndef __CH374_PORT_H
#define __CH374_PORT_H

#include <stdint.h>
#include "hal_platform.h"     /* STM32 系列 HAL 统一入口（SPI/GPIO 句柄类型） */
#include "SEGGER_RTT_Log.h"   /* RTT_LOG_TAG 引擎 */

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------
 * 模块专属日志标签（SDK 约定：模块最底层头自行定义，不集中放 SEGGER_RTT_Log.h）
 * ------------------------------------------------------------ */
#ifndef EUSB_LOG_ENABLE
#define EUSB_LOG_ENABLE   1
#endif
#define EUSB_LOG(fmt, ...)  RTT_LOG_TAG(EUSB_LOG_ENABLE, "EUSB", fmt, ##__VA_ARGS__)

/* ------------------------------------------------------------
 * 板级绑定
 * ------------------------------------------------------------ */

/**
 * @brief 注入 SPI 句柄与 CS / INT / USB 电源引脚（去 main.h / extern hspi2 硬编码）
 * @param hspi      CH374 所挂 SPI 句柄（如 &hspi2，硬件 SPI mode3）
 * @param cs_port   CS 片选端口（低电平选中）
 * @param cs_pin    CS 片选引脚
 * @param int_port  INT 中断输入端口（上拉，低有效）
 * @param int_pin   INT 中断输入引脚
 * @param pwr_port  USB 供电控制端口（原 GPIO_Toggle_INIT 中的 USB_PWR）
 * @param pwr_pin   USB 供电控制引脚
 * @note  本函数同时完成原 GPIO_Toggle_INIT() 的工作：CS 置无效、USB 供电使能、
 *        INT 配为上拉输入。SPI 超时沿用原值 100ms。
 */
void CH374_Bind(SPI_HandleTypeDef *hspi,
                GPIO_TypeDef *cs_port,  uint16_t cs_pin,
                GPIO_TypeDef *int_port, uint16_t int_pin,
                GPIO_TypeDef *pwr_port, uint16_t pwr_pin);

/* ------------------------------------------------------------
 * 片选 / 中断 / 电源（ch374.c 通过这些宏访问硬件）
 * ------------------------------------------------------------ */

void    CH374_CS_High(void);      /* CS 无效（高） */
void    CH374_CS_Low(void);       /* CS 有效（低） */
uint8_t CH374_INT_Level(void);    /* INT# 引脚原始电平：1=高（未中断）, 0=低（有中断） */
void    CH374_UsbPower(uint8_t on);/* USB 供电使能（1=使能） */

#define CH374T_CS_HIGH    CH374_CS_High()    /* CH374T 片选 */
#define CH374T_CS_LOW     CH374_CS_Low()
#define CH374_INT_WIRE    CH374_INT_Level()  /* INT# 中断查询（低有效） */

/* ------------------------------------------------------------
 * 底层字节收发（供 ch374.c 使用）
 * ------------------------------------------------------------ */

/**
 * @brief SPI 单字节全双工收发（CH374 寄存器式访问的基本操作）
 * @param  byte 待发送字节
 * @return 接收到的字节（传输失败返回 0xFF）
 */
uint8_t CH374_SPI_ReadWriteByte(uint8_t byte);

/* ------------------------------------------------------------
 * HID 设备类型
 * ------------------------------------------------------------ */
typedef enum {
    CH374_HID_NONE = 0,
    CH374_HID_KEYBOARD,   /* 键盘/扫码枪（Boot Keyboard） */
    CH374_HID_MOUSE,      /* 鼠标 */
} ch374_hid_type_t;

/**
 * HID 数据回调。
 * 由任务层通过 ch374_register_event_cb 注入。
 * 每个参数都携带 HubIndex（0/1/2），上层可据此区分 3 个物理端口。
 */
typedef struct {
    /* 设备枚举就绪后上报类型 */
    void (*on_dev_ready)(uint8_t hub_index, ch374_hid_type_t type);

    /* 设备断开/清除后上报（用于清理该端口的状态） */
    void (*on_dev_remove)(uint8_t hub_index);

    /* 键盘/扫码枪：收到一个可打印字符或 Enter/Backspace */
    /* key 为 ASCII 码；Enter=0x0A, Backspace=0x08, 其余为可见字符 */
    void (*on_key)(uint8_t hub_index, uint8_t key);

    /* 鼠标：一次坐标移动 */
    void (*on_mouse_move)(uint8_t hub_index, int8_t dx, int8_t dy);

    /* 鼠标：按键状态变化，buttons 按位：bit0=左 bit1=右 bit2=中 */
    void (*on_mouse_button)(uint8_t hub_index, uint8_t buttons, uint8_t changed);

    /* 鼠标：滚轮增量 */
    void (*on_mouse_wheel)(uint8_t hub_index, int8_t delta);
} ch374_event_cb_t;

/* 注册 HID 数据回调（返回之前注册的，或 NULL） */
const ch374_event_cb_t *ch374_register_event_cb(const ch374_event_cb_t *cb);

/* 获取当前回调（供 CH374.c 内部使用，无注册时为 NULL） */
const ch374_event_cb_t *ch374_get_event_cb(void);

#ifdef __cplusplus
}
#endif

#endif /* __CH374_PORT_H */
