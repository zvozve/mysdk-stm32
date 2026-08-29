/**
 * @file    oop_gpio_drv.c
 * @brief   OOP GPIO 抽象层 + 中断回调注册实现
 * @version V3.1
 * @date    2026-08-25
 */

#include "oop_gpio_drv.h"
#include "oop_dwt.h"
#include <string.h>

/* ========== 默认操作函数 ========== */

static void default_write(gpio_pin_t *pin, bool state)
{
    if (pin == NULL || pin->port == NULL) return;
    GPIO_PinState hal_state = pin->active_high ?
        (state ? GPIO_PIN_SET : GPIO_PIN_RESET) :
        (state ? GPIO_PIN_RESET : GPIO_PIN_SET);
    HAL_GPIO_WritePin(pin->port, pin->pin, hal_state);
}

static void default_toggle(gpio_pin_t *pin)
{
    if (pin == NULL || pin->port == NULL) return;
    HAL_GPIO_TogglePin(pin->port, pin->pin);
}

static bool default_read(gpio_pin_t *pin)
{
    if (pin == NULL || pin->port == NULL) return false;
    GPIO_PinState state = HAL_GPIO_ReadPin(pin->port, pin->pin);
    return pin->active_high ? (state == GPIO_PIN_SET) : (state == GPIO_PIN_RESET);
}

static void default_set_high(gpio_pin_t *pin) { default_write(pin, true); }
static void default_set_low(gpio_pin_t *pin)  { default_write(pin, false); }

static void default_set_mode(gpio_pin_t *pin, uint8_t mode, uint8_t pull, uint8_t speed, uint8_t alternate)
{
    if (pin == NULL || pin->port == NULL) return;

    GPIO_InitTypeDef init = {0};
    init.Pin       = pin->pin;
    init.Speed     = speed;
    init.Pull      = pull;
    init.Alternate = alternate;

    switch (mode) {
        case OOP_GPIO_MODE_OUTPUT_PP:  init.Mode = GPIO_MODE_OUTPUT_PP; break;
        case OOP_GPIO_MODE_OUTPUT_OD:  init.Mode = GPIO_MODE_OUTPUT_OD; break;
        case OOP_GPIO_MODE_INPUT:      init.Mode = GPIO_MODE_INPUT;     break;
        case OOP_GPIO_MODE_AF_PP:      init.Mode = GPIO_MODE_AF_PP;     break;
        case OOP_GPIO_MODE_AF_OD:      init.Mode = GPIO_MODE_AF_OD;     break;
        case OOP_GPIO_MODE_ANALOG:     init.Mode = GPIO_MODE_ANALOG;    break;
        default:                       init.Mode = GPIO_MODE_INPUT;     break;
    }

    HAL_GPIO_Init(pin->port, &init);

    pin->mode      = mode;
    pin->pull      = pull;
    pin->speed     = speed;
    pin->alternate = alternate;
}

static const gpio_ops_t g_default_ops = {
    .write    = default_write,
    .toggle   = default_toggle,
    .read     = default_read,
    .set_high = default_set_high,
    .set_low  = default_set_low,
    .set_mode = default_set_mode,
};

/* ========== GPIO 设备 API ========== */

bool oop_gpio_init_input(gpio_dev_t *dev, GPIO_TypeDef *port, uint16_t pin, bool active_high)
{
    return oop_gpio_init_with_mode(dev, port, pin, active_high,
                                   OOP_GPIO_MODE_INPUT,
                                   OOP_GPIO_PULL_UP,
                                   OOP_GPIO_SPEED_LOW);
}

bool oop_gpio_init_output(gpio_dev_t *dev, GPIO_TypeDef *port, uint16_t pin, bool active_high)
{
    return oop_gpio_init_with_mode(dev, port, pin, active_high,
                                   OOP_GPIO_MODE_OUTPUT_PP,
                                   OOP_GPIO_PULL_NOPULL,
                                   OOP_GPIO_SPEED_LOW);
}

bool oop_gpio_init_with_mode(gpio_dev_t *dev, GPIO_TypeDef *port, uint16_t pin,
                              bool active_high, uint8_t mode, uint8_t pull, uint8_t speed)
{
    return oop_gpio_init_af(dev, port, pin, active_high, mode, pull, speed, 0);
}

bool oop_gpio_init_af(gpio_dev_t *dev, GPIO_TypeDef *port, uint16_t pin,
                      bool active_high, uint8_t mode, uint8_t pull, uint8_t speed, uint8_t alternate)
{
    if (dev == NULL || port == NULL) return false;

    memset(dev, 0, sizeof(*dev));

    dev->pin.port        = port;
    dev->pin.pin         = pin;
    dev->pin.active_high = active_high;
    dev->pin.mode        = mode;
    dev->pin.pull        = pull;
    dev->pin.speed       = speed;
    dev->pin.alternate   = alternate;

    if (dev->ops.write == NULL) {
        dev->ops = g_default_ops;
    }

    dev->ops.set_mode(&dev->pin, mode, pull, speed, alternate);
    dev->is_initialized = true;

    return true;
}

bool oop_gpio_is_initialized(const gpio_dev_t *dev)
{
    return (dev != NULL && dev->is_initialized);
}

void oop_gpio_set_high(gpio_dev_t *dev)
{
    if (dev == NULL || !dev->is_initialized) return;
    dev->ops.set_high(&dev->pin);
}

