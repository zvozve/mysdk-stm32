/**
 * @file    ir_transmitter.h
 * @brief   红外发射驱动（TIM 硬件载波 + CCR 门控时序）
 * @version V2.0
 * @date    2026-09-18
 *
 * 工作原理：
 *   - 载波 PWM 常开，用比较值(CCR)做门控：mark 时写 CCR=占空比，space 时写 CCR=0
 *     （PWM mode1 下 CNT<CCR 才有效，CCR=0 即恒无效）。相比反复 Start/Stop PWM，
 *     这是单次寄存器写，几乎无抖动。
 *     前提：必须关掉输出比较预装载(OCxPE=0)，否则 CCR 要等下一个更新事件才生效，
 *     最坏引入一个载波周期(26us)的抖动 —— 驱动在 init 里强制关掉。
 *   - 各段时长用一路**注入的自由运行定时器**做微秒计时（不用 DWT：部分 F1 板实测
 *     CYCCNT 不可靠，自检偶发通过却在发码时冻结）。该时基定时器由驱动配成
 *     1MHz 自由向上计数、ARR 取满量程，与接收侧的捕获定时器同源方案。
 *   - 发送为阻塞调用（空调帧约 50~100ms），但中断保持打开；IR 容差约 ±30%，
 *     几十 us 的抖动完全可接受。
 *
 * 电平语义：CCR=0 时输出为**无效电平** —— 高有效时 space=低、mark=38kHz 脉冲串；
 * 低有效(inverted)时 space=高、mark=低有效脉冲串。与一体化接收头
 * 「低=检测到载波」的约定天然对齐。
 *
 * 依赖注入：载波定时器句柄/通道/时钟、µs 时基定时器句柄/时钟由
 * ir_transmitter_cfg_t 传入（工程侧 board_cfg 提供），本文件不引用任何 CubeMX
 * 生成的符号、不直调 HAL。
 */

#ifndef __IR_TRANSMITTER_H
#define __IR_TRANSMITTER_H

#include <stdint.h>
#include <stdbool.h>
#include "oop_tim_drv.h"   /* TIM_HandleTypeDef / chip 层 TIM 原语（不依赖工程 tim.h） */

