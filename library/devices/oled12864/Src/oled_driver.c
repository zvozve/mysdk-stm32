#include "oled_driver.h"
#include "oop_i2c_drv.h"
#include "oop_dwt.h"
#include <string.h>

/**
******************************************************************************
* @file    12864_SSD1306_IIC.c
* @author  fire
* @version V1.0
* @date    2014-xx-xx
* @brief   128*64点阵的OLED显示屏驱动文件，仅适用于SD1306驱动IIC通信方式显示屏
******************************************************************************

* Function List:
* 1. uint8_t OLED_CheckDevice(uint8_t _Address) -- 检测I2C总线设备OLED
* 2. void I2C_LCD_WriteByte(uint8_t addr,uint8_t data) -- 向寄存器地址写一个byte的数据
* 3. void I2C_LCD_WriteCmd(unsigned char I2C_Command) -- 写命令
* 4. void I2C_LCD_WriteDat(unsigned char I2C_Data) -- 写数据
* 5. void OLED_Init(void) -- OLED屏初始化
* 6. void OLED_SetPos(unsigned char x, unsigned char y) -- 设置起始点坐标
* 7. void OLED_Fill(unsigned char fill_Data) -- 全屏填充
* 8. void OLED_CLS(void) -- 清屏
* 9. void OLED_ON(void) -- 唤醒
* 10. void OLED_OFF(void) -- 睡眠
* 11. void OLED_ShowStr(unsigned char x, unsigned char y, unsigned char ch[], unsigned char TextSize) -- 显示字符串(字体大小有6*8和8*16两种)
* 12. void OLED_ShowCN(unsigned char x, unsigned char y, unsigned char N) -- 显示中文(中文需要先取模，然后放到codetab.h中)
* 13. void OLED_DrawBMP(unsigned char x0,unsigned char y0,unsigned char x1,unsigned char y1,unsigned char BMP[]) -- BMP图片
* 14. uint8_t OLED_Test(void) --OLED检测测试
*
******************************************************************************
*/ 

static soft_i2c_t g_oled_i2c;

uint8_t oled_page_buf[OLED_PAGE_MAX][OLED_COLUMN_MAX] = {0};        // 当前显示内容
uint8_t oled_page_last[OLED_PAGE_MAX][OLED_COLUMN_MAX] = {0};       // 上次发送内容

/**
* @brief  I2C_LCD_WriteByte，向OLED寄存器地址写一个byte的数据
* @param  addr：寄存器地址
*					data：要写入的数据
* @retval 无
*/
void I2C_LCD_WriteByte(uint8_t addr,uint8_t data) {
	oop_i2c_start(&g_oled_i2c);//开启I2C总线
	
	/* 发送设备地址+读写控制bit（0 = w， 1 = r) bit7 先传 */
	oop_i2c_send_byte(&g_oled_i2c, OLED_ADDRESS|OLED_I2C_WR);
	
	/*等待ACK */
	if (oop_i2c_wait_ack(&g_oled_i2c) != 0)
	{
		goto cmd_fail;	/* OLED器件无应答 */
	}
		
	oop_i2c_send_byte(&g_oled_i2c, addr);//发送寄存器地址
	
	/*等待ACK */
	if (oop_i2c_wait_ack(&g_oled_i2c) != 0)
	{
		goto cmd_fail;	/* OLED器件无应答 */
	}

	oop_i2c_send_byte(&g_oled_i2c, data);//发送数据
	
	/*等待ACK */
	if (oop_i2c_wait_ack(&g_oled_i2c) != 0)
	{
		goto cmd_fail;	/* OLED器件无应答 */
	}	
	
 /* 发送I2C总线停止信号 */
	oop_i2c_stop(&g_oled_i2c);

cmd_fail: /* 命令执行失败后，切记发送停止信号，避免影响I2C总线上其他设备 */
	/* 发送I2C总线停止信号 */
	oop_i2c_stop(&g_oled_i2c);

}

/**
* @brief  I2C_LCD_WriteCmd，向OLED写入命令
* @param  I2C_Command：命令代码
* @retval 无
*/
void I2C_LCD_WriteCmd(unsigned char I2C_Command) {
	I2C_LCD_WriteByte(0x00, I2C_Command);
}

/**
* @brief  I2C_LCD_WriteDat，向OLED写入数据
* @param  I2C_Data：数据
* @retval 无
*/
void I2C_LCD_WriteDat(unsigned char I2C_Data) {
	I2C_LCD_WriteByte(0x40, I2C_Data);
}

