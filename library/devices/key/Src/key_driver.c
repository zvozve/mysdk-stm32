#include "key_driver.h"

/* 注册的按键表（由 Key_Init 注入），驱动内部持有 */
static key_t *s_keys = NULL;
static uint8_t s_key_count = 0;

void Key_Init(key_t *table, uint8_t count)
{
    s_keys = table;
    s_key_count = count;

    for (uint8_t i = 0; i < count; i++) {
        s_keys[i].debounce_counter = 0;
        s_keys[i].stable_state = 0;
        s_keys[i].last_state = 1;
        s_keys[i].triggered = 0;
        s_keys[i].long_triggered = 0;
        s_keys[i].press_time = 0;
    }
}

void Key_Scan(void)
{
    if (!s_keys) return;

    for (uint8_t i = 0; i < s_key_count; i++) {
        key_t *k = &s_keys[i];

        /* 低电平为按下（与硬件上拉一致；若硬件相反，应用可在建表时翻转逻辑） */
        uint8_t pin_state = OOP_GPIO_READ_RAW(&k->gpio);

        if (pin_state == GPIO_PIN_RESET) {            /* 被按下 */
            if (k->debounce_counter < KEY_DEBOUNCE_COUNT) {
                k->debounce_counter++;
            } else {
                if (!k->stable_state) {
                    k->stable_state = 1;
                    k->press_time = 0;
                }

                k->press_time++;

                /* 首次长按触发 */
                if (k->press_time == k->long_press_start_count) {
                    if (k->long_press_callback) {
                        k->long_press_callback();
                        k->long_triggered = 1;
                        k->triggered = 1;            /* 标记已触发长按，避免释放时误触短按 */
                    }
                }

                /* 长按持续触发 */
                if (k->long_triggered &&
                    ((k->press_time - k->long_press_start_count) % k->long_press_repeat_count == 0)) {
                    if (k->long_press_callback) {
                        k->long_press_callback();
                    }
                }
            }
        } else {                                     /* 松开 */
            if (k->stable_state) {
                /* 仅在未触发过长按时，释放瞬间判为短按 */
                if (!k->long_triggered && k->short_press_callback) {
                    k->short_press_callback();
                }
            }

            k->debounce_counter = 0;
            k->press_time = 0;
            k->stable_state = 0;
            k->triggered = 0;
            k->long_triggered = 0;
        }
    }
}
