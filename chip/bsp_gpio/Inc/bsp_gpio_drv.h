/**
 * @file    bsp_gpio_drv.h
 * @brief   BSP GPIO 抽象层 + 中断回调注册
 * @version V3.1
 * @date    2026-08-25
 */

#ifndef __BSP_GPIO_DRV_H
#define __BSP_GPIO_DRV_H

#include <stdint.h>
#include <stdbool.h>
#include "hal_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========== GPIO 模式定义 ========== */

#define BSP_GPIO_MODE_OUTPUT_PP     0x01
#define BSP_GPIO_MODE_OUTPUT_OD     0x02
#define BSP_GPIO_MODE_INPUT         0x03
#define BSP_GPIO_MODE_AF_PP         0x04
#define BSP_GPIO_MODE_AF_OD         0x05
#define BSP_GPIO_MODE_ANALOG        0x06

#define BSP_GPIO_PULL_NOPULL        0x00
#define BSP_GPIO_PULL_UP            0x01
#define BSP_GPIO_PULL_DOWN          0x02

#define BSP_GPIO_SPEED_LOW          0x00
#define BSP_GPIO_SPEED_MEDIUM       0x01
#define BSP_GPIO_SPEED_HIGH         0x02
#define BSP_GPIO_SPEED_VERY_HIGH    0x03

/* ========== GPIO 设备结构 ========== */

typedef struct {
    GPIO_TypeDef *port;
    uint16_t      pin;
    bool          active_high;
    uint8_t       mode;
    uint8_t       pull;
    uint8_t       speed;
    uint8_t       alternate;   /* AF 模式专用，默认 0 */
} gpio_pin_t;

typedef struct {
    void (*write)(gpio_pin_t *pin, bool state);
    void (*toggle)(gpio_pin_t *pin);
    bool (*read)(gpio_pin_t *pin);
    void (*set_high)(gpio_pin_t *pin);
    void (*set_low)(gpio_pin_t *pin);
    void (*set_mode)(gpio_pin_t *pin, uint8_t mode, uint8_t pull, uint8_t speed, uint8_t alternate);
} gpio_ops_t;

typedef struct {
    gpio_pin_t  pin;
    gpio_ops_t  ops;
    bool        is_initialized;
} gpio_dev_t;

/* ========== 中断回调 ========== */

typedef void (*gpio_irq_callback_t)(uint16_t pin, void *user_data);

#define BSP_GPIO_EDGE_RISING    (1u << 0)
#define BSP_GPIO_EDGE_FALLING   (1u << 1)
#define BSP_GPIO_EDGE_BOTH      (BSP_GPIO_EDGE_RISING | BSP_GPIO_EDGE_FALLING)
#define BSP_GPIO_IRQ_LEVEL_ANY  0xFF

typedef struct {
    GPIO_TypeDef           *port;
    uint16_t                pin;
    gpio_irq_callback_t     callback;
    void                   *user_data;
    bool                    registered;   /* 是否已注册 */
    bool                    enabled;      /* 是否使能 */
    uint8_t                 edge;
    uint32_t                debounce_us;
    uint8_t                 active_level;
    uint32_t                last_active;
    uint8_t                 last_level;
} gpio_irq_reg_t;

/* ========== API 函数 ========== */

/* 初始化 */
bool bsp_gpio_init_input(gpio_dev_t *dev, GPIO_TypeDef *port, uint16_t pin, bool active_high);
bool bsp_gpio_init_output(gpio_dev_t *dev, GPIO_TypeDef *port, uint16_t pin, bool active_high);
bool bsp_gpio_init_with_mode(gpio_dev_t *dev, GPIO_TypeDef *port, uint16_t pin,
                              bool active_high, uint8_t mode, uint8_t pull, uint8_t speed);
bool bsp_gpio_init_af(gpio_dev_t *dev, GPIO_TypeDef *port, uint16_t pin,
                      bool active_high, uint8_t mode, uint8_t pull, uint8_t speed, uint8_t alternate);
bool bsp_gpio_is_initialized(const gpio_dev_t *dev);

/* 标准操作 */
void bsp_gpio_set_high(gpio_dev_t *dev);
void bsp_gpio_set_low(gpio_dev_t *dev);
void bsp_gpio_toggle(gpio_dev_t *dev);
void bsp_gpio_write(gpio_dev_t *dev, bool state);
bool bsp_gpio_read(const gpio_dev_t *dev);

/* 模式切换（支持 AF） */
void bsp_gpio_set_mode(gpio_dev_t *dev, uint8_t mode, uint8_t pull, uint8_t speed);
void bsp_gpio_set_mode_af(gpio_dev_t *dev, uint8_t mode, uint8_t pull, uint8_t speed, uint8_t alternate);

/* 原始操作宏（高频场景，忽略 active_high，直接操作硬件电平） */
#define BSP_GPIO_READ_RAW(dev)      HAL_GPIO_ReadPin((dev)->pin.port, (dev)->pin.pin)
#define BSP_GPIO_WRITE_RAW(dev, s)  HAL_GPIO_WritePin((dev)->pin.port, (dev)->pin.pin, (s))

/* 中断回调 */
bool bsp_gpio_irq_register(GPIO_TypeDef *port, uint16_t pin,
                           gpio_irq_callback_t callback, void *user_data,
                           uint8_t edge, uint32_t debounce_us, uint8_t active_level);
void bsp_gpio_irq_unregister(GPIO_TypeDef *port, uint16_t pin);
void bsp_gpio_irq_enable(GPIO_TypeDef *port, uint16_t pin, bool enable);
void bsp_gpio_irq_dispatch(uint16_t GPIO_Pin);

#ifdef __cplusplus
}
#endif

#endif /* __BSP_GPIO_DRV_H */