/**
 * @file    ds1302.c
 * @brief   DS1302 实时时钟驱动实现（3 线软件时序，基于 oop_gpio / oop_dwt）
 * @version V2.0
 * @date    2026-09-17
 *
 * @note    全部硬件访问经 chip 层封装：
 *            - oop_gpio : CLK/DIO/RST 电平、DIO 方向切换
 *            - oop_dwt  : 微秒级时序延时
 *          三根引脚由 DS1302_Bind() 注入，本文件不含 MX 名 / 不直调 HAL。
 */

#include "ds1302.h"
#include "oop_gpio_drv.h"
#include "oop_dwt.h"

/* ============================================
 * 板级绑定：三根引脚（由 工程 board_cfg 注入）
 * ============================================ */
static gpio_dev_t s_clk;
static gpio_dev_t s_dio;
static gpio_dev_t s_rst;
static uint8_t    s_bound = 0;

/* 引脚电平宏：语义与原 HAL 版一致（active_high=true → set_high 即输出高电平） */
#define RTC_CLK_SET()     oop_gpio_set_high(&s_clk)
#define RTC_CLK_RESET()   oop_gpio_set_low(&s_clk)
#define RTC_DIO_SET()     oop_gpio_set_high(&s_dio)
#define RTC_DIO_RESET()   oop_gpio_set_low(&s_dio)
#define RTC_DIO_READ()    oop_gpio_read(&s_dio)
#define RTC_RST_SET()     oop_gpio_set_high(&s_rst)
#define RTC_RST_RESET()   oop_gpio_set_low(&s_rst)

void DS1302_Bind(GPIO_TypeDef *clk_port, uint16_t clk_pin,
                 GPIO_TypeDef *dio_port, uint16_t dio_pin,
                 GPIO_TypeDef *rst_port, uint16_t rst_pin)
{
    oop_gpio_init_output(&s_clk, clk_port, clk_pin, true);
    oop_gpio_init_output(&s_dio, dio_port, dio_pin, true);
    oop_gpio_init_output(&s_rst, rst_port, rst_pin, true);

    RTC_CLK_RESET();
    RTC_DIO_RESET();
    RTC_RST_RESET();
    s_bound = 1;
}

/* ============================================
 * 静态（私有）函数声明
 * ============================================ */

static void DS1302_WriteByte(uint8_t addr, uint8_t dat);
static uint8_t DS1302_ReadByte(uint8_t addr);

/**
 * @brief 微秒延时（DWT 周期计数，不依赖 SysTick/RTOS）
 */
static void DS1302_Delay_us(uint32_t us)
{
    oop_DelayUS(us);
}

/* ============================================
 * 底层时序函数实现
 * ============================================ */

/**
 * @brief 向DS1302写入一个字节（含地址命令）
 * @param addr 地址/命令字节
 * @param dat  要写入的数据
 */
static void DS1302_WriteByte(uint8_t addr, uint8_t dat)
{
    uint8_t i;

    RTC_RST_RESET();
    DS1302_Delay_us(1);
    RTC_CLK_RESET();
    DS1302_Delay_us(1);
    RTC_RST_SET();
    DS1302_Delay_us(1);

    // 发送地址/命令字节 (LSB first)
    for (i = 0; i < 8; i++) {
        if (addr & 0x01) RTC_DIO_SET();
        else RTC_DIO_RESET();
        DS1302_Delay_us(1);
        RTC_CLK_SET();
        DS1302_Delay_us(1);
        RTC_CLK_RESET();
        DS1302_Delay_us(1);
        addr >>= 1;
    }

    // 发送数据字节 (LSB first)
    for (i = 0; i < 8; i++) {
        if (dat & 0x01) RTC_DIO_SET();
        else RTC_DIO_RESET();
        DS1302_Delay_us(1);
        RTC_CLK_SET();
        DS1302_Delay_us(1);
        RTC_CLK_RESET();
        DS1302_Delay_us(1);
        dat >>= 1;
    }

    RTC_RST_RESET();
    DS1302_Delay_us(1);
}

/**
 * @brief 从DS1302读取一个字节
 * @param addr 读命令（如 DS1302_CMD_READ_SEC = 0x81）
 * @return 读取到的数据
 */
static uint8_t DS1302_ReadByte(uint8_t addr)
{
    uint8_t i, dat = 0;

    RTC_RST_RESET();
    DS1302_Delay_us(1);
    RTC_CLK_RESET();
    DS1302_Delay_us(1);
    RTC_RST_SET();
    DS1302_Delay_us(1);

    // 发送地址/命令字节
    for (i = 0; i < 8; i++) {
        if (addr & 0x01) RTC_DIO_SET();
        else RTC_DIO_RESET();
        DS1302_Delay_us(1);
        RTC_CLK_SET();
        DS1302_Delay_us(1);
        RTC_CLK_RESET();
        DS1302_Delay_us(1);
        addr >>= 1;
    }

    // ===== 切换DIO为输入模式 =====
    oop_gpio_set_mode(&s_dio, OOP_GPIO_MODE_INPUT, OOP_GPIO_PULL_UP, OOP_GPIO_SPEED_HIGH);
    DS1302_Delay_us(1);

    // 读取数据 (在下降沿读取)
    for (i = 0; i < 8; i++) {
        dat >>= 1;
        DS1302_Delay_us(1);
        if (RTC_DIO_READ()) {
            dat |= 0x80;
        }
        RTC_CLK_SET();
        DS1302_Delay_us(1);
        RTC_CLK_RESET();
        DS1302_Delay_us(1);
    }

    // ===== 恢复DIO为输出模式 =====
    oop_gpio_set_mode(&s_dio, OOP_GPIO_MODE_OUTPUT_PP, OOP_GPIO_PULL_UP, OOP_GPIO_SPEED_HIGH);

    RTC_RST_RESET();
    DS1302_Delay_us(1);
    return dat;
}