/**
* @brief  OLED_Buf_Clear
* @param  清空Buf
* @retval 无
*/
void OLED_Buf_Clear(void) {
    memset(oled_page_buf, OLED_PIX_OFF, sizeof(oled_page_buf));
}

/**
* @brief  OLED_WritePage
* @param  向 OLED 指定页发送连续 128 字节数据
* @retval 无
*/
void OLED_WritePage(uint8_t page, const uint8_t *data_buf) {
    OLED_Address(page, 1);  // 设置页地址，从第1列开始

    oop_i2c_start(&g_oled_i2c);
    oop_i2c_send_byte(&g_oled_i2c, OLED_ADDRESS | OLED_I2C_WR);
    if (oop_i2c_wait_ack(&g_oled_i2c)) goto fail;

    // 控制字节：Co=0, D/C=1 表示数据后续连续写入
    oop_i2c_send_byte(&g_oled_i2c, 0x40);
    if (oop_i2c_wait_ack(&g_oled_i2c)) goto fail;

    for (uint16_t i = 0; i < 128; i++) {
        oop_i2c_send_byte(&g_oled_i2c, data_buf[i]);
        if (oop_i2c_wait_ack(&g_oled_i2c)) goto fail;
    }

    oop_i2c_stop(&g_oled_i2c);
    return;

fail:
    oop_i2c_stop(&g_oled_i2c);
}

void OLED_WritePagePartial(uint8_t page, uint8_t col_start, uint8_t col_end, const uint8_t *data_buf) {
    if (col_start > col_end || col_end >= 128 || page > 8) return;

    OLED_Address(page, col_start);  // 设置页地址和起始列

    oop_i2c_start(&g_oled_i2c);
    oop_i2c_send_byte(&g_oled_i2c, OLED_ADDRESS | OLED_I2C_WR);
    if (oop_i2c_wait_ack(&g_oled_i2c)) goto fail;

    oop_i2c_send_byte(&g_oled_i2c, 0x40);  // 控制字节，后续写入数据
    if (oop_i2c_wait_ack(&g_oled_i2c)) goto fail;

    for (uint8_t i = 0; i <= (col_end - col_start); i++) {
        oop_i2c_send_byte(&g_oled_i2c, data_buf[i]);
        if (oop_i2c_wait_ack(&g_oled_i2c)) goto fail;
    }

    oop_i2c_stop(&g_oled_i2c);
    return;

fail:
    oop_i2c_stop(&g_oled_i2c);
}

/*
*********************************************************************************************************
*	函 数 名: iic_CheckDevice
*	功能说明: 检测I2C总线设备，CPU向发送设备地址，然后读取设备应答来判断该设备是否存在
*	形    参：_Address：设备的I2C总线地址
*	返 回 值: 返回值 0 表示正确， 返回1表示未探测到
*********************************************************************************************************
*/
uint8_t OLED_CheckDevice(uint8_t _Address) {
	uint8_t ucAck;
	
	oop_i2c_start(&g_oled_i2c);		/* 发送启动信号 */

	oop_i2c_send_byte(&g_oled_i2c, _Address|OLED_I2C_WR);/* 发送设备地址 */
	ucAck = oop_i2c_wait_ack(&g_oled_i2c);	/* 检测设备的ACK应答 */

	oop_i2c_stop(&g_oled_i2c);			/* 发送停止信号 */

	return ucAck;
}

/**
* @brief  OLED_SetPos，设置光标
* @param  column,光标x位置
*					page，光标y位置
* @retval 无
*/
void OLED_Address(uint8_t page,uint8_t column) {
	column=column-1;  							
	page=page-1;
	I2C_LCD_WriteCmd(0xb0+page);   				// 设置页地址。每页是8行。一个画面的64行被分成8个页。我们平常所说的第1页，在LCD驱动IC里是第0页，所以在这里减去1*/
	I2C_LCD_WriteCmd(((column>>4)&0x0f)+0x10);	// 设置列地址的高4位
	I2C_LCD_WriteCmd(column&0x0f);				// 设置列地址的低4位
}

/**
* @brief  OLED_ON，将OLED从休眠中唤醒
* @param  无
* @retval 无
*/
void OLED_ON(void) {
	I2C_LCD_WriteCmd(0X8D);  //设置电荷泵
	I2C_LCD_WriteCmd(0X14);  //开启电荷泵
	I2C_LCD_WriteCmd(0XAF);  //OLED唤醒
}

