/* ============================================================
 * ch9434_port.h - CH9434（SPI 转 4 路串口）板级适配层
 *
 * 定位：CH9434 是板载外挂芯片（非 STM32 片上外设），故归 devices 层，
 * 坐 chip/oop_spi 与 chip/oop_gpio 之上，不直调 HAL_*。
 *
 * ch9434.h（芯片寄存器级驱动）只通过 3 个"用户层接口"访问硬件：
 *     CH9434_US_DELAY / CH9434_SPI_SCS_OP / CH9434_SPI_WRITE_BYTE
 * 本层即这 3 个接口的实现，并负责把 SPI 句柄与 CS/INT 引脚经
 * CH9434_Bind() 由工程 board_cfg 注入 —— SDK 不 extern 任何全局句柄、
 * 不含任何板级引脚。
 * ============================================================ */

#ifndef __CH9434_PORT_H
#define __CH9434_PORT_H

#include <stdint.h>
#include "hal_platform.h"   /* STM32 系列 HAL 统一入口（SPI/GPIO 句柄类型） */
#include "ch9434.h"         /* 芯片寄存器接口（其 extern 的用户层接口由本层实现） */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 每路串口独立配置参数
 * data_bits 使用 CH9434_UART_8_BITS_PER_CHAR 等常量(5/6/7/8)
 * stop_bits 使用 CH9434_UART_ONE_STOP_BIT / TWO_STOP_BITS
 * parity    使用 CH9434_UART_NO_PARITY / ODD / EVEN / MARK / SPACE
 */
typedef struct {
    uint32_t baudrate;   /* 波特率, 如 115200/9600 */
    uint8_t  data_bits;  /* 数据位 5/6/7/8 */
    uint8_t  stop_bits;  /* 停止位 1/2 */
    uint8_t  parity;     /* 校验位 0=无 1=奇 2=偶 3=mark 4=space */
} ch9434_uart_cfg_t;

/**
 * @brief 板级绑定：注入 SPI 句柄与 CS/INT 引脚（去 main.h / extern hspi3 硬编码）
 * @param hspi     CH9434 所挂 SPI 句柄（如 &hspi3）
 * @param cs_port  CS 引脚端口（低电平选中）
 * @param cs_pin   CS 引脚
 * @param int_port INT 引脚端口（输入上拉）
 * @param int_pin  INT 引脚
 * @note  SPI 传输本身按引用计数为阻塞式，超时沿用 oop_spi 默认 HAL_MAX_DELAY
 *        （与原 HAL_SPI_TransmitReceive(..., HAL_MAX_DELAY) 行为一致）。
 */
void CH9434_Bind(SPI_HandleTypeDef *hspi,
                 GPIO_TypeDef *cs_port,  uint16_t cs_pin,
                 GPIO_TypeDef *int_port, uint16_t int_pin);

void    CH9434_Init(void);
uint8_t CH9434_Send(uint8_t uart_idx, uint8_t *send_buf, uint8_t size);

/* 独立设置某一路串口的波特率/数据位/停止位/校验位（立即生效） */
void ch9434_uart_set_params(uint8_t uart_idx, const ch9434_uart_cfg_t *cfg);
/* 获取某一路当前配置 */
void ch9434_uart_get_params(uint8_t uart_idx, ch9434_uart_cfg_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* __CH9434_PORT_H */
