/**
 * @brief   24CXX EEPROM 驱动（基于软件 I2C / oop_i2c）
 * @version V1.0
 * @date    2026-08-29
 *
 * @note    板无关：I2C 引脚由 at24cxx_init() 注入，不引用任何 CubeMX 符号。
 *          依赖 chip 层 oop_i2c_drv（软件 I2C）。
 */

#ifndef __24CXX_H
#define __24CXX_H

#include <stdint.h>
#include <stdbool.h>
#include "oop_i2c_drv.h"   /* 提供 GPIO_TypeDef 与 soft_i2c_t */

#define AT24C01     127
#define AT24C02     255
#define AT24C04     511
#define AT24C08     1023
#define AT24C16     2047
#define AT24C32     4095
#define AT24C64     8191
#define AT24C128    16383
#define AT24C256    32767

#define EE_TYPE     AT24C02
#define EE_TYPEx    (EE_TYPE - 1)
#define EE_INIT     0xFF
#define EE_CRC      0x55
#define EE_ERASE 0

extern uint8_t EEPROM_ReadBuffer[EE_TYPE];
extern uint8_t EEPROM_WriteBuffer[EE_TYPE];
extern bool EEPROM_isFirstUse;
extern bool EEPROM_StatusOK;

/**
 * @brief   初始化 EEPROM（注入 I2C 引脚）
 * @param   scl_port/scl_pin  时钟线
 * @param   sda_port/sda_pin  数据线
 * @param   delay_us          软件 I2C 位延时（微秒）
 * @param   erase_data        true=首次使用擦除
 * @retval  true: 检测到器件  false: 检测失败
 */
bool    at24cxx_init(GPIO_TypeDef* scl_port, uint16_t scl_pin,
                     GPIO_TypeDef* sda_port, uint16_t sda_pin,
                     uint32_t delay_us, bool erase_data);
bool    at24cxx_check(void);
void    at24cxx_Erase(void);

uint8_t at24cxx_read_one_byte(uint16_t addr);
void    at24cxx_read_u8 (uint16_t addr, uint8_t  *pbuf, uint16_t datalen);
void    at24cxx_read_u16(uint16_t addr, uint16_t *pbuf, uint16_t datalen);
void    at24cxx_read_u32(uint16_t addr, uint32_t *pbuf, uint16_t datalen);

void    at24cxx_write_one_byte(uint16_t addr, uint8_t data);
void    at24cxx_write_page(uint16_t addr, uint8_t *data, uint8_t len);
void    at24cxx_write_u8 (uint16_t addr, uint8_t  *pbuf, uint16_t datalen);
void    at24cxx_write_u16(uint16_t addr, uint16_t *pbuf, uint16_t datalen);
void    at24cxx_write_u32(uint16_t addr, uint32_t *pbuf, uint16_t datalen);

int at24cxx_test(uint16_t start_addr, uint16_t len);
void at24cxx_test_string(uint16_t addr);
void at24cxx_run_all_tests(void);

#endif