/**
* @brief  OLED_OFF，让OLED休眠 -- 休眠模式下,OLED功耗不到10uA
* @param  无
* @retval 无
*/
void OLED_OFF(void) {
	I2C_LCD_WriteCmd(0X8D);  //设置电荷泵
	I2C_LCD_WriteCmd(0X10);  //关闭电荷泵
	I2C_LCD_WriteCmd(0XAE);  //OLED休眠
}

/**
* @brief  OLED_CLS
* @param  
* @retval 无
*/
void OLED_CLS(void) {
    memset(oled_page_buf, OLED_PIX_OFF, sizeof(oled_page_buf));
    for (uint8_t page = 0; page < 8; page++) {
        OLED_WritePage(1 + page, &oled_page_buf[page][0]);  // 页地址从 1 开始
    }
}

/**
* @brief  OLED_Fill，填充整个屏幕
* @param  fill_Data:要填充的数据
* @retval 无
*/
void OLED_Fill(uint8_t fill_Data) {
    memset(oled_page_buf, fill_Data, sizeof(oled_page_buf));
    for (uint8_t page = 0; page < 8; page++) {
        OLED_WritePage(1 + page, &oled_page_buf[page][0]);  // 页地址从 1 开始
    }
}

/**
* @brief  OLED_Fill，填充整个屏幕
* @param  fill_Data:要填充的数据
* @retval 无
*/
void OLED_Refresh(void) {
    for (uint8_t page = 0; page < 8; page++) {
        OLED_WritePage(1 + page, &oled_page_buf[page][0]);  // 页地址从 1 开始
    }
//    SEGGER_RTT_printf(0, "OLED_Refresh!\n");
}

void OLED_RefreshDiff(void) {
    for (uint8_t page = 0; page < 8; page++) {
        uint8_t col_start = 0xFF;
        uint8_t col_end = 0;

        // 查找变化的列范围
        for (uint8_t col = 0; col < 128; col++) {
            if (oled_page_buf[page][col] != oled_page_last[page][col]) {
                if (col_start == 0xFF) col_start = col;
                col_end = col;
            }
        }

        if (col_start <= col_end) {
            // 有变化，发送差异部分
            OLED_WritePagePartial(page + 1, col_start, col_end, &oled_page_buf[page][col_start]);

            // 更新缓存
            memcpy(&oled_page_last[page][col_start], &oled_page_buf[page][col_start], col_end - col_start + 1);
        }
    }
}

/**
* @brief  OLED_Init，初始化OLED
* @param  无
* @retval 无
*/
/* ---------- 字模注册 + 文本/位图渲染（自包含） ---------- */
#define OLED_FONT_MAX   8

static const oled_font_t *g_oled_fonts[OLED_FONT_MAX] = {0};

void OLED_RegisterFont(uint8_t id, const oled_font_t *font) {
    if (id < OLED_FONT_MAX) g_oled_fonts[id] = font;
}

void OLED_SetPixel(int x, int y, uint8_t on) {
    if (x < 0 || x >= OLED_COLUMN_MAX || y < 0 || y >= (OLED_PAGE_MAX * 8)) return;
    uint8_t page = (uint8_t)(y / 8);
    uint8_t bit  = (uint8_t)(y % 8);
    if (on) oled_page_buf[page][x] |= (uint8_t)(1u << bit);
    else    oled_page_buf[page][x] &= (uint8_t)(~(1u << bit));
}

/* 单字渲染：依据字模描述符逐点描到 oled_page_buf（MSB-first，逐行） */
static void oled_draw_glyph(int x, int y, const uint8_t *glyph,
                            uint8_t cell_w, uint8_t cell_h, uint8_t bytes_per_row,
                            uint8_t inverse) {
    for (uint8_t row = 0; row < cell_h; row++) {
        const uint8_t *row_bytes = glyph + (uint32_t)row * bytes_per_row;
        for (uint8_t col = 0; col < cell_w; col++) {
            uint8_t b = row_bytes[col >> 3];
            uint8_t bit = (uint8_t)((b >> (7 - (col & 7))) & 0x01);
            if (inverse) bit ^= 1;
            if (bit) OLED_SetPixel(x + col, y + row, 1);
        }
    }
}

