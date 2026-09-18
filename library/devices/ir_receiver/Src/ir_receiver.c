/**
 * @file    ir_receiver.c
 * @brief   红外接收驱动实现 —— 见 ir_receiver.h 顶部的设计说明
 * @version V2.0
 * @date    2026-09-18
 */

#include "ir_receiver.h"

/* ===========================
 * 计数 <-> 微秒换算
 * =========================== */

/** 捕获计数值 -> 微秒（先约掉整数倍关系，规避 32 位乘法溢出） */
static uint16_t ir_receiver_cnt_to_us(const ir_receiver_t *rx, uint32_t cnt)
{
    uint32_t per_us = rx->clk_hz / 1000000UL;
    uint32_t us;

    if (per_us > 0U) {
        us = cnt / per_us;                       /* 常见情况：clk_hz = 1MHz */
    } else {
        us = (uint32_t)(((uint64_t)cnt * 1000000ULL) / rx->clk_hz);
    }

    return (us > 0xFFFFUL) ? 0xFFFFU : (uint16_t)us;
}

/** 微秒 -> 捕获计数值（用于去抖/静默阈值比较） */
static uint32_t ir_receiver_us_to_cnt(const ir_receiver_t *rx, uint32_t us)
{
    uint64_t cnt = ((uint64_t)us * rx->clk_hz) / 1000000ULL;
    return (cnt > 0xFFFFFFFFULL) ? 0xFFFFFFFFUL : (uint32_t)cnt;
}

/* ===========================
 * API
 * =========================== */

bool ir_receiver_init(ir_receiver_t *rx, const ir_receiver_cfg_t *cfg)
{
    if ((rx == NULL) || (cfg == NULL) || (cfg->htim == NULL) ||
        (cfg->buf == NULL) || (cfg->cap == 0U) || (cfg->clk_hz == 0U)) {
        return false;
    }

    rx->htim        = cfg->htim;
    rx->channel     = cfg->channel;
    rx->clk_hz      = cfg->clk_hz;
    rx->buf         = cfg->buf;
    rx->cap         = cfg->cap;
    rx->idle_us     = (cfg->idle_us == 0U)     ? IR_RECEIVER_DEFAULT_IDLE_US     : cfg->idle_us;
    rx->debounce_us = (cfg->debounce_us == 0U) ? IR_RECEIVER_DEFAULT_DEBOUNCE_US : cfg->debounce_us;

    rx->len     = 0U;
    rx->state   = (uint8_t)IR_RECEIVER_IDLE;
    rx->primed  = 0U;
    rx->last_ts = 0U;
    /* 提前回读模数，使字段在 start() 之前即有效（start() 会再读一次并夹 idle） */
    rx->modulo  = oop_tim_period_get(rx->htim) + 1U;

    return true;
}

bool ir_receiver_start(ir_receiver_t *rx)
{
    if ((rx == NULL) || (rx->htim == NULL)) {
        return false;
    }

    /* 防御性停止：HAL_TIM_IC_Start_IT() 成功会把通道状态置 BUSY；
       若未先 Stop 就再次 Start（例如每帧重启），状态机检查会直接返回 HAL_ERROR。
       先 Stop 让通道回到 READY，使重复启动幂等（未启动时本就返回错误，忽略即可）。 */
    (void)oop_tim_ic_stop_it(rx->htim, rx->channel);

    /* 从 Falling 起：一体化接收头空闲=高、载波在场=低，故引导 mark 是「高→低」下降沿。
       首个被 primed 跳过的边沿恰为引导 mark 起点，下一上升沿才开始记时长
       -> buf[0]=引导 mark、偶数下标=mark，与发射侧「偶数=mark」回放约定一致。
       （早期误写成 Rising + "空闲为低"，会让引导 mark 被丢、整帧 mark/space 反相。） */
    if (!oop_tim_ic_init(rx->htim, rx->channel,
                         TIM_INPUTCHANNELPOLARITY_FALLING,
                         TIM_ICPSC_DIV1, 0U)) {
        return false;
    }

    rx->len     = 0U;
    rx->primed  = 0U;
    rx->last_ts = 0U;

    /* 回读计数器模数 = ARR+1，后续所有时间差按模数做环形减法，补偿 16 位定时器回绕，
       否则回绕边界落在帧内时 `now - last_ts` 会下溢成巨大值，被误判成静默超时把一帧截断。 */
    rx->modulo = oop_tim_period_get(rx->htim) + 1U;

    /* 受计数器量程限制，可测静默上限约「模数的一半」(16bit@1MHz -> 约 32ms)。
       超过此值的段间间隔不应靠抬 idle_us 解决，而应在任务层做多段序列记录。 */
    {
        uint32_t max_idle = rx->modulo / 2U;
        if (rx->idle_us > max_idle) {
            rx->idle_us = max_idle;
        }
    }

    rx->state = (uint8_t)IR_RECEIVER_BUSY;

    if (!oop_tim_ic_start_it(rx->htim, rx->channel)) {
        rx->state = (uint8_t)IR_RECEIVER_IDLE;
        return false;
    }
    return true;
}

