/**
 * @file    oop_gpio_drv.h
 * @brief   OOP GPIO 抽象层 + 中断回调注册 + 引脚组（bank）批量读写
 * @version V3.2
 * @date    2026-09-17
 *
 * @note    V3.2 新增 gpio_bank_t：把一组同向引脚当整体做扫描/输出（位图读写），
 *          引脚表由工程 board_cfg 注入。适用于 16 路输入点采集、16 路继电器
 *          位图输出等批处理场景，RAM 占用按 8 字节/路计，远小于逐路 gpio_dev_t。
 */

#ifndef __OOP_GPIO_DRV_H
#define __OOP_GPIO_DRV_H

#include <stdint.h>
#include <stdbool.h>
#include "hal_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========== GPIO 模式定义 ========== */

#define OOP_GPIO_MODE_OUTPUT_PP     0x01
#define OOP_GPIO_MODE_OUTPUT_OD     0x02
#define OOP_GPIO_MODE_INPUT         0x03
#define OOP_GPIO_MODE_AF_PP         0x04
#define OOP_GPIO_MODE_AF_OD         0x05
#define OOP_GPIO_MODE_ANALOG        0x06

#define OOP_GPIO_PULL_NOPULL        0x00
#define OOP_GPIO_PULL_UP            0x01
#define OOP_GPIO_PULL_DOWN          0x02

#define OOP_GPIO_SPEED_LOW          0x00
#define OOP_GPIO_SPEED_MEDIUM       0x01
#define OOP_GPIO_SPEED_HIGH         0x02
#define OOP_GPIO_SPEED_VERY_HIGH    0x03

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

#define OOP_GPIO_EDGE_RISING    (1u << 0)
#define OOP_GPIO_EDGE_FALLING   (1u << 1)
#define OOP_GPIO_EDGE_BOTH      (OOP_GPIO_EDGE_RISING | OOP_GPIO_EDGE_FALLING)
#define OOP_GPIO_IRQ_LEVEL_ANY  0xFF

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
bool oop_gpio_init_input(gpio_dev_t *dev, GPIO_TypeDef *port, uint16_t pin, bool active_high);
bool oop_gpio_init_output(gpio_dev_t *dev, GPIO_TypeDef *port, uint16_t pin, bool active_high);
bool oop_gpio_init_with_mode(gpio_dev_t *dev, GPIO_TypeDef *port, uint16_t pin,
                              bool active_high, uint8_t mode, uint8_t pull, uint8_t speed);
bool oop_gpio_init_af(gpio_dev_t *dev, GPIO_TypeDef *port, uint16_t pin,
                      bool active_high, uint8_t mode, uint8_t pull, uint8_t speed, uint8_t alternate);
bool oop_gpio_is_initialized(const gpio_dev_t *dev);

/* 标准操作 */
void oop_gpio_set_high(gpio_dev_t *dev);
void oop_gpio_set_low(gpio_dev_t *dev);
void oop_gpio_toggle(gpio_dev_t *dev);
void oop_gpio_write(gpio_dev_t *dev, bool state);
bool oop_gpio_read(const gpio_dev_t *dev);

/* 模式切换（支持 AF） */
void oop_gpio_set_mode(gpio_dev_t *dev, uint8_t mode, uint8_t pull, uint8_t speed);
void oop_gpio_set_mode_af(gpio_dev_t *dev, uint8_t mode, uint8_t pull, uint8_t speed, uint8_t alternate);

/* 原始操作宏（高频场景，忽略 active_high，直接操作硬件电平） */
#define OOP_GPIO_READ_RAW(dev)      HAL_GPIO_ReadPin((dev)->pin.port, (dev)->pin.pin)
#define OOP_GPIO_WRITE_RAW(dev, s)  HAL_GPIO_WritePin((dev)->pin.port, (dev)->pin.pin, (s))

/* ========== GPIO 引脚组（bank）：表驱动批量 / 位图读写 ========== */

/**
 * @note  用途：把「一组同向引脚」当作整体做扫描与输出（如 16 路输入点采集成
 *        一个位图、16 路继电器按位图一次性写出）。引脚表由工程 board_cfg 注入，
 *        本模块不持有任何板级信息。相比逐路 gpio_dev_t，bank 只存引脚描述
 *        （8 字节/路），RAM 占用显著更小。
 */

#define OOP_GPIO_BANK_MAX   32   /* 单组最大路数（位图为 uint32_t） */

/** 一路引脚的物理描述（引脚表元素） */
typedef struct {
    GPIO_TypeDef *port;
    uint16_t      pin;
} gpio_pin_desc_t;

/** 引脚组实例（由调用方持有） */
typedef struct {
    gpio_pin_desc_t pins[OOP_GPIO_BANK_MAX];
    uint16_t        count;        /* 实际路数，<= OOP_GPIO_BANK_MAX */
    bool            active_high;  /* 逻辑极性，组内所有路一致 */
    bool            initialized;
} gpio_bank_t;

/**
 * @brief  按引脚表初始化一组 IO（表驱动）
 * @param  bank        引脚组实例
 * @param  desc        引脚描述表（如工程 board_cfg.h 的 BOARD_DIN_MAP）
 * @param  count       路数（超过 OOP_GPIO_BANK_MAX 按上限截断）
 * @param  active_high 逻辑极性：true=高有效
 * @param  mode/pull/speed 逐引脚初始化参数（见 OOP_GPIO_MODE_* / PULL_* / SPEED_*）
 * @retval true 成功
 * @note   desc 只需在调用期间有效（内容拷贝进 bank）。
 */
bool oop_gpio_bank_init(gpio_bank_t *bank, const gpio_pin_desc_t *desc, uint16_t count,
                        bool active_high, uint8_t mode, uint8_t pull, uint8_t speed);

/**
 * @brief  一次读回整组逻辑电平
 * @return 位图，bit i = 第 i 路逻辑电平；未初始化时返回 0
 */
uint32_t oop_gpio_bank_read_bits(const gpio_bank_t *bank);

/**
 * @brief  按位图写整组
 * @param  bits 位图，bit i=1 → 第 i 路置为有效电平
 */
void oop_gpio_bank_write_bits(const gpio_bank_t *bank, uint32_t bits);

/** 单路读（索引越界或未初始化返回 false） */
bool oop_gpio_bank_read(const gpio_bank_t *bank, uint16_t idx);

/** 单路写（索引越界或未初始化为空操作） */
void oop_gpio_bank_write(const gpio_bank_t *bank, uint16_t idx, bool state);

/** 组内路数 / 是否已初始化 */
uint16_t oop_gpio_bank_count(const gpio_bank_t *bank);
bool     oop_gpio_bank_is_initialized(const gpio_bank_t *bank);

/* 中断回调 */
bool oop_gpio_irq_register(GPIO_TypeDef *port, uint16_t pin,
                           gpio_irq_callback_t callback, void *user_data,
                           uint8_t edge, uint32_t debounce_us, uint8_t active_level);
void oop_gpio_irq_unregister(GPIO_TypeDef *port, uint16_t pin);
void oop_gpio_irq_enable(GPIO_TypeDef *port, uint16_t pin, bool enable);
void oop_gpio_irq_dispatch(uint16_t GPIO_Pin);

#ifdef __cplusplus
}
#endif

#endif /* __OOP_GPIO_DRV_H */