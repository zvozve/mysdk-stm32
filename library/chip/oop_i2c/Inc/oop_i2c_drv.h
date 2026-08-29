/**
 * @file    oop_i2c_drv.h
 * @brief   软件 I2C（bit-bang）驱动，基于 OOP GPIO 抽象
 * @version V1.0
 * @date    2026-08-29
 *
 * @note    完全板无关：引脚由调用方注入（端口+引脚），不引用任何 CubeMX 符号。
 *          依赖 oop_gpio_drv（GPIO 抽象）与 oop_dwt（微秒级延时）。
 *          约定：GPIO 时钟由板级 MX_GPIO_Init 使能（与 oop_gpio 一致，本驱动不主动开时钟）。
 */

#ifndef __OOP_I2C_DRV_H
#define __OOP_I2C_DRV_H

#include <stdint.h>
#include <stdbool.h>
#include "oop_gpio_drv.h"
#include "oop_dwt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========== 软件 I2C 对象 ========== */
typedef struct {
    gpio_dev_t scl;
    gpio_dev_t sda;
    uint32_t   delay_us;   /* 每半位延时（微秒），不同实例可配不同速率 */
} soft_i2c_t;

/* ========== API ========== */

/**
 * @brief   初始化软件 I2C
 * @param   scl_port/scl_pin  时钟线端口与引脚
 * @param   sda_port/sda_pin  数据线端口与引脚
 * @param   delay_us          位延时（微秒），如 5
 * @retval  true: 成功  false: 参数无效
 * @note    SCL 配置为推挽输出，SDA 配置为开漏上拉；初始化后发 STOP 释放总线。
 */
bool oop_i2c_init(soft_i2c_t *i2c,
                  GPIO_TypeDef *scl_port, uint16_t scl_pin,
                  GPIO_TypeDef *sda_port, uint16_t sda_pin,
                  uint32_t delay_us);

void oop_i2c_start(soft_i2c_t *i2c);
void oop_i2c_stop(soft_i2c_t *i2c);
void oop_i2c_ack(soft_i2c_t *i2c);
void oop_i2c_nack(soft_i2c_t *i2c);

/**
 * @brief   等待从机应答
 * @retval  0=收到 ACK  1=超时/NACK
 */
uint8_t oop_i2c_wait_ack(soft_i2c_t *i2c);

void oop_i2c_send_byte(soft_i2c_t *i2c, uint8_t data);

/**
 * @brief   读取一个字节
 * @param   ack 1=读后发 ACK  0=读后发 NACK
 */
uint8_t oop_i2c_read_byte(soft_i2c_t *i2c, uint8_t ack);

#ifdef __cplusplus
}
#endif

#endif /* __OOP_I2C_DRV_H */