void ir_receiver_stop(ir_receiver_t *rx)
{
    if ((rx == NULL) || (rx->htim == NULL)) {
        return;
    }
    (void)oop_tim_ic_stop_it(rx->htim, rx->channel);
    if (rx->state == (uint8_t)IR_RECEIVER_BUSY) {
        rx->state = (uint8_t)IR_RECEIVER_IDLE;
    }
}

bool ir_receiver_process(ir_receiver_t *rx)
{
    if ((rx == NULL) || (rx->state != (uint8_t)IR_RECEIVER_BUSY) || (!rx->primed)) {
        return false;
    }

    uint32_t now     = oop_tim_counter_get(rx->htim);
    uint32_t elapsed = (now - rx->last_ts) % rx->modulo;   /* 环形减法补偿回绕 */

    if (elapsed > ir_receiver_us_to_cnt(rx, rx->idle_us)) {
        /* 静默超时 -> 认为一帧结束。停掉捕获，防止半截数据继续写入。 */
        (void)oop_tim_ic_stop_it(rx->htim, rx->channel);
        rx->state = (uint8_t)IR_RECEIVER_DONE;
        return true;
    }
    return false;
}

void ir_receiver_isr(ir_receiver_t *rx)
{
    if ((rx == NULL) || (rx->htim == NULL)) {
        return;
    }

    /* 先无条件读一次 CCR（同时清中断标志），再判断是否要记录 */
    uint32_t now = oop_tim_ic_read_capture(rx->htim, rx->channel);

    if (rx->state != (uint8_t)IR_RECEIVER_BUSY) {
        return;
    }

    /* 翻转极性，准备捕获下一个（反向）边沿 —— 软件模拟双沿 */
    oop_tim_ic_toggle_polarity(rx->htim, rx->channel);

    if (!rx->primed) {
        /* 第一个边沿只作为时基基准，不产生时长条目 */
        rx->primed  = 1U;
        rx->last_ts = now;
        return;
    }

    uint32_t dt = (now - rx->last_ts) % rx->modulo;

    /* 去抖：丢弃死区内的密集毛刺边沿，避免 AGC 饱和时缓冲溢出 */
    if (dt < ir_receiver_us_to_cnt(rx, rx->debounce_us)) {
        return;
    }
    rx->last_ts = now;

    if (rx->len >= rx->cap) {
        rx->state = (uint8_t)IR_RECEIVER_OVERFLOW;
        return;
    }

    rx->buf[rx->len++] = ir_receiver_cnt_to_us(rx, dt);
}

/* ===========================
 * 查询
 * =========================== */

ir_receiver_state_t ir_receiver_state(const ir_receiver_t *rx)
{
    return (rx == NULL) ? IR_RECEIVER_IDLE : (ir_receiver_state_t)rx->state;
}

uint16_t ir_receiver_count(const ir_receiver_t *rx)
{
    return (rx == NULL) ? 0U : rx->len;
}

const uint16_t *ir_receiver_data(const ir_receiver_t *rx)
{
    return (rx == NULL) ? NULL : rx->buf;
}
