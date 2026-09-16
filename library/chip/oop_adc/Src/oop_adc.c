/**
 * @file    oop_adc.c
 * @brief   OOP 通用 ADC+DMA 采样与滤波实现
 * @see     oop_adc.h
 *
 * SDK 版：由工程 BSP/bsp_adc 迁入 chip 层并重命名（BSP_ADC / bsp_adc_ →
 * OOP_ADC / oop_adc_）。chip 层直调 HAL_ADCxx / HAL_DMAxx 合法。
 */
#include "oop_adc.h"

/* 模块私有状态 */
static struct {
    ADC_HandleTypeDef *hadc;
    DMA_HandleTypeDef *hdma;
    uint8_t  n_ch;
    uint16_t samples_per_ch;
    uint16_t raw[OOP_ADC_MAX_CH * OOP_ADC_MAX_SMP];
    uint16_t ch_mv[OOP_ADC_MAX_CH];
    bool     started;
} g_adc = {0};

/* 一批采样完成后的用户回调（中断上下文） */
static void (*g_batch_cb)(void) = NULL;

/* 中断里算好的快照，供任务上下文安全读取（避免与 DMA 改写竞争） */
static uint16_t g_snap_raw[OOP_ADC_MAX_CH];
static uint16_t g_snap_mv [OOP_ADC_MAX_CH];
static bool     g_snap_ready = false;

/* 前向声明：ADC 原始计数值(0..RES-1) → 电压(mV) 的命名换算 */
static uint16_t oop_adc_raw_to_mv(uint16_t raw);

void OOP_ADC_Init(ADC_HandleTypeDef *hadc, DMA_HandleTypeDef *hdma,
                  uint8_t n_ch, uint16_t samples_per_ch) {
    g_adc.hadc = hadc;
    g_adc.hdma = hdma;
    g_adc.n_ch = (n_ch > OOP_ADC_MAX_CH) ? OOP_ADC_MAX_CH : n_ch;
    g_adc.samples_per_ch = (samples_per_ch > OOP_ADC_MAX_SMP)
                             ? OOP_ADC_MAX_SMP : samples_per_ch;
    g_adc.started = false;
    for (int i = 0; i < OOP_ADC_MAX_CH; i++) g_adc.ch_mv[i] = 0;
}

void OOP_ADC_Start(void) {
    if (!g_adc.hadc || !g_adc.hdma) return;

    /* 强制 circular，保证持续采样（不被 CubeMX 默认 normal 截断） */
    g_adc.hdma->Init.Mode = DMA_CIRCULAR;
    HAL_DMA_Init(g_adc.hdma);

    /* ★ 2026-08-19 启动卡死根因修复：
     * CubeMX 配了 ContinuousConvMode=ENABLE —— ADC 被 TIM1 TRGO 启动后会
     * 自行连续转换（~0.35us/次），DMA 20 项约 7us 就满一轮 → DMA2_Ch3 中断
     * （优先级 0，最高抢占）每 ~7us 一次 → 批次 ISR 风暴，主循环饿死
     * （TaskWave_Init 卡在 OOP_ADC_Start 之后，实机日志铁证）。
     * 此处强制关闭 CONT：ADC 只由外部触发（TIM1 TRGO，10us）驱动，
     * DMA 20 项 = 10 次触发 = 100us 一轮 → 批次中断恢复 100us 一次。 */
    g_adc.hadc->Instance->CFGR &= ~ADC_CFGR_CONT;

    uint16_t total = (uint16_t)(g_adc.n_ch * g_adc.samples_per_ch);
    HAL_ADC_Start_DMA(g_adc.hadc, (uint32_t *)g_adc.raw, total);
    g_adc.started = true;
}

void OOP_ADC_Stop(void) {
    if (g_adc.hadc) HAL_ADC_Stop_DMA(g_adc.hadc);
    g_adc.started = false;
}

void OOP_ADC_SetBatchCb(void (*cb)(void)) {
    g_batch_cb = cb;
}

/* ---- ADC 转换完成回调：本模块是 ADC 的唯一所有者 ---- */
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc) {
    if (hadc != g_adc.hadc || !g_adc.started) return;

    /* 在中断里完成轻量滤波，把快照存好（任务里再慢慢用，避免竞争） */
    for (uint8_t ch = 0; ch < g_adc.n_ch; ch++) {
        uint16_t raw = oop_adc_filter_stride_rm_extreme(
                            g_adc.raw, (uint8_t)g_adc.samples_per_ch,
                            g_adc.n_ch, ch);
        g_snap_raw[ch] = raw;
        g_snap_mv[ch]  = oop_adc_raw_to_mv(raw);
    }
    g_snap_ready = true;

    if (g_batch_cb) g_batch_cb();
}

bool OOP_ADC_SnapshotReady(void) { return g_snap_ready; }
uint16_t OOP_ADC_SnapshotRaw(uint8_t ch) {
    return (ch < g_adc.n_ch) ? g_snap_raw[ch] : 0;
}
uint16_t OOP_ADC_SnapshotMV(uint8_t ch) {
    return (ch < g_adc.n_ch) ? g_snap_mv[ch] : 0;
}

uint16_t oop_adc_filter_stride_rm_extreme(const uint16_t *src, uint8_t count,
                                          uint8_t stride, uint8_t offset) {
    uint32_t sum = 0;
    uint16_t min = 0xFFFF, max = 0;

    if (src == NULL || count == 0 || stride == 0 || offset >= stride) return 0;

    if (count <= 2) {
        for (uint8_t i = 0; i < count; i++) sum += src[i * stride + offset];
        return (uint16_t)(sum / count);
    }

    for (uint8_t i = 0; i < count; i++) {
        uint16_t v = src[i * stride + offset];
        sum += v;
        if (v < min) min = v;
        if (v > max) max = v;
    }
    sum -= min;
    sum -= max;
    return (uint16_t)(sum / (count - 2));
}

/* ADC 原始计数值(0..RES-1) → 电压(mV)：模块内自用的命名换算 */
static uint16_t oop_adc_raw_to_mv(uint16_t raw) {
    if (OOP_ADC_RESOLUTION == 0) return 0;
    uint32_t t = (uint32_t)raw * OOP_ADC_VREF_MV;
    return (uint16_t)(t / OOP_ADC_RESOLUTION);
}

uint16_t OOP_ADC_FilteredRaw(uint8_t ch) {
    if (!g_adc.started || ch >= g_adc.n_ch) return 0;
    return oop_adc_filter_stride_rm_extreme(g_adc.raw,
                                           (uint8_t)g_adc.samples_per_ch,
                                           g_adc.n_ch, ch);
}

bool OOP_ADC_ReadChannel(uint8_t ch, uint16_t *mv_out) {
    if (!mv_out || ch >= g_adc.n_ch) return false;
    uint16_t raw = OOP_ADC_FilteredRaw(ch);
    g_adc.ch_mv[ch] = oop_adc_raw_to_mv(raw);
    *mv_out = g_adc.ch_mv[ch];
    return true;
}
