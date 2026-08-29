/**
 * @file    ir_1838b.c
 * @brief   1838B 红外接收驱动实现
 * @version V1.2
 * @date    2026-08-27
 */

#include "ir_1838b.h"
#include "oop_dwt.h"
#include "oop_tim_drv.h"
#include "SEGGER_RTT_Log.h"
#include "hal_platform.h"   /* TIM_HandleTypeDef / HAL_TIM_Base_Start_IT（不依赖工程 tim.h） */
#include <string.h>

/* ========== 内部变量 ========== */
static gpio_dev_t               s_ir_dev;
static TIM_HandleTypeDef        *s_htim = NULL;   /* 1ms 节拍定时器，由 IR1838B_Init 注入 */
static volatile bool            s_enabled = false;
static volatile bool            s_frame_ready = false;

static ir_raw_frame_t           s_raw;
static volatile uint16_t        s_edge_cnt = 0;
static volatile uint32_t        s_last_edge_cycle = 0;
static volatile uint8_t         s_last_level = 1;

static ir_protocol_decoder_t    s_decoder = NULL;

/* 调试计数 */
static volatile uint32_t        s_irq_cnt = 0;      /* 中断触发次数 */
static volatile uint32_t        s_frame_cnt = 0;     /* 成功接收帧数 */

/* ========== 中断回调 ========== */
static void ir_gpio_callback(uint16_t pin, void *user_data)
{
    (void)pin;
    (void)user_data;

    s_irq_cnt++;

    if (!s_enabled || s_frame_ready) {
        return;
    }

    uint32_t now   = oop_GetCycleCount();
    uint8_t  level = (OOP_GPIO_READ_RAW(&s_ir_dev) == GPIO_PIN_SET) ? 1 : 0;

    /* 第一个边沿：记录起始电平 */
    if (s_edge_cnt == 0 && s_last_edge_cycle == 0) {
        s_raw.level_start = level;  // 直接记录当前电平作为起始电平
        s_last_edge_cycle = now;
        s_last_level = level;
        return;
    }

    /* 计算时间间隔 */
    uint32_t us = oop_GetElapsedUS(s_last_edge_cycle, now);

    /* 超时检测 - 帧结束 */
    if (us > (IR_FRAME_TIMEOUT_MS * 1000U)) {
        if (s_edge_cnt >= 4) {
            s_raw.edges     = s_edge_cnt;
            s_raw.valid     = true;
            s_raw.timestamp = oop_GetTickMS();
            s_frame_ready   = true;
            s_frame_cnt++;
        }
        s_edge_cnt = 0;
        s_last_edge_cycle = now;
        s_last_level = level;
        return;
    }

    /* 保存边沿时间（只保存有效范围内的） */
    if (s_edge_cnt < IR_RAW_MAX_EDGES) {
        s_raw.timing_us[s_edge_cnt] = (uint16_t)us;
        s_edge_cnt++;
    }

    s_last_edge_cycle = now;
    s_last_level      = level;
}

/* ========== TIM6 节拍：静默收尾 ========== */
/**
 * @brief  由 TIM6 更新中断每 1ms 调用
 * @note   TIM6 中断与 EXTI9_5 同为抢占优先级 5，二者不会互相抢占，
 *         因此访问共享状态无需额外加锁。
 */
void IR1838B_Tick1ms(void)
{
    if (!s_enabled || s_frame_ready || s_edge_cnt == 0) {
        return;
    }

    uint32_t now = oop_GetCycleCount();
    uint32_t us  = oop_GetElapsedUS(s_last_edge_cycle, now);

    if (us <= (IR_FRAME_TIMEOUT_MS * 1000U)) {
        return;
    }

    /* 静默超时：收尾当前帧 */
    if (s_edge_cnt >= 4) {
        s_raw.edges     = s_edge_cnt;
        s_raw.valid     = true;
        s_raw.timestamp = oop_GetTickMS();
        s_frame_ready   = true;
        s_frame_cnt++;
    }

    /* 无论是否成帧都清零，等待下一个首边沿重新建立计时 */
    s_edge_cnt        = 0;
    s_last_edge_cycle = 0;
    s_last_level      = 1;
}

/* ========== API 实现 ========== */

