/**
 * @file    ir_tx.c
 * @brief   红外发射驱动实现（TIM9 38kHz 载波 + DWT 门控）
 * @version V1.0
 * @date    2026-08-27
 */

#include "ir_tx.h"
#include "oop_dwt.h"
#include "SEGGER_RTT_Log.h"

/* ========== 内部状态 ========== */
static ir_tx_cfg_t          s_cfg;
static TIM_HandleTypeDef    s_htim_tx;
static volatile bool        s_busy    = false;
static bool                 s_inited  = false;

/* ========== 载波门控 ========== */
static inline void ir_tx_carrier(bool on)
{
    if (on) {
        HAL_TIM_PWM_Start(&s_htim_tx, s_cfg.tim_channel);
    } else {
        HAL_TIM_PWM_Stop(&s_htim_tx, s_cfg.tim_channel);
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

    /* 发射引脚：先拉低保证三极管关断，再切到复用推挽 */
    HAL_GPIO_WritePin(s_cfg.gpio_port, s_cfg.gpio_pin, GPIO_PIN_RESET);

    GPIO_InitTypeDef gpio = {0};
    gpio.Pin       = s_cfg.gpio_pin;
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_PULLDOWN;     /* 载波关闭时保持三极管可靠关断 */
    gpio.Speed     = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = s_cfg.gpio_af;
    HAL_GPIO_Init(s_cfg.gpio_port, &gpio);

    /* 时基：按注入参数配置 PWM 载波 */
    s_htim_tx.Instance               = s_cfg.tim_inst;
    s_htim_tx.Init.Prescaler         = s_cfg.tim_psc;
    s_htim_tx.Init.CounterMode       = TIM_COUNTERMODE_UP;
    s_htim_tx.Init.Period            = s_cfg.tim_arr;
    s_htim_tx.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    s_htim_tx.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    if (HAL_TIM_PWM_Init(&s_htim_tx) != HAL_OK) {
        SYS_LOG("IR_TX: TIM PWM init FAIL");
        return false;
    }

    TIM_OC_InitTypeDef oc = {0};
    oc.OCMode     = TIM_OCMODE_PWM1;
    oc.Pulse      = s_cfg.tim_ccr;
    oc.OCPolarity = TIM_OCPOLARITY_HIGH;
    oc.OCFastMode = TIM_OCFAST_DISABLE;
    if (HAL_TIM_PWM_ConfigChannel(&s_htim_tx, &oc, s_cfg.tim_channel) != HAL_OK) {
        SYS_LOG("IR_TX: PWM channel config FAIL");
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
    HAL_TIM_PWM_DeInit(&s_htim_tx);
    HAL_GPIO_WritePin(s_cfg.gpio_port, s_cfg.gpio_pin, GPIO_PIN_RESET);
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
