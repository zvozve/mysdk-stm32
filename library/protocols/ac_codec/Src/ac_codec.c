/**
 * @file    ac_codec.c
 * @brief   空调红外协议编码器实现（纯 C，无平台依赖，可在主机侧单测）
 * @version V1.0
 * @date    2026-08-27
 *
 * 输出为“载波时长数组”，与 ir_transmitter_send() 约定一致：
 *   数组元素依次为 [mark, space, mark, space, ...]（单位 us），
 *   调用方传 level_start=0（第一段点亮红外管）。
 */

#include "ac_codec.h"
#include <string.h>

/* =====================================================================
 * 通用工具
 * ===================================================================== */

/* 字节位反转（Midea 校验用） */
static uint8_t rev8(uint8_t x)
{
    uint8_t r = 0;
    for (int i = 0; i < 8; i++) {
        if (x & (uint8_t)(1u << i)) {
            r |= (uint8_t)(1u << (7 - i));
        }
    }
    return r;
}

static uint8_t clamp_u8(uint8_t v, uint8_t lo, uint8_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* =====================================================================
 * 美的 (Midea) 协议
 * 参考 IRremoteESP8266 ir_Midea.cpp。48bit 状态 + 全反相校验帧。
 * 时序（tick=80us）：头 4480/4480，位 560 + (0:560 / 1:1680)，
 * 收尾 560，帧间隔 5600。字节发送顺序 byte5(MSB)→byte0，字节内 MSB 优先。
 * ===================================================================== */

#define MIDEA_HDR_MARK      4480
#define MIDEA_HDR_SPACE     4480
#define MIDEA_BIT_MARK      560
#define MIDEA_ONE_SPACE     1680
#define MIDEA_ZERO_SPACE    560
#define MIDEA_FOOT_MARK     560
#define MIDEA_GAP           5600

#define MIDEA_MODE_COOL     0
#define MIDEA_MODE_DRY      1
#define MIDEA_MODE_AUTO     2
#define MIDEA_MODE_HEAT     3
#define MIDEA_MODE_FAN      4

#define MIDEA_FAN_AUTO      0
#define MIDEA_FAN_LOW       1
#define MIDEA_FAN_MED       2
#define MIDEA_FAN_HIGH      3

/* 由 5 个数据字节（byte5..byte1）计算校验字节 */
#ifdef AC_CODEC_UNIT_TEST
uint8_t AC_Codec_MideaChecksum(const uint8_t bytes5to1[5])
#else
static uint8_t midea_checksum(const uint8_t bytes5to1[5])
#endif
{
    uint8_t sum = 0;
    for (int i = 0; i < 5; i++) {
        sum = (uint8_t)(sum + rev8(bytes5to1[i]));
    }
    sum = (uint8_t)(256 - sum);
    return rev8(sum);
}

/* 将通用状态组装成美的 6 字节（bytes[0]=校验 … bytes[5]=类型头） */
static void midea_build(const ac_state_t *st, uint8_t bytes[6])
{
    uint8_t mode;
    switch (st->mode) {
        case AC_MODE_COOL: mode = MIDEA_MODE_COOL; break;
        case AC_MODE_DRY:  mode = MIDEA_MODE_DRY;  break;
        case AC_MODE_HEAT: mode = MIDEA_MODE_HEAT; break;
        case AC_MODE_FAN:  mode = MIDEA_MODE_FAN;  break;
        case AC_MODE_AUTO:
        default:           mode = MIDEA_MODE_AUTO; break;
    }

    uint8_t fan;
    switch (st->fan) {
        case AC_FAN_LOW:  fan = MIDEA_FAN_LOW;  break;
        case AC_FAN_MID:  fan = MIDEA_FAN_MED;  break;
        case AC_FAN_HIGH: fan = MIDEA_FAN_HIGH; break;
        case AC_FAN_AUTO:
        default:          fan = MIDEA_FAN_AUTO; break;
    }

    uint8_t temp = clamp_u8(st->temp_c, 17, 30);   /* 17~30 ℃ */

    bytes[5] = (uint8_t)0xA1;                       /* Type=Command, Header=0b10100 */
    bytes[4] = (uint8_t)((st->power ? 0x80 : 0x00) |
                         (uint8_t)(fan << 3) | mode);
    bytes[3] = (uint8_t)(temp - 17);                /* 摄氏度，bit5=0 */
    bytes[2] = (uint8_t)0xFF;                       /* OffTimer 关闭 */
    bytes[1] = (uint8_t)0xFF;                       /* 传感器温度禁用 */

    uint8_t data5[5] = { bytes[5], bytes[4], bytes[3], bytes[2], bytes[1] };
#ifdef AC_CODEC_UNIT_TEST
    bytes[0] = AC_Codec_MideaChecksum(data5);
#else
    bytes[0] = midea_checksum(data5);
#endif
}

/* 发射单个 48bit 相位（可选反相），返回写入的时序段数 */
static uint16_t midea_emit_phase(const uint8_t bytes[6], bool invert,
                                 uint16_t *out, uint16_t max_edges)
{
    const uint16_t need = 2 + 48 * 2 + 1;   /* 头 + 48位 + 收尾mark */
    if (max_edges < need) return 0;

    uint16_t n = 0;
    out[n++] = MIDEA_HDR_MARK;
    out[n++] = MIDEA_HDR_SPACE;

    for (int bi = 5; bi >= 0; bi--) {         /* byte5(MSB) → byte0 */
        uint8_t b = invert ? (uint8_t)~bytes[bi] : bytes[bi];
        for (int bit = 7; bit >= 0; bit--) {  /* 字节内 MSB 优先 */
            out[n++] = MIDEA_BIT_MARK;
            out[n++] = ((b >> bit) & 1) ? MIDEA_ONE_SPACE : MIDEA_ZERO_SPACE;
        }
    }
    out[n++] = MIDEA_FOOT_MARK;
    return n;
}

uint16_t AC_Codec_EncodeMidea(const ac_state_t *state,
                              uint16_t *timing_out, uint16_t max_edges)
{
    if (state == NULL || timing_out == NULL) return 0;

    uint8_t bytes[6];
    midea_build(state, bytes);

    uint16_t n = midea_emit_phase(bytes, false, timing_out, max_edges);
    if (n == 0) return 0;

    if (max_edges - n < 1) return 0;
    timing_out[n++] = MIDEA_GAP;              /* 正相与反相之间 */

    uint16_t n2 = midea_emit_phase(bytes, true, timing_out + n, max_edges - n);
    if (n2 == 0) return 0;
    return (uint16_t)(n + n2);
}

/* =====================================================================
 * 志高 (Chigo) 协议 —— LEGACY 格式（CS-21H3A-B155AF）
 * 96bit = 12 字节，byte0 先发、字节内 LSB 优先。
 * 头 6130/7360，位 550 + (0:560 / 1:1650)，尾 545/7380/545。
 *   byte7  = 0x18 | 电源(bit1) | 风速(bit5,6)
 *   byte9  = 模式(高4位, =mode_code*2) | (温度-16)(低4位)
 *   byte3  = 强力时 0x08；byte5 = 强力时 0x0A
 *   byte10 = 0xA5 固定；byte11 = sum(byte0..10) & 0xFF
 * ===================================================================== */

#define CHIGO_HDR_MARK      6130
#define CHIGO_HDR_SPACE     7360
#define CHIGO_BIT_MARK      550
#define CHIGO_ZERO_SPACE    560
#define CHIGO_ONE_SPACE     1650
#define CHIGO_FOOT_MARK     545
#define CHIGO_FOOT_SPACE    7380
#define CHIGO_TRAIL_MARK    545

#define CHIGO_BYTES         12
#define CHIGO_BITS          (CHIGO_BYTES * 8)

/* 风速 → byte7 的 bit5/bit6 掩码：Auto=00 High=10 Mid=01 Low=11 */
static uint8_t chigo_fan_mask(ac_fan_t fan)
{
    switch (fan) {
        case AC_FAN_LOW:  return (uint8_t)0x60;   /* bit5+bit6 */
        case AC_FAN_MID:  return (uint8_t)0x40;   /* bit6      */
        case AC_FAN_HIGH: return (uint8_t)0x20;   /* bit5      */
        case AC_FAN_AUTO:
        default:          return (uint8_t)0x00;
    }
}

/* 模式 → mode_code：Auto=0 Cool=1 Dry=2 Fan=3 Heat=4（byte9 高4位 = code*2） */
static uint8_t chigo_mode_code(ac_mode_t mode)
{
    switch (mode) {
        case AC_MODE_COOL: return 1;
        case AC_MODE_DRY:  return 2;
        case AC_MODE_FAN:  return 3;
        case AC_MODE_HEAT: return 4;
        case AC_MODE_AUTO:
        default:           return 0;
    }
}

/* 组装 12 字节，返回字节数；bits_out 可选（单测用） */
static uint16_t chigo_build_bytes(const ac_state_t *st, uint8_t bytes[CHIGO_BYTES])
{
    memset(bytes, 0, CHIGO_BYTES);

    uint8_t temp = clamp_u8(st->temp_c, 16, 31);   /* 16~31 ℃，4bit */

    /* byte7：基础 0x18 + 电源 + 风速 */
    bytes[7] = (uint8_t)(0x18 | (st->power ? 0x02 : 0x00) | chigo_fan_mask(st->fan));

    /* byte9：模式(高4位) | 温度偏移(低4位) */
    bytes[9] = (uint8_t)((uint8_t)(chigo_mode_code(st->mode) * 2u << 4) |
                         (uint8_t)(temp - 16));

    /* 强力 */
    if (st->turbo) {
        bytes[3] = 0x08;
        bytes[5] = 0x0A;
    }

    bytes[10] = 0xA5;

    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) sum = (uint8_t)(sum + bytes[i]);
    bytes[11] = sum;

    return CHIGO_BYTES;
}