bool IR1838B_Init(GPIO_TypeDef *port, uint16_t pin, TIM_HandleTypeDef *htim)
{
    if (port == NULL || htim == NULL) {
        SYS_LOG("IR1838B: Init FAIL, port/htim=NULL");
        return false;
    }
    s_htim = htim;

    memset(&s_raw, 0, sizeof(s_raw));
    s_edge_cnt        = 0;
    s_frame_ready     = false;
    s_enabled         = false;
    s_irq_cnt         = 0;
    s_frame_cnt       = 0;
    s_last_edge_cycle = 0;
    s_last_level      = 1;  // 空闲高电平

    /* 填充 GPIO 设备结构 */
    s_ir_dev.pin.port        = port;
    s_ir_dev.pin.pin         = pin;
    s_ir_dev.pin.active_high = true;
    s_ir_dev.is_initialized  = true;

    /* 注册中断回调（不重复配置 GPIO） */
    if (!oop_gpio_irq_register(port, pin, ir_gpio_callback, NULL,
                               OOP_GPIO_EDGE_BOTH, 0, OOP_GPIO_IRQ_LEVEL_ANY)) {
        SYS_LOG("IR1838B: IRQ register FAIL");
        return false;
    }

    s_enabled = true;

    /* 启动注入的 TIM（1ms 节拍）用于静默收尾。
     * TIM 由 CubeMX 配置好 1ms 参数，其更新中断里调用 IR1838B_Tick1ms；
     * SDK 只负责启动/停止，不记录具体定时器实例。 */
    if (!oop_tim_base_start_it(s_htim)) {
        SYS_LOG("IR1838B: TIM start FAIL");
    }

    uint8_t level = (OOP_GPIO_READ_RAW(&s_ir_dev) == GPIO_PIN_SET) ? 1 : 0;
    SYS_LOG("IR1838B: Init OK, pin level=%u (idle should be 1)", level);

    return true;
}

void IR1838B_DeInit(void)
{
    s_enabled = false;
    if (s_htim != NULL) {
        oop_tim_base_stop_it(s_htim);
    }
    oop_gpio_irq_unregister(s_ir_dev.pin.port, s_ir_dev.pin.pin);
    SYS_LOG("IR1838B: DeInit");
}

void IR1838B_Enable(bool enable)
{
    s_enabled = enable;
    oop_gpio_irq_enable(s_ir_dev.pin.port, s_ir_dev.pin.pin, enable);
    SYS_LOG("IR1838B: %s", enable ? "Enabled" : "Disabled");
}

bool IR1838B_Available(void)
{
    return s_frame_ready;
}

bool IR1838B_GetRaw(ir_raw_frame_t *frame)
{
    if (!s_frame_ready || frame == NULL) {
        return false;
    }

    __disable_irq();
    memcpy(frame, &s_raw, sizeof(ir_raw_frame_t));
    s_frame_ready     = false;
    s_edge_cnt        = 0;
    s_last_edge_cycle = 0;  /* 复位，让下一个边沿走“首边沿”分支重新建立计时 */
    s_last_level      = 1;
    __enable_irq();

    SYS_LOG("IR1838B: GetRaw OK, edges=%u, irq_cnt=%lu, frame_cnt=%lu",
            frame->edges, s_irq_cnt, s_frame_cnt);

    return frame->valid;
}

void IR1838B_RegisterDecoder(ir_protocol_decoder_t decoder)
{
    s_decoder = decoder;
    SYS_LOG("IR1838B: Decoder %s", decoder ? "registered" : "cleared");
}

bool IR1838B_Decode(void *result)
{
    if (s_decoder == NULL || !s_frame_ready) {
        return false;
    }

    ir_raw_frame_t frame;
    if (!IR1838B_GetRaw(&frame)) {
        return false;
    }

    bool ok = s_decoder(&frame, result);
    SYS_LOG("IR1838B: Decode %s", ok ? "OK" : "FAIL");
    return ok;
}

void IR1838B_PrintRaw(const ir_raw_frame_t *frame)
{
    if (frame == NULL || !frame->valid) {
        SYS_LOG("IR1838B: PrintRaw invalid frame");
        return;
    }

    SYS_LOG("IR RAW: edges=%u, start_level=%u, irq_total=%lu, frame_total=%lu",
            frame->edges, frame->level_start, s_irq_cnt, s_frame_cnt);

    for (uint16_t i = 0; i < frame->edges; i++) {
        SYS_LOG("  [%02u] %u us", i, frame->timing_us[i]);
    }
    SYS_LOG("IR RAW end -------------------");
}

/**
 * @brief  打印当前状态（可在任务里周期性调用）
 */
void IR1838B_PrintStatus(void)
{
    uint8_t level = 0;
    if (s_ir_dev.is_initialized) {
        level = (OOP_GPIO_READ_RAW(&s_ir_dev) == GPIO_PIN_SET) ? 1 : 0;
    }

    SYS_LOG("IR1838B Status: enabled=%d, level=%u, edges=%u, ready=%d, irq=%lu, frame=%lu",
            s_enabled,
            level,
            s_edge_cnt,
            s_frame_ready,
            s_irq_cnt,
            s_frame_cnt);
}