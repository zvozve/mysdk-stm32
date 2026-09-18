#ifndef __LED_MATRIX_H__
#define __LED_MATRIX_H__

#include <stdint.h>
#include "oop_gpio_drv.h"   /* 提供 GPIO_TypeDef / gpio_dev_t，引脚注入用 */

#define LM_W        64
#define LM_H        64
#define LM_TOP      0
#define LM_BOTTOM   1

typedef enum {
    LM_WHITE = 0, LM_RED, LM_GREEN, LM_BLUE, LM_YELLOW
} lm_color_t;

/* 单引脚描述（板无关，由工程 board_cfg 注入） */
typedef struct {
    GPIO_TypeDef *port;
    uint16_t     pin;
} lm_pin_t;

/* 全控制线：ICN2027(6数据+SCLK/LAT/OE) + TC7258(5行选) + SM245(2 OE) */
typedef struct {
    lm_pin_t sm245_oe1;     /* SM245 输出使能（参考工程原 F0） */
    lm_pin_t sm245_oe2;     /* SM245 输出使能（参考工程原 F11） */
    lm_pin_t icn2027_lat;   /* ICN2027 锁存 LAT */
    lm_pin_t icn2027_sclk;  /* ICN2027 移位时钟 SCLK */
    lm_pin_t icn2027_oe;    /* ICN2027 输出使能 OE（逐行扫描时翻转） */
    lm_pin_t r1, g1, b1;    /* 上半屏 RGB 数据线 */
    lm_pin_t r2, g2, b2;    /* 下半屏 RGB 数据线 */
    lm_pin_t tc_a, tc_b, tc_c, tc_d, tc_e; /* TC7258 行译码 A~E */
} led_matrix_pins_t;

/* 扫描逐行停留时间(us)，决定刷新率：整屏周期 ≈ 32 行 × 该值。
   520us ≈ 60Hz（与参考工程一致）；要更亮/更稳可减小。 */
#ifndef LM_SCAN_LINE_US
#define LM_SCAN_LINE_US  520
#endif

/* ---------- 字模描述符：用户按此格式填入自己的字模即可（"指定格式"） ---------- */
typedef enum { LM_FONT_ASCII = 0, LM_FONT_GBK } lm_font_enc_t;

typedef struct {
    const uint8_t *table;        /* 字模数组首地址（如 asc2_1608 / zhcn_2020） */
    uint8_t  cell_w;             /* 单字宽(px) */
    uint8_t  cell_h;             /* 单字高(px) */
    uint8_t  bytes_per_row;      /* 每行字节数（宽字用，如 16px 宽=2） */
    uint8_t  first;              /* 首字符编码（ASCII 空格=0x20） */
    uint8_t  last;               /* 末字符编码 */
    lm_font_enc_t encoding;      /* ASCII / GBK */
    uint32_t (*offset_of)(uint8_t hi, uint8_t lo); /* GBK->字模偏移(可选) */
} lm_font_t;

#define LM_FONT_MAX  4

/* ---------- 扫描 / 像素 API ---------- */
void LED_Matrix_Init(const led_matrix_pins_t *pins);
void LED_Matrix_SetPixel(uint8_t x, uint8_t y, lm_color_t color);
void LED_Matrix_Clear(lm_color_t color);
void LED_Matrix_ScanTick(void);   /* 定时器/任务周期调用：推进一行 */
void LED_Matrix_Refresh(void);    /* 阻塞整屏扫描一遍（无定时器/测试用） */

/* ---------- 字模注册 + 文本/位图渲染（自包含，直接操作本驱动帧缓冲） ---------- */
void LED_Matrix_RegisterFont(uint8_t id, const lm_font_t *font);
void LED_Matrix_DrawChar(int x, int y, char c, uint8_t font_id, lm_color_t color);
void LED_Matrix_DrawString(int x, int y, const char *s, uint8_t font_id, lm_color_t color);
void LED_Matrix_DrawBitmap(int x, int y, int w, int h, const uint8_t *bmp, lm_color_t color_on);

#endif /* __LED_MATRIX_H__ */
