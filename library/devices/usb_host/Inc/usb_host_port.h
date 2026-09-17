/**
 * @file    usb_host_port.h
 * @brief   USB Host (STM32 USBH + HID 类) 应用适配层
 * @version V2.0
 * @date    2026-09-17
 *
 * @note    板无关：USBH 句柄由 usb_host_port_bind() 注入；CubeMX 生成的
 *          Appli_state 不再被直接引用，改由工程在 USBH_UserProcess 的
 *          USER CODE 区内调用 usb_host_port_event() 上报事件。
 *          依赖 middleware/usbh（ST USB Host Library，随本 SDK 提供），
 *          其中 usbh_conf.h 由工程（CubeMX）提供。
 */

#ifndef __USB_HOST_PORT_H
#define __USB_HOST_PORT_H

#include <stdint.h>
#include "hal_platform.h"
#include "usbh_hid.h"        /* USBH_HandleTypeDef / HID_TypeTypeDef / HID_Keybd|Mouse API */
#include "SEGGER_RTT_Log.h"   /* RTT_LOG_TAG 引擎 */

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------
 * 模块专属日志标签（SDK 约定：模块最底层头自行定义）
 * 注意不要用 APP_LOG —— 那是工程应用层标签，SDK devices 不得依赖工程头。
 * ------------------------------------------------------------ */
#ifndef USB_LOG_ENABLE
#define USB_LOG_ENABLE   1
#endif
#define USB_LOG(fmt, ...)  RTT_LOG_TAG(USB_LOG_ENABLE, "USB", fmt, ##__VA_ARGS__)

// ===========================
// 配置
// ===========================
#define USB_HOST_RX_BUF_SIZE        128     // 环形缓冲区大小

// ===========================
// USB Host 设备状态
// ===========================
typedef enum {
    USB_HOST_DEV_IDLE = 0,
    USB_HOST_DEV_CONNECTED,
    USB_HOST_DEV_READY,
    USB_HOST_DEV_DISCONNECTED,
} usb_host_dev_state_t;

// ===========================
// 应用事件（由工程从 USBH_UserProcess 上报）
// ===========================
typedef enum {
    USB_HOST_EVT_START = 0,     /* 设备插入、开始枚举（对应 CubeMX APPLICATION_START） */
    USB_HOST_EVT_READY,         /* 类驱动就绪（对应 CubeMX APPLICATION_READY） */
    USB_HOST_EVT_DISCONNECT,    /* 设备拔出（对应 CubeMX APPLICATION_DISCONNECT） */
} usb_host_event_t;

// ===========================
// 函数声明
// ===========================

/**
 * @brief  注入 USBH 句柄（板级绑定，去 extern hUsbHostFS 硬编码）
 * @param  phost  CubeMX 生成的 USBH 句柄（如 &hUsbHostFS）
 * @note   须在 usb_host_port_init() 之前调用一次。
 */
void usb_host_port_bind(USBH_HandleTypeDef *phost);

/**
 * @brief  上报一个应用事件（由工程在 USBH_UserProcess 的 USER CODE 区内调用）
 * @param  evt  事件类型
 * @note   SDK 不引用 CubeMX 的 Appli_state / ApplicationTypeDef，
 *         由工程做「ApplicationType 枚举 → usb_host_event_t」的翻译。
 */
void usb_host_port_event(usb_host_event_t evt);

/**
 * @brief  USB Host HID 端口初始化（在 USB Host 库启动后调用）
 *         注册键盘回调、初始化环形缓冲区
 */
void usb_host_port_init(void);

/**
 * @brief  USB Host HID 轮询处理（由任务周期性调用）
 *         检查设备状态变化，就绪后轮询键盘输入并存入环形缓冲区
 */
void usb_host_port_process(void);

/**
 * @brief  从环形缓冲区读取一个字符（非阻塞）
 * @retval 读取到的 ASCII 字符；无数据时返回 0
 */
uint8_t usb_host_port_getchar(void);

/**
 * @brief  检查环形缓冲区是否有数据
 * @retval 1: 有数据, 0: 无数据
 */
uint8_t usb_host_port_has_data(void);

/**
 * @brief  获取当前 USB Host 设备状态
 */
usb_host_dev_state_t usb_host_port_get_state(void);

/**
 * @brief  获取当前已连接的 HID 设备类型
 * @retval HID_KEYBOARD / HID_MOUSE / HID_UNKNOWN
 */
HID_TypeTypeDef usb_host_port_get_dev_type(void);

#ifdef __cplusplus
}
#endif

#endif /* __USB_HOST_PORT_H */
