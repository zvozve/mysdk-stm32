/**
 * @file    ds1302.h
 * @brief   DS1302 实时时钟驱动（3 线软件时序，基于 oop_gpio / oop_dwt）
 * @version V2.0
 * @date    2026-09-17
 *
 * @note    板无关：CLK / DIO / RST 三根引脚由 DS1302_Bind() 注入，
 *          本驱动不引用任何 CubeMX 符号（RTC_CLK_GPIO_Port 等只存在于
 *          工程 board_cfg.h / .ioc）。全部硬件访问下沉 chip 层：
 *            - 电平读写 / 方向切换 : oop_gpio
 *            - 微秒延时            : oop_dwt
 *          DIO 为单线双向：写时序输出，读时序前半段输出地址、后半段切输入。
 */

#ifndef __DS1302_H
#define __DS1302_H

#include <stdint.h>
#include "hal_platform.h"   /* GPIO_TypeDef（头文件自包含，不依赖包含顺序） */

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================
 * 1. DS1302 命令字节定义
 * ============================================ */

/* 时间/日期寄存器读写命令 */
#define DS1302_CMD_WRITE_SEC    0x80    // 写秒寄存器
#define DS1302_CMD_READ_SEC     0x81    // 读秒寄存器
#define DS1302_CMD_WRITE_MIN    0x82    // 写分钟寄存器
#define DS1302_CMD_READ_MIN     0x83    // 读分钟寄存器
#define DS1302_CMD_WRITE_HOUR   0x84    // 写小时寄存器
#define DS1302_CMD_READ_HOUR    0x85    // 读小时寄存器
#define DS1302_CMD_WRITE_DATE   0x86    // 写日期寄存器
#define DS1302_CMD_READ_DATE    0x87    // 读日期寄存器
#define DS1302_CMD_WRITE_MONTH  0x88    // 写月份寄存器
#define DS1302_CMD_READ_MONTH   0x89    // 读月份寄存器
#define DS1302_CMD_WRITE_WEEK   0x8A    // 写星期寄存器
#define DS1302_CMD_READ_WEEK    0x8B    // 读星期寄存器
#define DS1302_CMD_WRITE_YEAR   0x8C    // 写年份寄存器
#define DS1302_CMD_READ_YEAR    0x8D    // 读年份寄存器

/* 控制寄存器命令 */
#define DS1302_CMD_WRITE_CTRL   0x8E    // 写控制寄存器（写保护）
#define DS1302_CMD_READ_CTRL    0x8F    // 读控制寄存器

/* 控制寄存器位定义 */
#define DS1302_CTRL_WP          0x80    // 写保护位（1=禁止写入）


/* ============================================
 * 2. 时间结构体定义
 * ============================================ */

/**
 * @brief DS1302 时间日期结构体
 * @note  所有成员均为十进制数值（非BCD码）
 */
typedef struct {
    uint8_t second;     // 秒 (0-59)
    uint8_t minute;     // 分钟 (0-59)
    uint8_t hour;       // 小时 (0-23，24小时制)
    uint8_t date;       // 日期 (1-31)
    uint8_t month;      // 月份 (1-12)
    uint8_t week;       // 星期 (1-7，1=Sunday)
    uint8_t year;       // 年份 (0-99，代表2000-2099)
} DS1302_Time_t;


/* ============================================
 * 3. BCD码转换工具宏
 * ============================================ */

/**
 * @brief BCD码转十进制
 * @param bcd BCD码值（如 0x12）
 * @return 十进制值（如 12）
 */
#define DS1302_BCD2DEC(bcd) ((((bcd) >> 4) * 10) + ((bcd) & 0x0F))

/**
 * @brief 十进制转BCD码
 * @param dec 十进制值（如 12）
 * @return BCD码值（如 0x12）
 */
#define DS1302_DEC2BCD(dec) ((((dec) / 10) << 4) + ((dec) % 10))


/* ============================================
 * 4. 外部API函数声明
 * ============================================ */

/**
 * @brief 板级绑定：注入三根引脚的端口/引脚号（去 CubeMX 宏硬编码）
 * @param clk_port, clk_pin  CLK 时钟线（输出）
 * @param dio_port, dio_pin  DIO 数据线（双向：写时输出、读时输入）
 * @param rst_port, rst_pin  RST 复位线（输出，高有效）
 * @note  须在 DS1302_Init() 之前调用；引脚来源为工程 board_cfg.h。
 */
void DS1302_Bind(GPIO_TypeDef *clk_port, uint16_t clk_pin,
                 GPIO_TypeDef *dio_port, uint16_t dio_pin,
                 GPIO_TypeDef *rst_port, uint16_t rst_pin);

/**
 * @brief DS1302 初始化
 * @note  关闭写保护，使能写入操作；未 Bind 时为空操作
 */
void DS1302_Init(void);

/**
 * @brief 获取当前时间
 * @param time 指向时间结构体的指针，用于存储读取的时间数据
 * @note  读取的数据会自动从BCD码转换为十进制
 */
void DS1302_GetTime(DS1302_Time_t *time);

/**
 * @brief 设置当前时间
 * @param time 指向时间结构体的指针，包含要设置的时间数据
 * @note  传入的数据应为十进制值，函数会自动转换为BCD码写入
 */
void DS1302_SetTime(DS1302_Time_t *time);
void DS1302_SetDateTime(uint8_t year, uint8_t month, uint8_t date,
                        uint8_t hour, uint8_t minute, uint8_t second,
                        uint8_t week);

/**
 * @brief 获取指定寄存器的值（原始BCD码）
 * @param cmd 读命令（如 DS1302_CMD_READ_SEC）
 * @return 寄存器原始值（BCD码格式）
 * @note  此函数用于调试或特殊需求
 */
uint8_t DS1302_ReadRegister(uint8_t cmd);

/**
 * @brief 写入指定寄存器的值（原始BCD码）
 * @param cmd 写命令（如 DS1302_CMD_WRITE_SEC）
 * @param dat 要写入的数据（BCD码格式）
 * @note  此函数用于调试或特殊需求
 */
void DS1302_WriteRegister(uint8_t cmd, uint8_t dat);

/**
 * @brief 检查DS1302芯片是否正常工作
 * @return 1: 正常, 0: 异常
 * @note  通过读取秒寄存器并检查最高位（CH位）来判断
 */
uint8_t DS1302_Check(void);

/**
 * @brief 开启或关闭写保护
 * @param enable 1: 开启写保护, 0: 关闭写保护
 */
void DS1302_WriteProtect(uint8_t enable);

#ifdef __cplusplus
}
#endif

#endif /* __DS1302_H */
