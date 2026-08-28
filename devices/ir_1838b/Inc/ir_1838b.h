/**
 * @file    ir_1838b.h
 * @brief   1838B 红外接收驱动（基于 bsp_gpio）
 * @version V1.0
 * @date    2026-08-25
 */

#ifndef __IR_1838B_H
#define __IR_1838B_H

#include <stdint.h>
#include <stdbool.h>
#include "bsp_gpio_drv.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========== 配置 ========== */
/* 空调帧边沿数较多：格力两段帧约 136，美的三连帧接近 300，预留到 320 */
#define IR_RAW_MAX_EDGES        320     /* 一帧最大边沿数 */
#define IR_FRAME_TIMEOUT_MS     30      /* 帧超时时间（ms） */
#define IR_IDLE_HIGH            1       /* 1838B 空闲为高电平 */

/* ========== 原始数据 ========== */
typedef struct {
    uint16_t    edges;                          /* 边沿数量 */
    uint16_t    timing_us[IR_RAW_MAX_EDGES];    /* 每个电平持续时间（us） */
    uint8_t     level_start;                    /* 第一个电平（0低 1高） */
    uint32_t    timestamp;                      /* 接收完成时间戳 */
    bool        valid;
} ir_raw_frame_t;

/* ========== 协议解析回调（给空调协议预留） ========== */
typedef bool (*ir_protocol_decoder_t)(const ir_raw_frame_t *raw, void *result);

/* ========== API ========== */

/**
 * @brief  初始化 1838B 接收
 * @param  port, pin  1838B OUT 连接的 GPIO
 */
bool IR1838B_Init(GPIO_TypeDef *port, uint16_t pin);

/**
 * @brief  1ms 周期节拍（由 TIM6 更新中断调用）
 * @note   检测“静默超时”并收尾当前帧。空调遥控多为单次长帧、无重复码，
 *         帧结束后不会再有边沿到来，必须由超时机制收尾，
 *         否则帧要等下一次按键的第一个边沿才能上报。
 */
void IR1838B_Tick1ms(void);

/**
 * @brief  反初始化
 */
void IR1838B_DeInit(void);

/**
 * @brief  使能/禁用接收
 */
void IR1838B_Enable(bool enable);

/**
 * @brief  是否有新帧
 */
bool IR1838B_Available(void);

/**
 * @brief  获取一帧原始数据
 */
bool IR1838B_GetRaw(ir_raw_frame_t *frame);

/**
 * @brief  注册协议解码器
 */
void IR1838B_RegisterDecoder(ir_protocol_decoder_t decoder);

/**
 * @brief  使用已注册解码器解析
 */
bool IR1838B_Decode(void *result);

/**
 * @brief  打印原始帧（用于协议逆向）
 */
void IR1838B_PrintRaw(const ir_raw_frame_t *frame);

/**
 * @brief  打印当前驱动状态（调试用）
 */
void IR1838B_PrintStatus(void);

#ifdef __cplusplus
}
#endif

#endif /* __IR_1838B_H */