/**
 * @file    ir_receiver.h
 * @brief   红外接收驱动（解调输出型接收头，如 VS1838B/HX1838/HS0038）—— TIM 输入捕获
 * @version V2.0
 * @date    2026-09-18
 *
 * 工作原理：一体化接收头输出的是**已解调**信号（空闲为高，收到 38kHz 载波时拉低），
 * 因此只要测出相邻边沿之间的时长序列，就得到了协议的 mark/space 时序，与具体协议无关。
 *
 * 设计要点（V2.0 起改用硬件输入捕获，取代 V1.x 的「GPIO EXTI + DWT 软件计时」）：
 *  1. 用定时器输入捕获取硬件时间戳，ISR 只做「读 CCR -> 存差值」，抖动为 0；
 *     不靠中断响应精度吃饭，也不依赖 DWT（部分 F1 板实测 CYCCNT 不可靠）。
 *  2. 双沿捕获：F1 的通用定时器 CCER 只有 1 位极性位，硬件不支持双沿捕获
 *     （HAL 的 BOTHEDGE 在 F1 上会退化为单边沿）。驱动统一改为：启动时设 Falling，
 *     之后在 ir_receiver_isr() 里每捕获一个边沿就翻转一次极性，用软件模拟双沿。
 *     该做法在支持 BOTHEDGE 的系列（F4/G4）上同样可用，故不做平台分叉。
 *  3. 计数器回绕必须补偿：F1 全系定时器都是 16 位，16bit@1MHz 仅 65.5ms 就回绕一次，
 *     而一次按键的突发常横跨 100ms 以上。故驱动内所有时间差都按计数器模数 (ARR+1)
 *     做环形减法；模数在 ir_receiver_start() 里从 ARR 回读，16/32 位定时器通吃。
 *     硬限制：可测量的静默上限约为「模数的一半」（16bit@1MHz -> 约 32ms）。
 *     要跨越更长的段间间隔，应在任务层做「多段序列 + 段间 ms 间隔」记录，
 *     而不是把 idle_us 往上抬。
 *  4. 帧结束判定放在 ir_receiver_process() 里由主循环/任务轮询，不额外占定时器；
 *     裸机超循环与 RTOS 任务用同一套接口。
 *
 * 依赖注入：定时器句柄/通道/计数时钟/缓冲区由 ir_receiver_cfg_t 传入（工程侧
 * board_cfg 提供），本文件不引用任何 CubeMX 生成的符号、不直调 HAL。
 *
 * 用法（中断转接由工程侧完成，本驱动不接管 HAL 回调）：
 * @code
 *   void HAL_TIM_IC_CaptureCallback(TIM_HandleTypeDef *htim) {
 *       if (htim == BOARD_IR_RX_TIM) { ir_receiver_isr(&s_rx); }   // board_cfg 注入
 *   }
 * @endcode
 */

#ifndef __IR_RECEIVER_H
#define __IR_RECEIVER_H

#include <stdint.h>
#include <stdbool.h>
#include "oop_tim_drv.h"   /* TIM_HandleTypeDef / chip 层 TIM 原语（不依赖工程 tim.h） */

