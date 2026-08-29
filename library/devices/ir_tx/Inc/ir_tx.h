/**
 * @file    ir_tx.h
 * @brief   红外发射驱动（TIM 硬件 38kHz 载波 + 软件门控时序）
 * @version V1.0
 * @date    2026-08-27
 *
 * 原理：
 *   - 定时器产生精确的 38kHz PWM 载波（硬件保证频率/占空比，无抖动）
 *   - 发射“mark”段时使能载波输出，发射“space”段时关闭输出
 *   - 各段时长用 DWT 微秒延时门控，从而原样重放学习到的波形
 *
 * 电平语义（与 ir_1838b 接收侧一致）：
 *   - VS1838B 输出 低电平 = 检测到 38kHz 载波（mark）
 *   - 因此重放时，段电平为 0 → 点亮红外发射管
 */

#ifndef __IR_TX_H
#define __IR_TX_H

#include <stdint.h>
#include <stdbool.h>
#include "hal_platform.h"   /* GPIO_TypeDef / TIM_TypeDef / HAL 类型（不依赖工程 main.h） */

#ifdef __cplusplus
extern "C" {
#endif

/* ========== 绑定配置（由工程 board_cfg 注入，SDK 不记录任何具体引脚/定时器） ========== */
typedef struct {
    GPIO_TypeDef    *gpio_port;     /* 发射引脚所在端口，如 GPIOE */
    uint16_t         gpio_pin;      /* 发射引脚，如 GPIO_PIN_6 */
    uint8_t          gpio_af;       /* 引脚复用功能，如 GPIO_AF3_TIM9 */
    TIM_TypeDef     *tim_inst;      /* 载波定时器，如 TIM9 */
    uint32_t         tim_channel;   /* PWM 通道，如 TIM_CHANNEL_2 */
    uint32_t         tim_psc;       /* 预分频：计数频率 = TIM_CLK/(PSC+1) */
    uint32_t         tim_arr;       /* 自动重装：周期 = (ARR+1) 计数 */
    uint32_t         tim_ccr;       /* 比较值：占空比 = CCR/(ARR+1) */
} ir_tx_cfg_t;

/* ========== API ========== */

/**
 * @brief  初始化红外发射（按注入配置初始化 GPIO + PWM，初始化后输出关闭）
 * @param  cfg  硬件绑定配置（引脚/定时器/载波参数），由工程提供
 * @retval true 成功
 * @note   外设时钟（GPIO/TIM）由工程 CubeMX 初始化开启，驱动不接管时钟使能。
 */
bool IR_TX_Init(const ir_tx_cfg_t *cfg);

/**
 * @brief  反初始化（关闭载波，停定时器）
 */
void IR_TX_DeInit(void);

/**
 * @brief  重放一帧原始时序（学习码回放的核心接口）
 * @param  dur_us       各段时长数组（us），即 ir_raw_frame_t.timing_us
 * @param  count        段数，即 ir_raw_frame_t.edges
 * @param  level_start  第一段电平：0=载波段（发射）1=静默段，即 ir_raw_frame_t.level_start
 * @param  repeats      重复次数（空调单次发射传 1；NEC 重复码场景可 >1）
 * @param  repeat_gap_ms 重复帧之间的间隔（ms），repeats=1 时无效
 * @retval true 发送完成
 * @note   阻塞调用：空调帧约持续 50~100ms；发送期间红外接收中断不受影响。
 */
bool IR_TX_SendRaw(const uint16_t *dur_us, uint16_t count, uint8_t level_start,
                   uint8_t repeats, uint16_t repeat_gap_ms);

/**
 * @brief  是否正在发送
 */
bool IR_TX_IsBusy(void);

#ifdef __cplusplus
}
#endif

#endif /* __IR_TX_H */
