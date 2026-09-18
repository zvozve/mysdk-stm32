/**
 * @file    ac_codec.h
 * @brief   空调红外协议编码器（纯 C，无平台依赖）
 * @version V1.0
 * @date    2026-08-27
 *
 * 将通用空调状态（电源/模式/温度/风速/强力）编码为原始时序数组，
 * 输出可直接交给 ir_transmitter_send() 发射。
 *
 * 已支持品牌：
 *   - 美的 (Midea)：48bit 状态帧 + 全反相校验帧，参考 IRremoteESP8266 逆向成果
 *   - 志高 (Chigo)：96bit 状态帧，基于 Flipper-IRDB 真机采集逆向
 *       * LEGACY  (CS-21H3A-B155AF)：字节校验和 = sum(byte0..10)
 *       * KRF51G  (KRF-51G_79F)：字节互补冗余 + XOR 校验
 */

#ifndef __AC_CODEC_H
#define __AC_CODEC_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========== 通用空调状态 ========== */

typedef enum {
    AC_BRAND_MIDEA = 0,     /* 美的 */
    AC_BRAND_CHIGO,         /* 志高 */
} ac_brand_t;

typedef enum {
    AC_MODE_AUTO = 0,
    AC_MODE_COOL,
    AC_MODE_DRY,
    AC_MODE_FAN,
    AC_MODE_HEAT,
} ac_mode_t;

typedef enum {
    AC_FAN_AUTO = 0,
    AC_FAN_LOW,
    AC_FAN_MID,
    AC_FAN_HIGH,
} ac_fan_t;

typedef struct {
    bool      power;        /* 电源开关 */
    ac_mode_t mode;         /* 运行模式 */
    uint8_t   temp_c;       /* 设定温度（摄氏度，内部自动限幅） */
    ac_fan_t  fan;          /* 风速 */
    bool      turbo;        /* 强力（目前仅美的/志高 LEGACY 格式支持） */
} ac_state_t;

/* 志高帧格式变体 */
typedef enum {
    AC_CHIGO_FMT_LEGACY = 0,    /* CS-21H3A-B155AF：sum 校验和 */
    AC_CHIGO_FMT_KRF51G,        /* KRF-51G_79F：互补冗余 + XOR 校验 */
} ac_chigo_fmt_t;

/* 编码输出缓冲上限（美的 199 / 志高 197 段） */
#define AC_CODEC_MAX_EDGES      256

/* ========== API ========== */

/**
 * @brief  将空调状态编码为原始时序（us 数组）
 * @param  brand           品牌
 * @param  state           空调状态
 * @param  timing_out      输出缓冲
 * @param  max_edges       缓冲容量（建议 >= AC_CODEC_MAX_EDGES）
 * @param  level_start_out 输出起始电平（恒为 0：第一段为载波 mark）
 * @retval 时序段数；0 = 失败（参数错误/缓冲不足）
 */
uint16_t AC_Codec_Encode(ac_brand_t brand, const ac_state_t *state,
                         uint16_t *timing_out, uint16_t max_edges,
                         uint8_t *level_start_out);

/* ---------- 分品牌接口（需单独控制时使用） ---------- */

/** 美的：48bit 状态帧 + 反相帧 */
uint16_t AC_Codec_EncodeMidea(const ac_state_t *state,
                              uint16_t *timing_out, uint16_t max_edges);

/** 志高：96bit 状态帧，可选帧格式 */
uint16_t AC_Codec_EncodeChigo(const ac_state_t *state, ac_chigo_fmt_t fmt,
                              uint16_t *timing_out, uint16_t max_edges);

/* ========== 单元测试钩子（仅定义 AC_CODEC_UNIT_TEST 时编译） ========== */
#ifdef AC_CODEC_UNIT_TEST
uint8_t AC_Codec_MideaChecksum(const uint8_t bytes5to1[5]);
uint16_t AC_Codec_ChigoBuildBits(const ac_state_t *state, ac_chigo_fmt_t fmt,
                                 uint8_t bits_out[96]);
#endif

#ifdef __cplusplus
}
#endif

#endif /* __AC_CODEC_H */