#ifdef AC_CODEC_UNIT_TEST
/* 将 12 字节按“byte0 先发、LSB 优先”展开为 96 个 bit（单测用） */
uint16_t AC_Codec_ChigoBuildBits(const ac_state_t *state, ac_chigo_fmt_t fmt,
                                 uint8_t bits_out[96])
{
    (void)fmt;
    uint8_t bytes[CHIGO_BYTES];
    chigo_build_bytes(state, bytes);
    uint16_t n = 0;
    for (int bi = 0; bi < CHIGO_BYTES; bi++) {
        for (int bit = 0; bit < 8; bit++) {       /* LSB 优先 */
            bits_out[n++] = (bytes[bi] >> bit) & 1;
        }
    }
    return n;
}
#endif

uint16_t AC_Codec_EncodeChigo(const ac_state_t *state, ac_chigo_fmt_t fmt,
                              uint16_t *timing_out, uint16_t max_edges)
{
    (void)fmt;   /* 当前仅实现 LEGACY；KRF51G 变体待有更多数据后补充 */
    if (state == NULL || timing_out == NULL) return 0;

    const uint16_t need = 2 + CHIGO_BITS * 2 + 3;  /* 头 + 96位 + 尾3段 */
    if (max_edges < need) return 0;

    uint8_t bytes[CHIGO_BYTES];
    chigo_build_bytes(state, bytes);

    uint16_t n = 0;
    timing_out[n++] = CHIGO_HDR_MARK;
    timing_out[n++] = CHIGO_HDR_SPACE;

    for (int bi = 0; bi < CHIGO_BYTES; bi++) {     /* byte0 先发 */
        for (int bit = 0; bit < 8; bit++) {        /* LSB 优先 */
            timing_out[n++] = CHIGO_BIT_MARK;
            timing_out[n++] = ((bytes[bi] >> bit) & 1)
                              ? CHIGO_ONE_SPACE : CHIGO_ZERO_SPACE;
        }
    }

    timing_out[n++] = CHIGO_FOOT_MARK;
    timing_out[n++] = CHIGO_FOOT_SPACE;
    timing_out[n++] = CHIGO_TRAIL_MARK;
    return n;
}

/* =====================================================================
 * 统一入口
 * ===================================================================== */

uint16_t AC_Codec_Encode(ac_brand_t brand, const ac_state_t *state,
                         uint16_t *timing_out, uint16_t max_edges,
                         uint8_t *level_start_out)
{
    if (state == NULL || timing_out == NULL || max_edges == 0) return 0;
    if (level_start_out != NULL) *level_start_out = 0;   /* 第一段为载波 mark */

    switch (brand) {
        case AC_BRAND_MIDEA:
            return AC_Codec_EncodeMidea(state, timing_out, max_edges);
        case AC_BRAND_CHIGO:
            return AC_Codec_EncodeChigo(state, AC_CHIGO_FMT_LEGACY,
                                        timing_out, max_edges);
        default:
            return 0;
    }
}
