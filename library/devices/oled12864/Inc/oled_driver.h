/**
 * @file    oled_driver.h
 * @brief   SSD1306 OLED（128x64，I2C）显示驱动，含自包含字模注册 + 文本/位图渲染
 * @version V1.1
 * @date    2026-09-18
 *
 * @note    板无关：I2C 引脚由 OLED_I2C_Init() 注入，不引用任何 CubeMX 符号。
 *          调用顺序：先 OLED_I2C_Init() 绑定引脚，再 OLED_12832_Init()/OLED_12864_Init() 初始化显示。
 *          依赖 chip 层 oop_i2c_drv（软件 I2C）。
 */

#ifndef __OLED_DRIVER_H__
#define __OLED_DRIVER_H__

#include <stdint.h>
#include "oop_i2c_drv.h"   /* 提供 GPIO_TypeDef 与 soft_i2c_t */

#define OLED_I2C_WR      0       /* 写控制bit */
#define OLED_I2C_RD      1       /* 读控制bit */
#define OLED_ADDRESS      0x78    /* 默认地址（可通过0R电阻改为0x7A） */
#define OLED_USE_OSD_PWR  0       /* 1: 外部电源  0: 内部升压 */

#define OLED_PIX_ON       0xFF
#define OLED_PIX_OFF      0x00

#define OLED_PAGE_MAX     8
#define OLED_COLUMN_MAX   128

extern uint8_t oled_page_buf[OLED_PAGE_MAX][OLED_COLUMN_MAX];
extern uint8_t oled_page_last[OLED_PAGE_MAX][OLED_COLUMN_MAX];

/* 注入 I2C 引脚（板无关），必须在显示初始化前调用 */
void OLED_I2C_Init(GPIO_TypeDef* scl_port, uint16_t scl_pin,
                   GPIO_TypeDef* sda_port, uint16_t sda_pin, uint32_t delay_us);

void I2C_LCD_WriteByte(uint8_t addr, uint8_t data);
void I2C_LCD_WriteCmd(unsigned char I2C_Command);
void I2C_LCD_WriteDat(unsigned char I2C_Data);

void OLED_Buf_Clear(void);
void OLED_WritePage(uint8_t page, const uint8_t *data_buf);
void OLED_WritePagePartial(uint8_t page, uint8_t col_start, uint8_t col_end, const uint8_t *data_buf);
void OLED_Address(uint8_t page, uint8_t column);
uint8_t OLED_CheckDevice(uint8_t _Address);

void OLED_ON(void);
void OLED_OFF(void);
void OLED_CLS(void);
void OLED_Fill(unsigned char fill_Data);
void OLED_Refresh(void);
void OLED_RefreshDiff(void);

void OLED_SetPos(unsigned char x, unsigned char y);

/* ---------- 字模注册 + 文本/位图渲染（自包含，不依赖外部 gfx 层） ---------- */
typedef enum { OLED_FONT_ASCII = 0, OLED_FONT_GBK } oled_font_enc_t;

typedef struct {
    const uint8_t *table;       /* 字模首地址（asc2_1608 / zhcn_2020 ...） */
    uint8_t  cell_w, cell_h;    /* 单字宽高（"字符大小"由这里定义，不写死函数名） */
    uint8_t  bytes_per_row;     /* 每行字节数 = ceil(cell_w/8) */
    uint8_t  first, last;       /* 首/末字符编码（ASCII） */
    oled_font_enc_t enc;        /* ASCII / GBK */
    uint32_t (*offset_of)(uint8_t hi, uint8_t lo); /* GBK 查表（可选） */
} oled_font_t;

void OLED_SetPixel(int x, int y, uint8_t on);                 /* 写 oled_page_buf 像素位 */
void OLED_RegisterFont(uint8_t id, const oled_font_t *font);  /* 注入用户字模 */
void OLED_DrawChar(int x, int y, char c, uint8_t font_id, uint8_t inverse);
void OLED_DrawString(int x, int y, const char *s, uint8_t font_id, uint8_t inverse);
void OLED_DrawBitmap(int x, int y, int w, int h, const uint8_t *bmp, uint8_t inverse);

void OLED_12832_Init(void);
void OLED_12864_Init(void);
uint8_t OLED_Test(void);

#endif /* __OLED_DRIVER_H__ */
