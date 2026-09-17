/* ============================================================
 * ch9434_port.c - CH9434 板级适配层实现（devices 层，禁直调 HAL）
 *
 * 全部硬件访问下沉到 chip 层：SPI 走 oop_spi（实例化）、CS/INT 走 oop_gpio、
 * 延时走 oop_dwt。SPI 句柄与引脚由 CH9434_Bind() 注入，本文件不含任何
 * MX 名（hspi3 / GPIOA / GPIO_PIN_15 …）。
 * ============================================================ */

#include "ch9434_port.h"
#include "oop_spi.h"          /* SPI 传输（多实例，hspi2/hspi3 可并存） */
#include "oop_gpio_drv.h"     /* CS / INT 引脚 */
#include "oop_dwt.h"          /* oop_DelayUS / oop_DelayMS */

/* 模块级上下文：SPI 与引脚均由 CH9434_Bind 注入（板级绑定） */
static oop_spi_dev_t s_spi;
static gpio_dev_t    s_cs;    /* CS 片选（低有效，直通电平） */
static gpio_dev_t    s_int;   /* INT 中断输入（上拉） */
static uint8_t       s_bound = 0;

/* 每路串口当前配置（保存实际生效值，供查询与日志） */
static ch9434_uart_cfg_t s_uart_cfg[4];

void CH9434_Bind(SPI_HandleTypeDef *hspi,
                 GPIO_TypeDef *cs_port,  uint16_t cs_pin,
                 GPIO_TypeDef *int_port, uint16_t int_pin)
{
    oop_spi_dev_init(&s_spi, hspi);
    /* active_high = true：set_high 即输出高电平，与原 HAL_GPIO_WritePin 直通语义一致 */
    oop_gpio_init_output(&s_cs,  cs_port,  cs_pin,  true);
    oop_gpio_init_input (&s_int, int_port, int_pin, true);
    oop_gpio_set_high(&s_cs);   /* 片选默认不选中 */
    s_bound = 1;
}

/* ---------- ch9434.h 要求的 3 个用户层接口（本层实现，全走 oop_*） ---------- */

void CH9434_US_DELAY(void)
{
    oop_DelayUS(1);
}

void CH9434_SPI_SCS_OP(uint8_t dat)
{
    if (!s_bound) return;
    if (dat) oop_gpio_set_high(&s_cs);
    else     oop_gpio_set_low(&s_cs);
}

uint8_t CH9434_SPI_WRITE_BYTE(uint8_t dat)
{
    uint8_t rx = 0xFF;
    if (!s_bound) return rx;
    if (oop_spi_transmit_receive(&s_spi, &dat, &rx, 1) != HAL_OK) rx = 0xFF;
    return rx;
}

/* ---------- 对上层（task_euart）暴露的接口 ---------- */

uint8_t CH9434_Send(uint8_t uart_idx, uint8_t *send_buf, uint8_t size)
{
    return CH9434UARTxSetTxFIFOData(uart_idx, send_buf, size);
}

void ch9434_uart_set_params(uint8_t uart_idx, const ch9434_uart_cfg_t *cfg)
{
    if (uart_idx >= 4 || !cfg) return;

    s_uart_cfg[uart_idx] = *cfg;
    CH9434UARTxParaSet(uart_idx,
                       cfg->baudrate,
                       cfg->data_bits,
                       cfg->stop_bits,
                       cfg->parity);
    CH9434UARTxFIFOSet(uart_idx, CH9434_ENABLE, CH9434_UART_FIFO_MODE_1280);
    CH9434UARTxFlowSet(uart_idx, CH9434_DISABLE);
    CH9434UARTxIrqSet(uart_idx, CH9434_DISABLE, CH9434_ENABLE, CH9434_ENABLE, CH9434_ENABLE);
    CH9434UARTxIrqOpen(uart_idx);
    CH9434UARTxRtsDtrPin(uart_idx, CH9434_ENABLE, CH9434_ENABLE);
}

void ch9434_uart_get_params(uint8_t uart_idx, ch9434_uart_cfg_t *cfg)
{
    if (uart_idx >= 4 || !cfg) return;
    *cfg = s_uart_cfg[uart_idx];
}

void CH9434_Init(void)
{
    uint8_t i;
    ch9434_uart_cfg_t default_cfg;

    if (!s_bound) return;

    oop_gpio_set_high(&s_cs);   /* 片选默认高（不选中） */
    oop_DelayMS(50);

    CH9434InitClkMode(CH9434_ENABLE,   /* 外部晶振 */
                      CH9434_ENABLE,   /* 开启倍频功能 */
                      13);             /* 分频系数 */
    oop_DelayMS(50);

    /* 默认参数：115200-8N1，各路可独立修改 */
    default_cfg.baudrate  = 115200;
    default_cfg.data_bits = CH9434_UART_8_BITS_PER_CHAR;
    default_cfg.stop_bits = CH9434_UART_ONE_STOP_BIT;
    default_cfg.parity    = CH9434_UART_NO_PARITY;

    for (i = 0; i < 4; i++) ch9434_uart_set_params(i, &default_cfg);
}
