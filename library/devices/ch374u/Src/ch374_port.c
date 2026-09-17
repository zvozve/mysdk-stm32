/* ============================================================
 * ch374_port.c - CH374 板级适配层实现（devices 层，禁直调 HAL）
 *
 * 全部硬件访问下沉到 chip 层：SPI 走 oop_spi（多实例，hspi2 与 hspi3 可并存）、
 * CS/INT/USB 电源走 oop_gpio。SPI 句柄与引脚由 CH374_Bind() 注入，
 * 本文件不含任何 MX 名（hspi2 / GPIOA / GPIO_PIN_x …）。
 * ============================================================ */

#include "ch374_port.h"
#include "oop_spi.h"          /* SPI 传输（实例化） */
#include "oop_gpio_drv.h"     /* CS / INT / USB 电源引脚 */

/* 模块级上下文：SPI 与引脚均由 CH374_Bind 注入（板级绑定） */
static oop_spi_dev_t s_spi;
static gpio_dev_t    s_cs;    /* 片选（低有效） */
static gpio_dev_t    s_int;   /* 中断输入（上拉） */
static gpio_dev_t    s_pwr;   /* USB 供电控制 */
static uint8_t       s_bound = 0;

/* HID 数据回调（由任务层注入） */
static const ch374_event_cb_t *s_event_cb = NULL;

void CH374_Bind(SPI_HandleTypeDef *hspi,
                GPIO_TypeDef *cs_port,  uint16_t cs_pin,
                GPIO_TypeDef *int_port, uint16_t int_pin,
                GPIO_TypeDef *pwr_port, uint16_t pwr_pin)
{
    oop_spi_dev_init(&s_spi, hspi);
    s_spi.timeout = 100;   /* 与原 HAL_SPI_TransmitReceive(..., 100) 一致 */

    /* 片选 / 电源：推挽输出 + 上拉 + 中速（与原 GPIO_Toggle_INIT 一致） */
    oop_gpio_init_with_mode(&s_cs,  cs_port,  cs_pin,  true,
                            OOP_GPIO_MODE_OUTPUT_PP, OOP_GPIO_PULL_UP, OOP_GPIO_SPEED_MEDIUM);
    oop_gpio_init_with_mode(&s_pwr, pwr_port, pwr_pin, true,
                            OOP_GPIO_MODE_OUTPUT_PP, OOP_GPIO_PULL_UP, OOP_GPIO_SPEED_MEDIUM);
    /* 中断输入：上拉（与原 GPIO_Toggle_INIT 一致） */
    oop_gpio_init_input(&s_int, int_port, int_pin, true);

    oop_gpio_set_high(&s_cs);    /* 片选默认无效 */
    oop_gpio_set_high(&s_pwr);   /* USB 供电使能 */
    s_bound = 1;
}

/* ---------- 片选 / 中断 / 电源 ---------- */

void CH374_CS_High(void)
{
    if (s_bound) oop_gpio_set_high(&s_cs);
}

void CH374_CS_Low(void)
{
    if (s_bound) oop_gpio_set_low(&s_cs);
}

uint8_t CH374_INT_Level(void)
{
    if (!s_bound) return 1;
    return oop_gpio_read(&s_int) ? 1 : 0;
}

void CH374_UsbPower(uint8_t on)
{
    if (!s_bound) return;
    oop_gpio_write(&s_pwr, on ? true : false);
}

/* ---------- SPI 单字节收发 ---------- */

uint8_t CH374_SPI_ReadWriteByte(uint8_t byte)
{
    uint8_t rx = 0xFF;
    if (!s_bound) return rx;
    if (oop_spi_transmit_receive(&s_spi, &byte, &rx, 1) != HAL_OK) {
        rx = 0xFF;
    }
    return rx;
}

/* ---------- HID 事件回调注册 ---------- */

const ch374_event_cb_t *ch374_register_event_cb(const ch374_event_cb_t *cb)
{
    const ch374_event_cb_t *prev = s_event_cb;
    s_event_cb = cb;
    return prev;
}

const ch374_event_cb_t *ch374_get_event_cb(void)
{
    return s_event_cb;
}