static uint32_t oled_font_offset(const oled_font_t *f, char c) {
    if (c < (char)f->first || c > (char)f->last) return 0xFFFFFFFF;
    return (uint32_t)(c - (char)f->first) * f->cell_h * f->bytes_per_row;
}

void OLED_DrawChar(int x, int y, char c, uint8_t font_id, uint8_t inverse) {
    if (font_id >= OLED_FONT_MAX) return;
    const oled_font_t *f = g_oled_fonts[font_id];
    if (!f) return;
    uint32_t off = oled_font_offset(f, c);
    if (off == 0xFFFFFFFF) return;
    oled_draw_glyph(x, y, f->table + off, f->cell_w, f->cell_h, f->bytes_per_row, inverse);
}

void OLED_DrawString(int x, int y, const char *s, uint8_t font_id, uint8_t inverse) {
    if (!s || font_id >= OLED_FONT_MAX) return;
    const oled_font_t *f = g_oled_fonts[font_id];
    if (!f) return;
    int cx = x;
    if (f->enc == OLED_FONT_ASCII) {
        for (const char *p = s; *p; p++) {
            if (*p == '\n') { cx = x; y += f->cell_h; continue; }
            uint32_t off = oled_font_offset(f, *p);
            if (off != 0xFFFFFFFF)
                oled_draw_glyph(cx, y, f->table + off, f->cell_w, f->cell_h, f->bytes_per_row, inverse);
            cx += f->cell_w;
        }
    } else { /* GBK：双字节（兼容半角 ASCII 混合） */
        const uint8_t *p = (const uint8_t *)s;
        for (; *p; ) {
            uint8_t hi = p[0];
            if (hi >= 0x81 && hi <= 0xFE && p[1]) {
                uint8_t lo = p[1];
                uint32_t off = f->offset_of ? f->offset_of(hi, lo) : 0xFFFFFFFF;
                if (off != 0xFFFFFFFF)
                    oled_draw_glyph(cx, y, f->table + off, f->cell_w, f->cell_h, f->bytes_per_row, inverse);
                cx += f->cell_w; p += 2;
            } else {
                uint32_t off = oled_font_offset(f, (char)hi);
                if (off != 0xFFFFFFFF)
                    oled_draw_glyph(cx, y, f->table + off, f->cell_w, f->cell_h, f->bytes_per_row, inverse);
                cx += f->cell_w; p += 1;
            }
        }
    }
}

void OLED_DrawBitmap(int x, int y, int w, int h, const uint8_t *bmp, uint8_t inverse) {
    if (!bmp) return;
    uint8_t bpr = (uint8_t)((w + 7) / 8);
    for (int row = 0; row < h; row++) {
        const uint8_t *row_bytes = bmp + (uint32_t)row * bpr;
        for (int col = 0; col < w; col++) {
            uint8_t b = row_bytes[col >> 3];
            uint8_t bit = (uint8_t)((b >> (7 - (col & 7))) & 0x01);
            if (inverse) bit ^= 1;
            if (bit) OLED_SetPixel(x + col, y + row, 1);
        }
    }
}

void OLED_I2C_Init(GPIO_TypeDef* scl_port, uint16_t scl_pin,
                   GPIO_TypeDef* sda_port, uint16_t sda_pin, uint32_t delay_us) {
    oop_i2c_init(&g_oled_i2c, scl_port, scl_pin, sda_port, sda_pin, delay_us);
}

