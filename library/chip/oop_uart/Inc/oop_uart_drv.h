/**
 * @file    oop_uart_drv.h
 * @brief   通用串口DMA驱动 - 状态机管理
 * @version V3.0
 * @date    2026-08-04
 * 
 * @note    功能说明：
 *          1. 基于DMA+空闲中断的串口数据收发
 *          2. 内置状态机管理：IDLE / TX_BUSY / DATA_READY / ERROR
 *          3. 支持RS485发送使能脚管理（由上层注入GPIO）
 *          4. 支持运行时波特率动态重配置
 *          5. 数据包锁定机制，防止接收数据被覆盖
 *          6. 完全硬件无关，由上层注入 huart 句柄
 *          7. 支持 UART1~UART8 全覆盖（通过描述表）
 * 
 * @note    使用流程：
 *          1. 上层分配 uart_drv_t 实例
 *          2. 调用 uart_drv_init() 注入 huart 和 RS485 配置
 *          3. 调用 uart_drv_reg_cb() 注册回调
 *          4. 中断中调用 uart_drv_on_tx_done() / uart_drv_on_idle() / uart_drv_on_error()
 */

#ifndef __UART_DRV_H__
#define __UART_DRV_H__

#include <stdint.h>
#include <stdbool.h>
#include "hal_platform.h"   /* STM32 系列 HAL 统一入口（移植层） */
#include "SEGGER_RTT_Log.h"   /* 复用 RTT_LOG_TAG；UART 驱动专属标签在此定义 */

#ifdef __cplusplus
extern "C" {
#endif

// ===========================
// UART 驱动专属日志标签
// ===========================
#ifndef UART_LOG_ENABLE
    #define UART_LOG_ENABLE    1
#endif
#define UART_LOG(fmt, ...)    RTT_LOG_TAG(UART_LOG_ENABLE,   "UART",   fmt, ##__VA_ARGS__)

/* 收发缓冲容量（每实例各一个 rx/tx）。
 * ⚠ 跑 YMODEM 时必须加大：一个 1024 字节数据包整帧是 1029 字节
 *   （1 + 1 + 1 + 1024 + 2），256 的默认值会被 DMA 覆盖 → 表现为「包随机 CRC 错」。
 *   工程侧 `-DUART_DRV_BUF_SIZE=1088` 即可（F407 上每实例多占 ~1.7 KB RAM）。
 *   只发 128 字节小包（帧长 133）时默认值就够。 */
#ifndef UART_DRV_BUF_SIZE
#define UART_DRV_BUF_SIZE   256
#endif

// ===========================
// 状态机
// ===========================
typedef enum {
    UART_DRV_IDLE = 0,      // 空闲，可以收发
    UART_DRV_TX_BUSY,       // 发送中
    UART_DRV_DATA_READY,    // 数据已就绪，等待上层读取
    UART_DRV_ERROR          // 错误
} uart_drv_state_t;

// ===========================
// 串口参数
// ===========================
typedef struct {
    uint32_t baudrate;
    /* ⚠ 以下三项直接承载 HAL 的 UART_WORDLENGTH_* / UART_STOPBITS_* / UART_PARITY_* 宏值，
     *   三者均是 USART_CR1/CR2 的寄存器位域（如 UART_STOPBITS_2 = 0x2000、UART_PARITY_ODD = 0x600、
     *   UART_WORDLENGTH_9B = 0x1000），必须用 uint32_t 存放。
     *   若收窄为 uint8_t，这些宏会被静默截断成 0x00 → 2 停止位变 1 停止位、奇校验变无校验，
     *   且编译器只在常量初始化时给一条 conversion 警告，运行时无任何报错。 */
    uint32_t word_length;
    uint32_t stop_bits;
    uint32_t parity;
    uint8_t  emulate_7bit;        // 7 位数据软件模拟开关（F4 硬件无 7 位字长，用 8N1 + 软件校验实现 7E1/7O1）
    uint8_t  emulate_7bit_parity; // 0=偶校验(7E1), 1=奇校验(7O1)
} uart_drv_cfg_t;

// ===========================
// RS485配置
// ===========================
typedef struct {
    GPIO_TypeDef *port;
    uint16_t      pin;
    uint8_t       active_level;  // 1=高电平发送, 0=低电平发送
} uart_rs485_t;

// ===========================
// 驱动实例
// ===========================
typedef struct uart_drv_instance {
    // 硬件句柄
    UART_HandleTypeDef *huart;
    uart_rs485_t *rs485;
    
    // 缓冲区
    uint8_t rx_buf[UART_DRV_BUF_SIZE];
    uint16_t rx_len;
    uint8_t tx_buf[UART_DRV_BUF_SIZE];
    
    // ★ 状态管理 ★
    volatile uart_drv_state_t state;
    uint8_t locked;              // 仅当 state=DATA_READY 时有效
    
    // 配置
    uart_drv_cfg_t cfg;

    // 7 位数据软件模拟状态（仅当 cfg.emulate_7bit 时生效，详见 uart_drv_reconfig/send/on_idle）
    uint8_t emulate_7bit;
    uint8_t emulate_7bit_parity;

    // 回调
    void (*on_recv)(struct uart_drv_instance *pInst, uint8_t *data, uint16_t len);
    void (*on_sent)(struct uart_drv_instance *pInst);
    void (*on_error)(struct uart_drv_instance *pInst);
} uart_drv_t;

// ===========================
// API
// ===========================

void uart_drv_init(uart_drv_t *drv, UART_HandleTypeDef *huart, uart_rs485_t *rs485);
int uart_drv_reconfig(uart_drv_t *drv, const uart_drv_cfg_t *cfg);

void uart_drv_reg_cb(uart_drv_t *drv,
                     void (*on_recv)(uart_drv_t *, uint8_t *, uint16_t),
                     void (*on_sent)(uart_drv_t *),
                     void (*on_error)(uart_drv_t *));

// ★ 发送接口 ★
int uart_drv_send(uart_drv_t *drv, const uint8_t *data, uint16_t len);

// ★ 接收接口 ★
uint8_t* uart_drv_get_packet(uart_drv_t *drv, uint16_t *len);
void uart_drv_release_packet(uart_drv_t *drv);

// ★ 查询接口 ★
uart_drv_state_t uart_drv_get_state(uart_drv_t *drv);
uint16_t uart_drv_available(uart_drv_t *drv);  // ★ 新增：查询可读数据长度

// ★ 控制接口 ★
void uart_drv_reset(uart_drv_t *drv);
void uart_drv_get_cfg(uart_drv_t *drv, uart_drv_cfg_t *cfg);

// ★ 工具接口 ★
const char* uart_drv_get_name(UART_HandleTypeDef *huart);
uint8_t uart_drv_get_index(UART_HandleTypeDef *huart);

// ===========================
// 中断入口
// ===========================
void uart_drv_on_tx_done(UART_HandleTypeDef *huart);
void uart_drv_on_idle(UART_HandleTypeDef *huart);
void uart_drv_on_error(UART_HandleTypeDef *huart);

#ifdef __cplusplus
}
#endif

#endif