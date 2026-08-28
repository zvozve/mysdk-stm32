/**
 * @file    ir_tx.c
 * @brief   红外发射驱动实现（TIM9 38kHz 载波 + DWT 门控）
 * @version V1.0
 * @date    2026-08-27
 */

#include "ir_tx.h"
#include "bsp_dwt.h"
#include "SEGGER_RTT_Log.h"

/* ========== 内部状态 ========== */
static TIM_HandleTypeDef    s_htim_tx;
static volatile bool        s_busy    = false;
static bool                 s_inited  = false;

/* ========== 载波门控 ========== */
static inline void ir_tx_carrier(bool on)
{
    if (on) {
        HAL_TIM_PWM_Start(&s_htim_tx, IR_TX_TIM_CHANNEL);
    } else {
        HAL_TIM_PWM_Stop(&s_htim_tx, IR_TX_TIM_CHANNEL);
    }
}

/* ========== API ========== */

bool IR_TX_Init(void)
{
    if (s_inited) {
        return true;
    }

    /* 外设时钟 */
    IR_TX_TIM_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();

    /* 发射引脚：先拉低保证三极管关断，再切到复用推挽 */
    HAL_GPIO_WritePin(IR_TX_GPIO_PORT, IR_TX_GPIO_PIN, GPIO_PIN_RESET);

    GPIO_InitTypeDef gpio = {0};
    gpio.Pin       = IR_TX_GPIO_PIN;
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_PULLDOWN;     /* 载波关闭时保持三极管可靠关断 */
    gpio.Speed     = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = IR_TX_GPIO_AF;
    HAL_GPIO_Init(IR_TX_GPIO_PORT, &gpio);

    /* 时基：168MHz / 168 = 1MHz，ARR=25 → 26us 周期 ≈ 38.46kHz */
    s_htim_tx.Instance               = IR_TX_TIM;
    s_htim_tx.Init.Prescaler         = IR_TX_TIM_PSC;
    s_htim_tx.Init.CounterMode       = TIM_COUNTERMODE_UP;
    s_htim_tx.Init.Period            = IR_TX_TIM_ARR;
    s_htim_tx.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    s_htim_tx.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    if (HAL_TIM_PWM_Init(&s_htim_tx) != HAL_OK) {
        SYS_LOG("IR_TX: TIM PWM init FAIL");
        return false;
    }

    TIM_OC_InitTypeDef oc = {0};
    oc.OCMode     = TIM_OCMODE_PWM1;
    oc.Pulse      = IR_TX_TIM_CCR;
    oc.OCPolarity = TIM_OCPOLARITY_HIGH;
    oc.OCFastMode = TIM_OCFAST_DISABLE;
    if (HAL_TIM_PWM_ConfigChannel(&s_htim_tx, &oc, IR_TX_TIM_CHANNEL) != HAL_OK) {
        SYS_LOG("IR_TX: PWM channel config FAIL");
        return false;
    }

    /* 初始化后不启动计数器，发送时才开 */
    s_inited = true;
    SYS_LOG("IR_TX: Init OK (PE6/TIM9_CH2, carrier ~38.46kHz, duty ~35%%)");
    return true;
}

void IR_TX_DeInit(void)
{
    if (!s_inited) {
        return;
    }
    ir_tx_carrier(false);
    HAL_TIM_PWM_DeInit(&s_htim_tx);
    HAL_GPIO_WritePin(IR_TX_GPIO_PORT, IR_TX_GPIO_PIN, GPIO_PIN_RESET);
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
                bsp_DelayUS(dur_us[i]);
                ir_tx_carrier(false);
            } else {
                /* 静默段：保持熄灭 */
                bsp_DelayUS(dur_us[i]);
            }
            level ^= 1;
        }

        if ((r + 1) < repeats && repeat_gap_ms > 0) {
            bsp_DelayMS(repeat_gap_ms);
        }
    }

    s_busy = false;
    return true;
}

bool IR_TX_IsBusy(void)
{
    return s_busy;
}
