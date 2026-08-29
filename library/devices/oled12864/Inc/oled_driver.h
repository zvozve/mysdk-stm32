/**
 * @file    oled_driver.h
 * @brief   SSD1306 OLED（128x64，I2C）显示驱动（仅驱动层；UI 见 oled_func/oled_ui，不入库）
 * @version V1.0
 * @date    2026-08-29
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
void OLED_ShowStr(unsigned char x, unsigned char y, unsigned char ch[], unsigned char TextSize);
void OLED_ShowCN(unsigned char x, unsigned char y, unsigned char N);
void OLED_DrawBMP(unsigned char x0, unsigned char y0, unsigned char x1, unsigned char y1, unsigned char BMP[]);

void OLED_12832_Init(void);
void OLED_12864_Init(void);
uint8_t OLED_Test(void);

#endif /* __OLED_DRIVER_H__ */
