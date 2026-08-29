/**
 * @file    oop_i2c_drv.c
 * @brief   软件 I2C 实现（bit-bang），基于 OOP GPIO 抽象
 * @version V1.0
 * @date    2026-08-29
 *
 * @note    GPIO 操作经 oop_gpio 的 RAW 宏完成，本文件不直接接触 HAL 也不引用 CubeMX 符号。
 */

#include "oop_i2c_drv.h"

/* ========== 底层引脚原语（忽略 active_high，直接控硬件电平） ========== */
static inline void i2c_scl(soft_i2c_t *i2c, bool hi) {
    OOP_GPIO_WRITE_RAW(&i2c->scl, hi ? GPIO_PIN_SET : GPIO_PIN_RESET);
}
static inline void i2c_sda(soft_i2c_t *i2c, bool hi) {
    OOP_GPIO_WRITE_RAW(&i2c->sda, hi ? GPIO_PIN_SET : GPIO_PIN_RESET);
}
static inline bool i2c_read_sda(soft_i2c_t *i2c) {
    return OOP_GPIO_READ_RAW(&i2c->sda) == GPIO_PIN_SET;
}
static inline void i2c_delay(soft_i2c_t *i2c) {
    oop_DelayUS(i2c->delay_us);
}

/* ========== 初始化 ========== */
bool oop_i2c_init(soft_i2c_t *i2c,
                  GPIO_TypeDef *scl_port, uint16_t scl_pin,
                  GPIO_TypeDef *sda_port, uint16_t sda_pin,
                  uint32_t delay_us)
{
    if (i2c == NULL || scl_port == NULL || sda_port == NULL) return false;

    oop_gpio_init_output(&i2c->scl, scl_port, scl_pin, true);          /* SCL: 推挽输出 */
    oop_gpio_init_with_mode(&i2c->sda, sda_port, sda_pin, true,
                            OOP_GPIO_MODE_OUTPUT_OD,
                            OOP_GPIO_PULL_UP,
                            OOP_GPIO_SPEED_HIGH);                       /* SDA: 开漏上拉 */
    i2c->delay_us = delay_us;

    oop_i2c_stop(i2c);   /* 释放总线，进入 idle 态 */
    return true;
}

/* ========== 起止信号 ========== */
void oop_i2c_start(soft_i2c_t *i2c) {
    i2c_sda(i2c, true);
    i2c_scl(i2c, true);
    i2c_delay(i2c);
    i2c_sda(i2c, false);
    i2c_delay(i2c);
    i2c_scl(i2c, false);
    i2c_delay(i2c);
}

void oop_i2c_stop(soft_i2c_t *i2c) {
    i2c_sda(i2c, false);
    i2c_delay(i2c);
    i2c_scl(i2c, true);
    i2c_delay(i2c);
    i2c_sda(i2c, true);
    i2c_delay(i2c);
}

/* ========== 应答 ========== */
uint8_t oop_i2c_wait_ack(soft_i2c_t *i2c) {
    uint8_t waittime = 0;

    i2c_sda(i2c, true);
    i2c_delay(i2c);
    i2c_scl(i2c, true);
    i2c_delay(i2c);

    while (i2c_read_sda(i2c)) {
        if (++waittime > 250) {
            oop_i2c_stop(i2c);
            return 1;
        }
    }

    i2c_scl(i2c, false);
    i2c_delay(i2c);
    return 0;
}

void oop_i2c_ack(soft_i2c_t *i2c) {
    i2c_sda(i2c, false);
    i2c_delay(i2c);
    i2c_scl(i2c, true);
    i2c_delay(i2c);
    i2c_scl(i2c, false);
    i2c_delay(i2c);
    i2c_sda(i2c, true);
    i2c_delay(i2c);
}

void oop_i2c_nack(soft_i2c_t *i2c) {
    i2c_sda(i2c, true);
    i2c_delay(i2c);
    i2c_scl(i2c, true);
    i2c_delay(i2c);
    i2c_scl(i2c, false);
    i2c_delay(i2c);
}

/* ========== 数据收发 ========== */
void oop_i2c_send_byte(soft_i2c_t *i2c, uint8_t data) {
    for (uint8_t i = 0; i < 8; i++) {
        i2c_sda(i2c, (data & 0x80) ? true : false);
        data <<= 1;
        i2c_delay(i2c);
        i2c_scl(i2c, true);
        i2c_delay(i2c);
        i2c_scl(i2c, false);
        i2c_delay(i2c);
    }
    i2c_sda(i2c, true);
    i2c_delay(i2c);
}

uint8_t oop_i2c_read_byte(soft_i2c_t *i2c, uint8_t ack) {
    uint8_t receive = 0;

    i2c_sda(i2c, true);   /* 释放 SDA，交由从机驱动 */

    for (uint8_t i = 0; i < 8; i++) {
        receive <<= 1;
        i2c_scl(i2c, true);
        i2c_delay(i2c);
        if (i2c_read_sda(i2c)) {
            receive |= 0x01;
        }
        i2c_scl(i2c, false);
        i2c_delay(i2c);
    }

    if (ack)
        oop_i2c_ack(i2c);
    else
        oop_i2c_nack(i2c);

    return receive;
}