#ifdef __cplusplus
extern "C" {
#endif

/* ========== 配置（由工程 board_cfg 注入） ========== */

typedef struct {
    /* --- 载波通道（PWM） --- */
    TIM_HandleTypeDef *htim;           /**< 载波定时器句柄（通道须已配到发射引脚 AF） */
    uint32_t           channel;        /**< TIM_CHANNEL_x */
    uint32_t           tim_clk_hz;     /**< 载波定时器计数时钟（PSC 之前），如 72MHz */
    uint32_t           carrier_hz;     /**< 载波频率，红外一般 38000 */
    uint8_t            duty_percent;   /**< 占空比百分比，建议 30~50 */
    bool               inverted;       /**< 外部驱动是否反相（低有效点亮时置 true） */
    bool               use_carrier;    /**< true=本端产生 38kHz 载波(默认)；
                                            false=mark 只给有效电平，由外部模块内部调制 */

    /* --- µs 时基（自由运行计数器） --- */
    TIM_HandleTypeDef *tick_htim;      /**< 时基定时器句柄（任意一路空闲 TIM，无输出） */
    uint32_t           tick_tim_clk_hz;/**< 时基定时器计数时钟（PSC 之前），如 72MHz；
                                            驱动会把它配成 1MHz 自由运行 */
} ir_transmitter_cfg_t;

/* ========== 实例 ========== */

typedef struct {
    TIM_HandleTypeDef *htim;
    uint32_t           channel;
    uint32_t           tim_clk_hz;
    TIM_HandleTypeDef *tick_htim;
    uint32_t           tick_clk_hz;    /**< 时基实际计数频率（1MHz 目标，换算后回读） */
    uint32_t           tick_modulo;    /**< 时基模数 = ARR+1 */
    uint32_t           ccr_on;         /**< mark 时写入的比较值（占空比） */
    uint32_t           ccr_max;        /**< 恒有效电平时写入的比较值 = ARR */
    uint32_t           carrier_hz;     /**< 当前载波频率 */
    uint8_t            duty_percent;   /**< 当前占空比，缓存以支持运行时改载波 */
    bool               inverted;       /**< 当前极性 */
    bool               use_carrier;    /**< 当前载波产生方 */
    bool               ready;          /**< init 成功且时基健康；置 false 后须重新 init */
    volatile bool      busy;           /**< 正在发送（阻塞期间另一上下文可查） */
} ir_transmitter_t;

/* ========== API ========== */

/**
 * @brief   初始化发射实例：配置载波并启动 PWM（输出空号，不发光），配好 µs 时基
 * @param   tx   实例
 * @param   cfg  配置（载波句柄 + 时基句柄 + 时钟均须有效）
 * @retval  true 成功；false 参数非法 / 载波周期超出定时器量程 / 时基定时器不计时
 * @note    外设时钟由工程 CubeMX 的 MspInit 开启，驱动不接管时钟使能。
 *          时基定时器必须真的在计数（init 内探测一次），否则直接失败而非静默跑偏。
 */
bool ir_transmitter_init(ir_transmitter_t *tx, const ir_transmitter_cfg_t *cfg);

/**
 * @brief   反初始化（先给空号，再停 PWM 与时基计数）
 */
void ir_transmitter_deinit(ir_transmitter_t *tx);

/**
 * @brief   发一段有载波的 mark（阻塞 us 微秒）
 * @retval  true 正常；false 未就绪或时基冻结（已自动回空号并置 ready=false）
 */
bool ir_transmitter_mark(ir_transmitter_t *tx, uint16_t us);

/**
 * @brief   发一段无载波的 space（阻塞 us 微秒）
 */
bool ir_transmitter_space(ir_transmitter_t *tx, uint16_t us);

/**
 * @brief   播放一段 raw 时序（学习码回放的核心接口）
 * @param   tx       实例
 * @param   timings  时长数组，单位 us；**偶数下标 = mark（有载波），奇数下标 = space**
 * @param   len      条目数
 * @retval  true 发送完成；false 参数非法 / 时基异常
 * @note    发送前把载波计数器 CNT 清零以对齐相位，让首段 mark 干净起步；
 *          结束后一律回空号。重复发送（重复码）与帧间间隔由任务层控制。
 */
bool ir_transmitter_send(ir_transmitter_t *tx, const uint16_t *timings, uint16_t len);

/**
 * @brief   运行时切换输出极性（无需重初始化，立即生效）
 * @param   inverted false=高有效（mark 输出高），true=低有效（mark 输出低）
 * @retval  true 成功
 */
bool ir_transmitter_set_polarity(ir_transmitter_t *tx, bool inverted);

/**
 * @brief   运行时切换载波频率（重算 ARR/CCR 并立即装载，不重启 PWM）
 * @param   carrier_hz 新载波频率（如 38000）
 * @retval  true 成功；false 参数非法或超出定时器量程
 * @note    仅在空闲时调用（会触发更新事件复位计数器，正在发码时调用会打断本帧）。
 */
bool ir_transmitter_set_carrier(ir_transmitter_t *tx, uint32_t carrier_hz);

/**
 * @brief   运行时切换「载波产生方」
 * @param   use_carrier true=由本端 TIM 产生载波（默认）；
 *                      false=mark 只输出有效电平（原始数据），由外部模块内部调制。
 *                      某些红外模块（自带 38kHz 振荡）要求后者，否则会出现
 *                      「状态灯闪但 IR 管不发光」的现象。
 * @retval  true 成功
 */
bool ir_transmitter_set_carrier_mode(ir_transmitter_t *tx, bool use_carrier);

/**
 * @brief   载波通道是否就绪（可发送）
 */
bool ir_transmitter_is_ready(const ir_transmitter_t *tx);

/**
 * @brief   是否正在发送
 */
bool ir_transmitter_is_busy(const ir_transmitter_t *tx);

#ifdef __cplusplus
}
#endif

#endif /* __IR_TRANSMITTER_H */
