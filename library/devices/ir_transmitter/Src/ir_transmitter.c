/**
 * @file    ir_transmitter.c
 * @brief   红外发射驱动实现 —— 见 ir_transmitter.h 顶部的设计说明
 * @version V2.0
 * @date    2026-09-18
 */

#include "ir_transmitter.h"

/* 时基定时器目标分辨率：1 计数 = 1us */
#define IR_TRANSMITTER_TICK_HZ          1000000UL

/* 时基 ARR：16/32 位定时器通吃的满量程（模数 = 65536） */
#define IR_TRANSMITTER_TICK_PERIOD      0xFFFFUL

/* 死机保护：连续读到同一个计数值超过此次数，判定时基冻结。
   1MHz 计数下该值不可能停留这么久（>1us 就会变），故不会误判；
   同时把「时基假死」的挂死时间限制在毫秒级，绝不拖死主循环。 */
#define IR_TRANSMITTER_TICK_STUCK_MAX   50000UL

/* ===========================
 * 内部：µs 时基
 * =========================== */

/** 秒 -> 计数（1MHz 时 1:1；非整除时向下取整，只影响极短段，误差远小于 IR 容差） */
static uint32_t ir_transmitter_us_to_ticks(const ir_transmitter_t *tx, uint32_t us)
{
    uint32_t per_us = tx->tick_clk_hz / IR_TRANSMITTER_TICK_HZ;

    if (per_us > 0U) {
        return us * per_us;
    }
    return (uint32_t)(((uint64_t)us * tx->tick_clk_hz) / IR_TRANSMITTER_TICK_HZ);
}

/**
 * @brief   阻塞等待 us 微秒（环形减法，容忍计数器回绕）
 * @retval  true 正常；false 时基冻结（已置 ready=false）
 */
static bool ir_transmitter_wait(ir_transmitter_t *tx, uint16_t us)
{
    if (!tx->ready) {
        return false;
    }
    if (us == 0U) {
        return true;
    }

    const uint32_t start  = oop_tim_counter_get(tx->tick_htim);
    const uint32_t target = ir_transmitter_us_to_ticks(tx, (uint32_t)us);
    uint32_t       last   = start;
    uint32_t       stuck  = 0U;

    for (;;) {
        uint32_t now = oop_tim_counter_get(tx->tick_htim);

        if (((now - start) % tx->tick_modulo) >= target) {
            return true;
        }
        if (now == last) {
            if (++stuck > IR_TRANSMITTER_TICK_STUCK_MAX) {
                /* 时基冻结：不是代码死循环，但继续等下去等于挂死主循环。
                   直接判废，置 ready=false 让后续发送立即返回失败。 */
                tx->ready = false;
                return false;
            }
        } else {
            last  = now;
            stuck = 0U;
        }
    }
}

/* ===========================
 * 内部：载波门控
 * =========================== */

/** on=true 有载波（mark），on=false 空号（space）。
 *  use_carrier=false 时 mark 改为恒「有效电平」(CCR=ARR，PWM mode1 下约 100% 占空比)，
 *  把原始数据交给外部模块内部去调制。space 一律 CCR=0（无效电平）。 */
static void ir_transmitter_carrier_set(ir_transmitter_t *tx, bool on)
{
    uint32_t ccr = 0U;
    if (on) {
        ccr = tx->use_carrier ? tx->ccr_on : tx->ccr_max;
    }
    (void)oop_tim_ccr_write(tx->htim, tx->channel, ccr);
}