void OLED_12832_Init(void) {
    /* I2C 由 OLED_I2C_Init() 绑定引脚 */ 

    I2C_LCD_WriteCmd(0xAE);    /*display off*/

    I2C_LCD_WriteCmd(0x00);    /*set lower column address*/       
    I2C_LCD_WriteCmd(0x10);    /*set higher column address*/

    I2C_LCD_WriteCmd(0x00);    /*set display start line*/

    I2C_LCD_WriteCmd(0xB0);    /*set page address*/

    I2C_LCD_WriteCmd(0x81);    /*contract control*/
    I2C_LCD_WriteCmd(0x8f);    /*128*/
    
    I2C_LCD_WriteCmd(0xA1);    /*set segment remap*/
    
    I2C_LCD_WriteCmd(0xA6);    /*normal / reverse*/
   
    I2C_LCD_WriteCmd(0xA8);    /*multiplex ratio*/
    I2C_LCD_WriteCmd(0x1F);    /*duty = 1/32*/

    I2C_LCD_WriteCmd(0xC8);    /*Com scan direction*/

    I2C_LCD_WriteCmd(0xD3);    /*set display offset*/
    I2C_LCD_WriteCmd(0x00);

    I2C_LCD_WriteCmd(0xD5);    /*set osc division*/
    I2C_LCD_WriteCmd(0x80);    

    I2C_LCD_WriteCmd(0xD9);    /*set pre-charge period*/
    I2C_LCD_WriteCmd(0x1f);    /*0x22*/

    I2C_LCD_WriteCmd(0xDA);    /*set COM pins*/
    I2C_LCD_WriteCmd(0x00);

    I2C_LCD_WriteCmd(0xdb);    /*set vcomh*/
    I2C_LCD_WriteCmd(0x40);

    I2C_LCD_WriteCmd(0x8d);    /*set charge pump enable*/
    #if OLED_USE_OSD_PWR
    I2C_LCD_WriteCmd(0x10);
    #else
    I2C_LCD_WriteCmd(0x14);
    #endif // OLED_USE_OSD_PWR
    
    I2C_LCD_WriteCmd(0xAF);    /*display ON*/ 
}

void OLED_12864_Init(void) {
    /* I2C 由 OLED_I2C_Init() 绑定引脚 */ 
    
	oop_DelayMS(1000); // 1s,这里的延时很重要,上电后延时，没有错误的冗余设计
	
	I2C_LCD_WriteCmd(0xAE); //display off
	
	I2C_LCD_WriteCmd(0xd5); //Set Display Clock Divide Ratio/Oscillator Frequency
	I2C_LCD_WriteCmd(0x80); //0xf0 Set Clock as 100 Frames/Sec

	I2C_LCD_WriteCmd(0xa8); //--set multiplex ratio(1 to 64)
	I2C_LCD_WriteCmd(0x3F); //1/64 Duty (0x0F~0x3F) duty=1/64

    I2C_LCD_WriteCmd(0xd3); //-set display offset
	I2C_LCD_WriteCmd(0x00); //-not offset
	
	I2C_LCD_WriteCmd(0x40); //--set start line address
	
	I2C_LCD_WriteCmd(0xa1); //--set segment re-map 0 to 127
	I2C_LCD_WriteCmd(0xc8);	//Set COM Output Scan Direction

	I2C_LCD_WriteCmd(0xda); //Set COM Pins Hardware Configuration
	I2C_LCD_WriteCmd(0x12);

	I2C_LCD_WriteCmd(0x81); //--set contrast control register
	I2C_LCD_WriteCmd(0xff); //亮度调节 0x00~0xff
	
	I2C_LCD_WriteCmd(0xd9); //--set pre-charge period
	I2C_LCD_WriteCmd(0xf1); //0x22

	I2C_LCD_WriteCmd(0xdb); //Set VCOMH Deselect Level
	I2C_LCD_WriteCmd(0x40); //0x20 

//	I2C_LCD_WriteCmd(0xa4); //Set Entire Display On/Off
//	I2C_LCD_WriteCmd(0xa6); //Set Normal/Inverse Display	

	I2C_LCD_WriteCmd(0x8d); //Set Charge Pump
	I2C_LCD_WriteCmd(0x14); //
	
//	I2C_LCD_WriteCmd(0x20);	//Set Memory Addressing Mode	
//	
//	I2C_LCD_WriteCmd(0x10);	//00,Horizontal Addressing Mode;01,Vertical Addressing Mode;10,Page Addressing Mode (RESET);11,Invalid
//	I2C_LCD_WriteCmd(0xb0);	//Set Page Start Address for Page Addressing Mode,0-7
//	
//	I2C_LCD_WriteCmd(0x00); //---set low column address
//	
//	I2C_LCD_WriteCmd(0x10); //---set high column address

	I2C_LCD_WriteCmd(0xaf); //Set Display On
}

/*
*********************************************************************************************************
*	函 数 名: OLED检测测试
*	功能说明: 检测I2C总线设备，实际是对OLED_CheckDevice()的封装
*	形    参：
*	返 回 值: 返回值 0 表示没有检测到OLED，返回1表示检测到OLED
*********************************************************************************************************
*/
uint8_t OLED_Test(void) {
  if (OLED_CheckDevice(OLED_ADDRESS) == 1)
	{
		return 0;
	}
	else
	{
		return 1;
	}
}
