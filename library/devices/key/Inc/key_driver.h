#ifndef __KEY_DRIVER_H
#define __KEY_DRIVER_H

#include "oop_gpio_drv.h"

/*
 * 通用按键驱动（板无关）：轮询式扫描 + 防抖 + 短按/长按回调。
 * 每个按键由应用通过 key_t 表注入（gpio_dev_t 来自 board_cfg，回调由应用注册），
 * 驱动本身不引用任何具体 IO / 应用全局。
 */

/* 每次 Key_Scan() 调用间隔(us)，决定防抖/长按计数的时间基准，board_cfg 可覆盖 */
#ifndef KEY_SCAN_INTERVAL_US
#define KEY_SCAN_INTERVAL_US   1000
#endif

#define KEY_DEBOUNCE_MS        10
#define KEY_MS_TO_COUNT(ms)    ((ms) * 1000 / KEY_SCAN_INTERVAL_US)
#define KEY_DEBOUNCE_COUNT     KEY_MS_TO_COUNT(KEY_DEBOUNCE_MS)

typedef struct {
    gpio_dev_t gpio;                   /* 由 board_cfg 注入的引脚（oop_gpio） */

    uint16_t debounce_counter;
    uint8_t  stable_state;
    uint8_t  last_state;
    uint8_t  triggered;
    uint8_t  long_triggered;
    uint32_t press_time;

    uint32_t long_press_start_count;   /* 长按首次触发（单位：scan count），应用按表设置 */
    uint32_t long_press_repeat_count;  /* 长按重复触发间隔（单位：scan count） */

    void (*short_press_callback)(void);
    void (*long_press_callback)(void);
} key_t;

/*
 * 注册按键表并复位状态。table 由应用/board_cfg 提供（含 gpio_dev_t 与回调），
 * count 为按键数。之后由定时器周期性调用 Key_Scan() 即可。
 */
void Key_Init(key_t *table, uint8_t count);

/* 周期性扫描（建议由 1ms 级定时器调用），驱动内部持有注册的按键表 */
void Key_Scan(void);

#endif
