/**
 * @file    ir_tx.c
 * @brief   红外发射驱动实现（TIM9 38kHz 载波 + DWT 门控）
 * @version V1.0
 * @date    2026-08-27
 */

#include "ir_tx.h"
#include "oop_dwt.h"
#include "oop_gpio_drv.h"
#include "oop_tim_drv.h"
#include "SEGGER_RTT_Log.h"

/* ========== 内部状态 ========== */
static ir_tx_cfg_t          s_cfg;
static TIM_HandleTypeDef    s_htim_tx;
static gpio_dev_t           s_tx_gpio = {0};
static volatile bool        s_busy    = false;
static bool                 s_inited  = false;

/* ========== 载波门控 ========== */
static inline void ir_tx_carrier(bool on)
{
    if (on) {
        oop_tim_pwm_start(&s_htim_tx, s_cfg.tim_channel);
    } else {
        oop_tim_pwm_stop(&s_htim_tx, s_cfg.tim_channel);
    }
}

/* ========== API ========== */

bool IR_TX_Init(const ir_tx_cfg_t *cfg)
{
    if (cfg == NULL) {
        SYS_LOG("IR_TX: Init FAIL, cfg=NULL");
        return false;
    }
    if (s_inited) {
        return true;
    }
    s_cfg = *cfg;

    /* 外设时钟（GPIO/TIM）由工程 CubeMX 初始化开启，驱动不接管时钟使能 */

    /* 发射引脚：先拉低保证三极管关断，再切到复用推挽（走 OOP GPIO） */
    oop_gpio_init_af(&s_tx_gpio,
                     s_cfg.gpio_port, s_cfg.gpio_pin, true,
                     OOP_GPIO_MODE_AF_PP,
                     OOP_GPIO_PULL_DOWN,
                     OOP_GPIO_SPEED_HIGH,
                     s_cfg.gpio_af);
    OOP_GPIO_WRITE_RAW(&s_tx_gpio, GPIO_PIN_RESET);

    /* 时基：按注入参数配置 PWM 载波（走 OOP TIM） */
    s_htim_tx.Instance = s_cfg.tim_inst;
    if (!oop_tim_pwm_init(&s_htim_tx, s_cfg.tim_channel,
                          s_cfg.tim_psc, s_cfg.tim_arr, s_cfg.tim_ccr)) {
        SYS_LOG("IR_TX: TIM PWM init FAIL");
        return false;
    }

    /* 初始化后不启动计数器，发送时才开 */
    s_inited = true;
    SYS_LOG("IR_TX: Init OK (PWM carrier via injected TIM/channel)");
    return true;
}

void IR_TX_DeInit(void)
{
    if (!s_inited) {
        return;
    }
    ir_tx_carrier(false);
    oop_tim_pwm_deinit(&s_htim_tx);
    OOP_GPIO_WRITE_RAW(&s_tx_gpio, GPIO_PIN_RESET);
    s_inited = false;
    SYS_LOG("IR_TX: DeInit");
}

bool IR_TX_SendRaw(const uint16_t *dur_us, uint16_t count, uint8_t level_start,
                   uint8_t repeats, uint16_t repeat_gap_ms)
{
    if (!s_inited || dur_us == NULL || count == 0) {
        return false;
    }
    if (s_busy) {
        return false;
    }
    if (repeats == 0) {
        repeats = 1;
    }

    s_busy = true;

    for (uint8_t r = 0; r < repeats; r++) {
        uint8_t level = level_start;

        for (uint16_t i = 0; i < count; i++) {
            if (level == 0) {
                /* 载波段：点亮红外发射管（VS1838B 低电平 = 有载波） */
                ir_tx_carrier(true);
                oop_DelayUS(dur_us[i]);
                ir_tx_carrier(false);
            } else {
                /* 静默段：保持熄灭 */
                oop_DelayUS(dur_us[i]);
            }
            level ^= 1;
        }

        if ((r + 1) < repeats && repeat_gap_ms > 0) {
            oop_DelayMS(repeat_gap_ms);
        }
    }

    s_busy = false;
    return true;
}

bool IR_TX_IsBusy(void)
{
    return s_busy;
}
