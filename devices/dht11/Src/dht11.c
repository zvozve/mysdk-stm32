/**
 * @file    dht11.c
 * @brief   DHT11 温湿度传感器驱动（适配 OOP GPIO V3.1）
 * @version V3.1
 * @date    2026-08-25
 */

#include "dht11.h"
#include "oop_gpio_drv.h"
#include "oop_dwt.h"
#include "SEGGER_RTT_Log.h"
#include <string.h>

/* ========== 内部变量 ========== */

static gpio_dev_t s_dev = {0};

/* ========== 内部函数 ========== */

/**
 * @brief   切换为输出模式（开漏）
 */
static inline void set_output(void)
{
    oop_gpio_set_mode(&s_dev,
                      OOP_GPIO_MODE_OUTPUT_OD,
                      OOP_GPIO_PULL_NOPULL,
                      OOP_GPIO_SPEED_HIGH);
}

/**
 * @brief   切换为输入模式（上拉）
 */
static inline void set_input(void)
{
    oop_gpio_set_mode(&s_dev,
                      OOP_GPIO_MODE_INPUT,
                      OOP_GPIO_PULL_UP,
                      OOP_GPIO_SPEED_LOW);
}

/**
 * @brief   读取一个位
 * @retval  0/1 正常，0xFF 超时
 */
static uint8_t read_bit(void)
{
    uint32_t start;
    uint32_t high_us;

    /* 等待低电平开始（DHT11 拉低约 50us） */
    start = oop_GetCycleCount();
    while (OOP_GPIO_READ_RAW(&s_dev) == GPIO_PIN_SET) {
        if (oop_GetElapsedUS(start, oop_GetCycleCount()) > 100) {
            return 0xFF;
        }
    }

    /* 等待高电平开始（DHT11 释放） */
    start = oop_GetCycleCount();
    while (OOP_GPIO_READ_RAW(&s_dev) == GPIO_PIN_RESET) {
        if (oop_GetElapsedUS(start, oop_GetCycleCount()) > 100) {
            return 0xFF;
        }
    }

    /* 测量高电平持续时间 */
    start = oop_GetCycleCount();
    while (OOP_GPIO_READ_RAW(&s_dev) == GPIO_PIN_SET) {
        if (oop_GetElapsedUS(start, oop_GetCycleCount()) > 100) {
            break;
        }
    }

    high_us = oop_GetElapsedUS(start, oop_GetCycleCount());

    /* DHT11: 26~28us → 0，70us → 1 */
    return (high_us > 40) ? 1 : 0;
}

/**
 * @brief   读取 40 位原始数据
 * @param   buffer  5 字节缓冲区
 * @retval  true 成功，false 失败
 */
static bool read_raw(uint8_t *buffer)
{
    uint32_t start;

    /* 1. 发送启动信号（不关中断，20ms 可以接受被打断） */
    set_output();
    OOP_GPIO_WRITE_RAW(&s_dev, GPIO_PIN_RESET);
    oop_DelayUS(20000);                     // 20ms 低电平，开中断状态

    OOP_GPIO_WRITE_RAW(&s_dev, GPIO_PIN_SET);
    oop_DelayUS(30);

    set_input();

    /* 2. 等待 DHT11 响应（这里开始关中断，后面时序要求高） */
    __disable_irq();

    // 等待 DHT11 拉低（约 80us）
    start = oop_GetCycleCount();
    while (OOP_GPIO_READ_RAW(&s_dev) == GPIO_PIN_SET) {
        if (oop_GetElapsedUS(start, oop_GetCycleCount()) > 100) {
            __enable_irq();
            SYS_LOG("DHT11: No response");
            return false;
        }
    }

    // 等待 DHT11 释放总线（拉高约 80us）
    start = oop_GetCycleCount();
    while (OOP_GPIO_READ_RAW(&s_dev) == GPIO_PIN_RESET) {
        if (oop_GetElapsedUS(start, oop_GetCycleCount()) > 100) {
            __enable_irq();
            SYS_LOG("DHT11: Stuck LOW");
            return false;
        }
    }

    /* 3. 读取 40 位数据 */
    memset(buffer, 0, 5);
    for (uint8_t i = 0; i < 40; i++) {
        uint8_t bit = read_bit();
        if (bit == 0xFF) {
            __enable_irq();
            SYS_LOG("DHT11: Bit timeout at %d", i);
            return false;
        }
        buffer[i / 8] = (buffer[i / 8] << 1) | bit;
    }

    __enable_irq();
    return true;
}

/* ========== 对外 API ========== */

void DHT11_Init(GPIO_TypeDef *port, uint16_t pin)
{
    /* 使用新版 OOP GPIO 初始化为开漏输出（端口/引脚由工程注入） */
    if (!oop_gpio_init_with_mode(&s_dev,
                                 port,
                                 pin,
                                 true,                          /* active_high */
                                 OOP_GPIO_MODE_OUTPUT_OD,
                                 OOP_GPIO_PULL_NOPULL,
                                 OOP_GPIO_SPEED_HIGH)) {
        SYS_LOG("DHT11: Init FAILED");
        return;
    }

    /* 总线默认释放 */
    OOP_GPIO_WRITE_RAW(&s_dev, GPIO_PIN_SET);
    SYS_LOG("DHT11: Init OK");
}

bool DHT11_Read(DHT11_Data_t *pData)
{
    uint8_t buf[5] = {0};

    if (pData == NULL) {
        return false;
    }

    pData->is_valid = false;

    // 不再在这里关中断
    bool ok = read_raw(buf);

    if (!ok) {
        return false;
    }

    // 校验和...
    uint8_t sum = (uint8_t)(buf[0] + buf[1] + buf[2] + buf[3]);
    if (buf[4] != sum) {
        SYS_LOG("DHT11: Checksum fail (calc=%u, recv=%u)", sum, buf[4]);
        return false;
    }

    pData->hum_int  = buf[0];
    pData->hum_dec  = buf[1];
    pData->temp_int = buf[2];
    pData->temp_dec = buf[3];
    pData->checksum = buf[4];
    pData->is_valid = true;

    return true;
}