#ifdef __cplusplus
extern "C" {
#endif

/* ========== 默认参数 ========== */

/** 静默多久判定一帧结束（us）。美的两帧之间约 5.6ms、帧间约 100ms。 */
#define IR_RECEIVER_DEFAULT_IDLE_US        15000U

/**
 * 去抖死区（us）：接收头在近距离强信号下 AGC 会被灌饱和，解调输出跟随 38kHz
 * 载波产生密集毛刺边沿（26us 周期），会把缓冲撑爆。真实 mark/space 最短约
 * 560us，故用 200us 死区滤掉亚载波级尖刺，只保留包络跳变。
 * 间距小于死区的边沿直接丢弃（不更新 last_ts，后续边沿仍以最后一条有效边沿为基准）。
 */
#define IR_RECEIVER_DEFAULT_DEBOUNCE_US     200U

/* ========== 状态 ========== */

typedef enum {
    IR_RECEIVER_IDLE     = 0,   /**< 未开始采集 */
    IR_RECEIVER_BUSY     = 1,   /**< 采集中 */
    IR_RECEIVER_DONE     = 2,   /**< 一帧收完，数据可用（捕获已自动停止） */
    IR_RECEIVER_OVERFLOW = 3    /**< 缓冲溢出，本帧作废 */
} ir_receiver_state_t;

/* ========== 配置（由工程 board_cfg 注入） ========== */

typedef struct {
    TIM_HandleTypeDef *htim;        /**< 捕获定时器句柄（通道须已配到接收头 OUT 引脚） */
    uint32_t           channel;     /**< TIM_CHANNEL_x */
    uint32_t           clk_hz;      /**< 捕获计数频率（PSC 之后），1MHz 即 1 计数=1us */
    uint16_t          *buf;         /**< 时长缓冲，由调用方提供 */
    uint16_t           cap;         /**< 缓冲容量（条目数） */
    uint32_t           idle_us;     /**< 静默判帧结束阈值；0 = 默认值 */
    uint32_t           debounce_us; /**< 去抖死区；0 = 默认值 */
} ir_receiver_cfg_t;

/* ========== 实例 ========== */

typedef struct {
    TIM_HandleTypeDef *htim;
    uint32_t           channel;
    uint32_t           clk_hz;
    uint16_t          *buf;
    uint16_t           cap;
    uint32_t           idle_us;
    uint32_t           debounce_us;
    uint32_t           modulo;      /**< 计数器模数 = ARR+1，时间差按它环形减法补偿回绕 */

    volatile uint16_t  len;         /**< 已采集条目数 */
    volatile uint8_t   state;       /**< ir_receiver_state_t */
    volatile uint8_t   primed;      /**< 是否已收到第一个边沿（首个边沿只做时基基准） */
    volatile uint32_t  last_ts;     /**< 上一个有效边沿的计数值 */
} ir_receiver_t;

/* ========== API ========== */

/**
 * @brief   初始化接收实例（只登记配置，不启动捕获）
 * @param   rx   实例
 * @param   cfg  配置（句柄/通道/缓冲均须有效）
 * @retval  true 成功；false 参数非法
 */
bool ir_receiver_init(ir_receiver_t *rx, const ir_receiver_cfg_t *cfg);

/**
 * @brief   启动一次采集：复位缓冲、等待第一个边沿
 * @note    可重复调用（内部先防御性 Stop，使重复启动幂等）。
 *          收完一帧后捕获会自动停止，取走数据后须再次 start()。
 * @retval  true 成功
 */
bool ir_receiver_start(ir_receiver_t *rx);

/**
 * @brief   停止采集（只关通道与中断，定时器计数器继续跑）
 */
void ir_receiver_stop(ir_receiver_t *rx);

/**
 * @brief   主循环/任务里周期调用：判定静默超时并结帧
 * @retval  true 表示本次调用刚好收到一帧（状态转 DONE），此后须取数据并重新 start()
 * @note    只在 BUSY 状态下工作；已在 DONE/OVERFLOW 时直接返回 false，
 *          因此「一帧只上报一次」由状态机保证，调用方无需额外去重。
 */
bool ir_receiver_process(ir_receiver_t *rx);

/**
 * @brief   输入捕获中断入口
 * @note    必须在工程的 HAL_TIM_IC_CaptureCallback() 中、且确认是本实例的定时器后
 *          调用。ISR 内只做最轻量的取值与存数。
 */
void ir_receiver_isr(ir_receiver_t *rx);

/** 当前状态 */
ir_receiver_state_t ir_receiver_state(const ir_receiver_t *rx);

/** 已采集条目数（有效于 DONE / OVERFLOW） */
uint16_t ir_receiver_count(const ir_receiver_t *rx);

/**
 * @brief   取时长缓冲只读指针（有效于 DONE）
 * @note    偶数下标 = mark（有载波），奇数下标 = space，与 ir_transmitter_send() 回放约定一致。
 */
const uint16_t *ir_receiver_data(const ir_receiver_t *rx);

#ifdef __cplusplus
}
#endif

#endif /* __IR_RECEIVER_H */