/* ============================================
 * 对外API函数实现
 * ============================================ */

/**
 * @brief DS1302 初始化
 */
void DS1302_Init(void)
{
    if (!s_bound) return;

    // 1. 关闭写保护
    DS1302_WriteByte(DS1302_CMD_WRITE_CTRL, 0x00);

    // 2. ★★★ 关键修复：启动时钟（清除CH位）★★★
    // 读取当前秒值，清除最高位后写回
    uint8_t sec = DS1302_ReadByte(DS1302_CMD_READ_SEC);
    sec &= 0x7F;  // 清除CH位（Bit7）
    DS1302_WriteByte(DS1302_CMD_WRITE_SEC, sec);

    // 3. 也可以直接写入一个有效值（确保时钟启动）
    // DS1302_WriteByte(DS1302_CMD_WRITE_SEC, 0x00);  // 从0秒开始

    // 4. 可选：重新开启写保护（保护数据）
    // DS1302_WriteByte(DS1302_CMD_WRITE_CTRL, 0x80);
}

/**
 * @brief 获取当前时间
 */
void DS1302_GetTime(DS1302_Time_t *time)
{
    if (time == NULL) return;

    time->second = DS1302_BCD2DEC(DS1302_ReadByte(DS1302_CMD_READ_SEC));
    time->minute = DS1302_BCD2DEC(DS1302_ReadByte(DS1302_CMD_READ_MIN));
    time->hour   = DS1302_BCD2DEC(DS1302_ReadByte(DS1302_CMD_READ_HOUR));
    time->date   = DS1302_BCD2DEC(DS1302_ReadByte(DS1302_CMD_READ_DATE));
    time->month  = DS1302_BCD2DEC(DS1302_ReadByte(DS1302_CMD_READ_MONTH));
    time->week   = DS1302_BCD2DEC(DS1302_ReadByte(DS1302_CMD_READ_WEEK));
    time->year   = DS1302_BCD2DEC(DS1302_ReadByte(DS1302_CMD_READ_YEAR));
}

/**
 * @brief 设置当前时间
 */
void DS1302_SetTime(DS1302_Time_t *time)
{
    if (time == NULL) return;

    // 关闭写保护
    DS1302_WriteProtect(0);

    // 写入时间 (写秒寄存器时，最高位CH=0启动时钟)
    DS1302_WriteByte(DS1302_CMD_WRITE_SEC, DS1302_DEC2BCD(time->second) & 0x7F);
    DS1302_WriteByte(DS1302_CMD_WRITE_MIN, DS1302_DEC2BCD(time->minute));
    DS1302_WriteByte(DS1302_CMD_WRITE_HOUR, DS1302_DEC2BCD(time->hour));
    DS1302_WriteByte(DS1302_CMD_WRITE_DATE, DS1302_DEC2BCD(time->date));
    DS1302_WriteByte(DS1302_CMD_WRITE_MONTH, DS1302_DEC2BCD(time->month));
    DS1302_WriteByte(DS1302_CMD_WRITE_WEEK, DS1302_DEC2BCD(time->week));
    DS1302_WriteByte(DS1302_CMD_WRITE_YEAR, DS1302_DEC2BCD(time->year));

    // 重新开启写保护
    DS1302_WriteProtect(1);
}

/**
 * @brief 直接设置时间（不用定义结构体）
 */
void DS1302_SetDateTime(uint8_t year, uint8_t month, uint8_t date,
                        uint8_t hour, uint8_t minute, uint8_t second,
                        uint8_t week)
{
    DS1302_Time_t time = {
        .year = year,
        .month = month,
        .date = date,
        .hour = hour,
        .minute = minute,
        .second = second,
        .week = week
    };
    DS1302_SetTime(&time);
}

/**
 * @brief 读取寄存器（原始BCD码）
 */
uint8_t DS1302_ReadRegister(uint8_t cmd)
{
    return DS1302_ReadByte(cmd);
}

/**
 * @brief 写入寄存器（原始BCD码）
 */
void DS1302_WriteRegister(uint8_t cmd, uint8_t dat)
{
    DS1302_WriteByte(cmd, dat);
}

/**
 * @brief 检查DS1302是否正常
 */
uint8_t DS1302_Check(void)
{
    uint8_t sec = DS1302_ReadByte(DS1302_CMD_READ_SEC);
    // 如果秒值小于0x60（BCD码60秒内），说明芯片响应正常
    if (sec <= 0x59) {
        return 1;
    }
    return 0;
}

/**
 * @brief 写保护控制
 */
void DS1302_WriteProtect(uint8_t enable)
{
    if (enable) {
        DS1302_WriteByte(DS1302_CMD_WRITE_CTRL, DS1302_CTRL_WP);  // 开启写保护
    } else {
        DS1302_WriteByte(DS1302_CMD_WRITE_CTRL, 0x00);            // 关闭写保护
    }
}
