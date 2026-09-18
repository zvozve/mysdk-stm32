/**
 * @file    led_matrix.c
 * @brief   64x64 RGB LED 点阵屏驱动（ICN2027 列驱动 + TC7258 行译码 + SM245 OE）
 *          板无关：所有控制引脚由 LED_Matrix_Init() 注入，不引用任何 CubeMX / HAL 符号。
 *          依赖 chip 层 oop_gpio（GPIO 抽象）与 oop_dwt（延时）。
 *          自带字模注册 + 文本/位图渲染，用户填字模即可开箱用，不依赖任何外部 gfx 模块。
 * @version V1.0
 * @date    2026-09-18
 */

#include "led_matrix.h"
#include "oop_gpio_drv.h"
#include "oop_dwt.h"
#include <string.h>

/* ---------- 内部状态 ---------- */
static const led_matrix_pins_t *g_pins = NULL;

static gpio_dev_t d_sm245_oe1, d_sm245_oe2;
static gpio_dev_t d_icn_lat, d_icn_sclk, d_icn_oe;
static gpio_dev_t d_r1, d_g1, d_b1, d_r2, d_g2, d_b2;
static gpio_dev_t d_tc_a, d_tc_b, d_tc_c, d_tc_d, d_tc_e;

/* 帧缓冲：on/off 按位打包，64 行 × 8 字节(=64 列) */
static uint8_t lm_frame[LM_H][LM_W / 8];
/* 颜色缓冲：每像素一字节 */
static uint8_t lm_color[LM_H][LM_W];

static uint8_t g_scan_line = 0;
static const lm_font_t *lm_fonts[LM_FONT_MAX];

/* ---------- 底层 GPIO 封装 ---------- */
static void lm_w(gpio_dev_t *d, int hi) { oop_gpio_write(d, hi ? 1 : 0); }

static void select_line(uint8_t line) {
    lm_w(&d_tc_a, line & 0x01);
    lm_w(&d_tc_b, line & 0x02);
    lm_w(&d_tc_c, line & 0x04);
    lm_w(&d_tc_d, line & 0x08);
    lm_w(&d_tc_e, line & 0x10);
}

static void lm_dot_write(uint8_t pos) {
    if (pos == LM_TOP) { lm_w(&d_r1,1); lm_w(&d_g1,1); lm_w(&d_b1,1); }
    else              { lm_w(&d_r2,1); lm_w(&d_g2,1); lm_w(&d_b2,1); }
}
static void lm_dot_off(uint8_t pos) {
    if (pos == LM_TOP) { lm_w(&d_r1,0); lm_w(&d_g1,0); lm_w(&d_b1,0); }
    else              { lm_w(&d_r2,0); lm_w(&d_g2,0); lm_w(&d_b2,0); }
}
static void lm_dot_red(uint8_t pos) {
    if (pos == LM_TOP) { lm_w(&d_r1,1); lm_w(&d_g1,0); lm_w(&d_b1,0); }
    else              { lm_w(&d_r2,1); lm_w(&d_g2,0); lm_w(&d_b2,0); }
}
static void lm_dot_green(uint8_t pos) {
    if (pos == LM_TOP) { lm_w(&d_r1,0); lm_w(&d_g1,1); lm_w(&d_b1,0); }
    else              { lm_w(&d_r2,0); lm_w(&d_g2,1); lm_w(&d_b2,0); }
}
static void lm_dot_blue(uint8_t pos) {
    if (pos == LM_TOP) { lm_w(&d_r1,0); lm_w(&d_g1,0); lm_w(&d_b1,1); }
    else              { lm_w(&d_r2,0); lm_w(&d_g2,0); lm_w(&d_b2,1); }
}
static void lm_dot_yellow(uint8_t pos) {
    if (pos == LM_TOP) { lm_w(&d_r1,1); lm_w(&d_g1,1); lm_w(&d_b1,0); }
    else              { lm_w(&d_r2,1); lm_w(&d_g2,1); lm_w(&d_b2,0); }
}

/* ICN2027 OE 低有效：OFF=高电平禁输出，ON=低电平使能 */
static void lm_icn_oe_off(void) { lm_w(&d_icn_oe, 1); }
static void lm_icn_oe_on(void)  { lm_w(&d_icn_oe, 0); }