/** 由载波频率/占空比算 (ARR, CCR)，超出定时器量程返回 false */
static bool ir_transmitter_calc_carrier(uint32_t tim_clk_hz, uint32_t carrier_hz,
                                        uint8_t duty_percent,
                                        uint32_t *period_out, uint32_t *ccr_out)
{
    if ((carrier_hz == 0U) || (tim_clk_hz == 0U) || (duty_percent == 0U) ||
        (duty_percent >= 100U)) {
        return false;
    }

    /* 载波周期（计数值），四舍五入到最近的整数分频 */
    uint32_t period = (tim_clk_hz + (carrier_hz / 2U)) / carrier_hz;
    if ((period < 2U) || (period > (IR_TRANSMITTER_TICK_PERIOD + 1UL))) {
        return false;   /* 定时器装不下，需先用 PSC 降频 */
    }

    uint32_t ccr = (period * (uint32_t)duty_percent) / 100U;
    if (ccr >= period) {
        ccr = period - 1U;
    }
    if (ccr == 0U) {
        ccr = 1U;
    }

    *period_out = period;
    *ccr_out    = ccr;
    return true;
}

/* ===========================
 * API
 * =========================== */

bool ir_transmitter_init(ir_transmitter_t *tx, const ir_transmitter_cfg_t *cfg)
{
    if ((tx == NULL) || (cfg == NULL) || (cfg->htim == NULL) ||
        (cfg->tick_htim == NULL) || (cfg->tick_tim_clk_hz == 0U)) {
        return false;
    }

    uint32_t period = 0U;
    uint32_t ccr    = 0U;
    if (!ir_transmitter_calc_carrier(cfg->tim_clk_hz, cfg->carrier_hz,
                                     cfg->duty_percent, &period, &ccr)) {
        return false;
    }

    /* 载波定时器时基：PSC=0，靠 ARR 分频（工程侧 .ioc 的 Period/Pulse 仅用于
       让 CubeMX 生成 PWM 通道，实际值以 cfg 为准）。 */
    if (!oop_tim_pwm_init(cfg->htim, cfg->channel, 0U, period - 1U, 0U)) {
        return false;
    }

    /* 关键：关掉输出比较预装载，保证写 CCR 立即生效 */
    if (!oop_tim_oc_preload_disable(cfg->htim, cfg->channel)) {
        return false;
    }

    /* µs 时基：配成 1MHz 自由运行（ARR 取满量程，容忍回绕） */
    {
        uint32_t psc = (cfg->tick_tim_clk_hz + (IR_TRANSMITTER_TICK_HZ / 2U))
                       / IR_TRANSMITTER_TICK_HZ;
        if (psc == 0U) {
            psc = 1U;   /* 输入时钟已低于 1MHz，退化为 1 分频，精度下降 */
        }
        psc -= 1U;

        if (!oop_tim_base_init(cfg->tick_htim, psc, IR_TRANSMITTER_TICK_PERIOD)) {
            return false;
        }
        (void)oop_tim_counter_enable(cfg->tick_htim);

        tx->tick_htim   = cfg->tick_htim;
        tx->tick_clk_hz = cfg->tick_tim_clk_hz / (psc + 1U);
        tx->tick_modulo = IR_TRANSMITTER_TICK_PERIOD + 1U;
    }

    tx->htim         = cfg->htim;
    tx->channel      = cfg->channel;
    tx->tim_clk_hz   = cfg->tim_clk_hz;
    tx->ccr_on       = ccr;
    tx->ccr_max      = period - 1U;
    tx->carrier_hz   = cfg->carrier_hz;
    tx->duty_percent = cfg->duty_percent;
    tx->inverted     = cfg->inverted;
    tx->use_carrier  = cfg->use_carrier;
    tx->busy         = false;
    tx->ready        = false;   /* 探测通过后才置位 */

    (void)oop_tim_oc_polarity_set(cfg->htim, cfg->channel, cfg->inverted);

    /* 先给空号，再启动：PWM 常开，完全靠 CCR 门控 */
    (void)oop_tim_ccr_write(cfg->htim, cfg->channel, 0U);
    if (!oop_tim_pwm_start(cfg->htim, cfg->channel)) {
        return false;
    }

    /* 时基健康探测：不真的在计数就直接失败，避免后续每段延时都跑满保护计数
       （宁可 init 明确报错，也不要发出时序全错的波形）。 */
    {
        uint32_t a = oop_tim_counter_get(tx->tick_htim);
        for (volatile uint32_t i = 0U; i < 1000U; i++) {
            /* 空转等计数前进 */
        }
        if (oop_tim_counter_get(tx->tick_htim) == a) {
            return false;
        }
    }

    tx->ready = true;
    return true;
}