void oop_gpio_set_low(gpio_dev_t *dev)
{
    if (dev == NULL || !dev->is_initialized) return;
    dev->ops.set_low(&dev->pin);
}

void oop_gpio_toggle(gpio_dev_t *dev)
{
    if (dev == NULL || !dev->is_initialized) return;
    dev->ops.toggle(&dev->pin);
}

void oop_gpio_write(gpio_dev_t *dev, bool state)
{
    if (dev == NULL || !dev->is_initialized) return;
    dev->ops.write(&dev->pin, state);
}

bool oop_gpio_read(const gpio_dev_t *dev)
{
    if (dev == NULL || !dev->is_initialized) return false;
    return dev->ops.read((gpio_pin_t *)&dev->pin);  /* const 兼容 */
}

void oop_gpio_set_mode(gpio_dev_t *dev, uint8_t mode, uint8_t pull, uint8_t speed)
{
    oop_gpio_set_mode_af(dev, mode, pull, speed, 0);
}

void oop_gpio_set_mode_af(gpio_dev_t *dev, uint8_t mode, uint8_t pull, uint8_t speed, uint8_t alternate)
{
    if (dev == NULL || !dev->is_initialized) return;
    dev->ops.set_mode(&dev->pin, mode, pull, speed, alternate);
}

/* ========== 中断回调注册表 ========== */

#define MAX_IRQ_REGISTERS  16
static gpio_irq_reg_t g_irq_regs[MAX_IRQ_REGISTERS];

static int irq_find_slot(GPIO_TypeDef *port, uint16_t pin)
{
    for (int i = 0; i < MAX_IRQ_REGISTERS; i++) {
        if (g_irq_regs[i].registered &&
            g_irq_regs[i].port == port &&
            g_irq_regs[i].pin  == pin) {
            return i;
        }
    }
    return -1;
}

static int irq_find_free_slot(void)
{
    for (int i = 0; i < MAX_IRQ_REGISTERS; i++) {
        if (!g_irq_regs[i].registered) return i;
    }
    return -1;
}

bool oop_gpio_irq_register(GPIO_TypeDef *port, uint16_t pin,
                           gpio_irq_callback_t callback, void *user_data,
                           uint8_t edge, uint32_t debounce_us, uint8_t active_level)
{
    if (callback == NULL || port == NULL) return false;

    int slot = irq_find_slot(port, pin);
    if (slot < 0) {
        slot = irq_find_free_slot();
        if (slot < 0) return false;
    }

    gpio_irq_reg_t *reg = &g_irq_regs[slot];

    reg->port         = port;
    reg->pin          = pin;
    reg->callback     = callback;
    reg->user_data    = user_data;
    reg->registered   = true;
    reg->enabled      = true;
    reg->edge         = edge;
    reg->debounce_us  = debounce_us;
    reg->active_level = active_level;
    reg->last_active  = oop_GetCycleCount();
    reg->last_level   = (uint8_t)HAL_GPIO_ReadPin(port, pin);

    return true;
}

void oop_gpio_irq_unregister(GPIO_TypeDef *port, uint16_t pin)
{
    int slot = irq_find_slot(port, pin);
    if (slot < 0) return;

    /* 只清除必要字段，保留结构体清零开销最小 */
    g_irq_regs[slot].registered = false;
    g_irq_regs[slot].enabled    = false;
    g_irq_regs[slot].callback   = NULL;
    g_irq_regs[slot].user_data  = NULL;
}

void oop_gpio_irq_enable(GPIO_TypeDef *port, uint16_t pin, bool enable)
{
    int slot = irq_find_slot(port, pin);
    if (slot < 0) return;
    g_irq_regs[slot].enabled = enable;
}

void oop_gpio_irq_dispatch(uint16_t GPIO_Pin)
{
    for (int i = 0; i < MAX_IRQ_REGISTERS; i++) {
        gpio_irq_reg_t *reg = &g_irq_regs[i];

        if (!reg->registered || !reg->enabled || reg->callback == NULL) continue;
        if (reg->pin != GPIO_Pin || reg->port == NULL) continue;

        GPIO_PinState now  = HAL_GPIO_ReadPin(reg->port, reg->pin);
        GPIO_PinState last = (GPIO_PinState)reg->last_level;
        
        /* 边沿检测 */
        uint8_t detected = 0;
        if (last == GPIO_PIN_RESET && now == GPIO_PIN_SET) {
            detected = OOP_GPIO_EDGE_RISING;
        } else if (last == GPIO_PIN_SET && now == GPIO_PIN_RESET) {
            detected = OOP_GPIO_EDGE_FALLING;
        } else {
            /* 电平未变化，更新 last_level 并跳过 */
            reg->last_level = (uint8_t)now;
            continue;
        }

        /* 更新最后电平状态 */
        reg->last_level = (uint8_t)now;

        if ((reg->edge & detected) == 0) continue;

        /* 防抖检查 */
        if (reg->debounce_us > 0) {
            uint32_t now_cycle = oop_GetCycleCount();
            if (oop_GetElapsedUS(reg->last_active, now_cycle) < reg->debounce_us) {
                continue;
            }
            reg->last_active = now_cycle;
        }

        reg->callback(GPIO_Pin, reg->user_data);
    }
}