/* ICN2027 移位输出一行：同时送出上半屏(line)与下半屏(line+32) */
static void Write_ICN2027(const uint8_t *DataBuf_Top, const uint8_t *DataBuf_Bot,
                          const uint8_t *ColorBuf_Top, const uint8_t *ColorBuf_Bot) {
    uint8_t i, j, data_top, data_bot, color_top, color_bot;
    for (i = 0; i < 8; i++) {
        data_top = DataBuf_Top[i];
        data_bot = DataBuf_Bot[i];
        for (j = 0; j < 8; j++) {
            color_top = ColorBuf_Top[i * 8 + j];
            color_bot = ColorBuf_Bot[i * 8 + j];
            if (data_top & 0x80) {
                switch (color_top) {
                    case LM_WHITE:  lm_dot_write(LM_TOP);  break;
                    case LM_RED:    lm_dot_red(LM_TOP);    break;
                    case LM_GREEN:  lm_dot_green(LM_TOP);  break;
                    case LM_BLUE:   lm_dot_blue(LM_TOP);   break;
                    case LM_YELLOW: lm_dot_yellow(LM_TOP); break;
                }
            } else {
                lm_dot_off(LM_TOP);
            }
            if (data_bot & 0x80) {
                switch (color_bot) {
                    case LM_WHITE:  lm_dot_write(LM_BOTTOM);  break;
                    case LM_RED:    lm_dot_red(LM_BOTTOM);    break;
                    case LM_GREEN:  lm_dot_green(LM_BOTTOM);  break;
                    case LM_BLUE:   lm_dot_blue(LM_BOTTOM);   break;
                    case LM_YELLOW: lm_dot_yellow(LM_BOTTOM); break;
                }
            } else {
                lm_dot_off(LM_BOTTOM);
            }
            lm_w(&d_icn_sclk, 1);
            lm_w(&d_icn_sclk, 0);
            data_top <<= 1;
            data_bot <<= 1;
        }
    }
    lm_w(&d_icn_lat, 1);
    lm_w(&d_icn_lat, 0);
}

static void lm_draw_point(uint8_t x, uint8_t y, uint8_t on, uint8_t color) {
    if (x >= LM_W || y >= LM_H) return;
    if (!on) {
        lm_frame[y][x / 8] &= ~(1 << (7 - (x % 8)));
        lm_color[y][x] = LM_WHITE;     /* 背景色 */
    } else {
        lm_frame[y][x / 8] |= (1 << (7 - (x % 8)));
        lm_color[y][x] = color;
    }
}

/* ---------- 公共 API ---------- */
void LED_Matrix_Init(const led_matrix_pins_t *pins) {
    g_pins = pins;
    oop_gpio_init_output(&d_sm245_oe1, pins->sm245_oe1.port, pins->sm245_oe1.pin, true);
    oop_gpio_init_output(&d_sm245_oe2, pins->sm245_oe2.port, pins->sm245_oe2.pin, true);
    oop_gpio_init_output(&d_icn_lat, pins->icn2027_lat.port, pins->icn2027_lat.pin, true);
    oop_gpio_init_output(&d_icn_sclk, pins->icn2027_sclk.port, pins->icn2027_sclk.pin, true);
    oop_gpio_init_output(&d_icn_oe, pins->icn2027_oe.port, pins->icn2027_oe.pin, true);
    oop_gpio_init_output(&d_r1, pins->r1.port, pins->r1.pin, true);
    oop_gpio_init_output(&d_g1, pins->g1.port, pins->g1.pin, true);
    oop_gpio_init_output(&d_b1, pins->b1.port, pins->b1.pin, true);
    oop_gpio_init_output(&d_r2, pins->r2.port, pins->r2.pin, true);
    oop_gpio_init_output(&d_g2, pins->g2.port, pins->g2.pin, true);
    oop_gpio_init_output(&d_b2, pins->b2.port, pins->b2.pin, true);
    oop_gpio_init_output(&d_tc_a, pins->tc_a.port, pins->tc_a.pin, true);
    oop_gpio_init_output(&d_tc_b, pins->tc_b.port, pins->tc_b.pin, true);
    oop_gpio_init_output(&d_tc_c, pins->tc_c.port, pins->tc_c.pin, true);
    oop_gpio_init_output(&d_tc_d, pins->tc_d.port, pins->tc_d.pin, true);
    oop_gpio_init_output(&d_tc_e, pins->tc_e.port, pins->tc_e.pin, true);
    /* SM245 OE 保持使能（低有效），ICN2027 OE 由扫描逻辑控制 */
    lm_w(&d_sm245_oe1, 0);
    lm_w(&d_sm245_oe2, 0);
    /* 初始全灭 */
    memset(lm_frame, 0x00, sizeof(lm_frame));
    memset(lm_color, LM_WHITE, sizeof(lm_color));
}