void ir_transmitter_deinit(ir_transmitter_t *tx)
{
    if ((tx == NULL) || (tx->htim == NULL)) {
        return;
    }
    ir_transmitter_carrier_set(tx, false);
    (void)oop_tim_pwm_stop(tx->htim, tx->channel);
    if (tx->tick_htim != NULL) {
        (void)oop_tim_counter_disable(tx->tick_htim);
    }
    tx->ready = false;
    tx->busy  = false;
}

bool ir_transmitter_mark(ir_transmitter_t *tx, uint16_t us)
{
    if ((tx == NULL) || (!tx->ready)) {
        return false;
    }
    ir_transmitter_carrier_set(tx, true);
    if (!ir_transmitter_wait(tx, us)) {
        ir_transmitter_carrier_set(tx, false);   /* 异常时不留常亮 */
        return false;
    }
    return true;
}

bool ir_transmitter_space(ir_transmitter_t *tx, uint16_t us)
{
    if ((tx == NULL) || (!tx->ready)) {
        return false;
    }
    ir_transmitter_carrier_set(tx, false);
    return ir_transmitter_wait(tx, us);
}

bool ir_transmitter_send(ir_transmitter_t *tx, const uint16_t *timings, uint16_t len)
{
    if ((tx == NULL) || (!tx->ready) || (timings == NULL) || (len == 0U)) {
        return false;
    }

    tx->busy = true;

    /* 对齐载波相位，让首段 mark 干净起步 */
    oop_tim_counter_set(tx->htim, 0U);

    bool ok = true;
    /* 逐段独立计时（不依赖全局 deadline 累加，避免某段误差向后传播） */
    for (uint16_t i = 0U; i < len; i++) {
        ir_transmitter_carrier_set(tx, (i & 1U) == 0U);
        if (!ir_transmitter_wait(tx, timings[i])) {
            ok = false;
            break;
        }
    }

    ir_transmitter_carrier_set(tx, false);
    tx->busy = false;
    return ok;
}

bool ir_transmitter_set_polarity(ir_transmitter_t *tx, bool inverted)
{
    if ((tx == NULL) || (!tx->ready)) {
        return false;
    }
    if (!oop_tim_oc_polarity_set(tx->htim, tx->channel, inverted)) {
        return false;
    }
    tx->inverted = inverted;
    return true;
}

bool ir_transmitter_set_carrier(ir_transmitter_t *tx, uint32_t carrier_hz)
{
    if ((tx == NULL) || (!tx->ready)) {
        return false;
    }

    uint32_t period = 0U;
    uint32_t ccr    = 0U;
    if (!ir_transmitter_calc_carrier(tx->tim_clk_hz, carrier_hz,
                                     tx->duty_percent, &period, &ccr)) {
        return false;
    }

    (void)oop_tim_period_set(tx->htim, period - 1U);
    /* 强制一次更新事件装载 ARR（载波相位无所谓；CCR 无预装载，下次 mark 直接生效） */
    oop_tim_generate_update(tx->htim);

    tx->ccr_on     = ccr;
    tx->ccr_max    = period - 1U;
    tx->carrier_hz = carrier_hz;
    return true;
}

bool ir_transmitter_set_carrier_mode(ir_transmitter_t *tx, bool use_carrier)
{
    if ((tx == NULL) || (!tx->ready)) {
        return false;
    }
    tx->use_carrier = use_carrier;
    return true;
}

bool ir_transmitter_is_ready(const ir_transmitter_t *tx)
{
    return (tx != NULL) && tx->ready;
}

bool ir_transmitter_is_busy(const ir_transmitter_t *tx)
{
    return (tx != NULL) && tx->busy;
}