void LED_Matrix_Clear(lm_color_t color) {
    memset(lm_color, color, sizeof(lm_color));
    memset(lm_frame, 0xff, sizeof(lm_frame));   /* 全点亮，填充 color */
}

void LED_Matrix_SetPixel(uint8_t x, uint8_t y, lm_color_t color) {
    lm_draw_point(x, y, 1, color);
}

static void lm_scan_line(uint8_t line) {
    Write_ICN2027(lm_frame[line], lm_frame[32 + line],
                  lm_color[line], lm_color[32 + line]);
    lm_icn_oe_off();
    select_line(line);
    lm_icn_oe_on();
}

void LED_Matrix_ScanTick(void) {
    lm_scan_line(g_scan_line);
    g_scan_line = (g_scan_line + 1) % 32;
}

void LED_Matrix_Refresh(void) {
    for (uint8_t i = 0; i < 32; i++) {
        LED_Matrix_ScanTick();
        oop_DelayUS(LM_SCAN_LINE_US);
    }
}

/* ---------- 字模注册 + 文本/位图渲染 ---------- */
void LED_Matrix_RegisterFont(uint8_t id, const lm_font_t *font) {
    if (id < LM_FONT_MAX) lm_fonts[id] = font;
}

/* 通用单色字形 blit：按 cell_w × cell_h、bytes_per_row 取模渲染 */
static void lm_blit(int x, int y, const uint8_t *glyph, uint8_t w, uint8_t h,
                    uint8_t bpr, lm_color_t color) {
    (void)w;
    for (uint8_t row = 0; row < h; row++) {
        for (uint8_t b = 0; b < bpr; b++) {
            uint8_t byte = glyph[row * bpr + b];
            for (uint8_t bit = 0; bit < 8; bit++) {
                if (byte & (0x80 >> bit)) {
                    LED_Matrix_SetPixel((uint8_t)(x + b * 8 + bit), (uint8_t)(y + row), color);
                }
            }
        }
    }
}

void LED_Matrix_DrawChar(int x, int y, char c, uint8_t font_id, lm_color_t color) {
    const lm_font_t *f = (font_id < LM_FONT_MAX) ? lm_fonts[font_id] : NULL;
    if (!f) return;
    uint8_t idx = (uint8_t)c - f->first;
    if ((uint8_t)c < f->first || (uint8_t)c > f->last) return;
    uint32_t base = (uint32_t)idx * f->cell_h * f->bytes_per_row;
    lm_blit(x, y, f->table + base, f->cell_w, f->cell_h, f->bytes_per_row, color);
}

void LED_Matrix_DrawString(int x, int y, const char *s, uint8_t font_id, lm_color_t color) {
    const lm_font_t *f = (font_id < LM_FONT_MAX) ? lm_fonts[font_id] : NULL;
    if (!f || !s) return;
    int cx = x;
    while (*s) {
        if (f->encoding == LM_FONT_GBK && (uint8_t)*s >= 0x80) {
            uint32_t off = f->offset_of ? f->offset_of((uint8_t)s[0], (uint8_t)s[1]) : 0xFFFFFFFFu;
            if (off != 0xFFFFFFFFu) {
                lm_blit(cx, y, f->table + off, f->cell_w, f->cell_h, f->bytes_per_row, color);
            }
            s += 2; cx += f->cell_w;
        } else {
            LED_Matrix_DrawChar(cx, y, *s, font_id, color);
            s++; cx += f->cell_w;
        }
    }
}

void LED_Matrix_DrawBitmap(int x, int y, int w, int h, const uint8_t *bmp, lm_color_t color_on) {
    if (!bmp) return;
    int row_bytes = (w + 7) / 8;
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            int byte_idx = row * row_bytes + col / 8;
            int bit = 7 - (col % 8);
            if (bmp[byte_idx] & (1 << bit)) {
                LED_Matrix_SetPixel((uint8_t)(x + col), (uint8_t)(y + row), color_on);
            }
        }
    }
}
