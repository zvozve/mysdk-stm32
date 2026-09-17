/* ============================================================
 * mcp42010.h - MCP42010 数字电位器驱动 (SPI)
 *
 * SDK 版：由工程 Hardware/mcp42010 迁入 devices 层（外挂 SPI 电位器，
 * 非 STM32 片上外设）。去掉对 CubeMX main 头与 extern hspi1 硬编码的依赖，
 * 改为 MCP42010_Init(hspi, cs_port, cs_pin) 注入 SPI 句柄与 CS 引脚
 * （板级绑定）。SPI 走 chip/oop_spi（HAL_SPI_* 唯一入口），CS 走
 * oop_gpio，device 层不直调 HAL。MCP42010 前缀名称保持，工程调用点零改动。
 * ============================================================ */

#ifndef __MCP42010_H__
#define __MCP42010_H__

#include "hal_platform.h"   /* STM32 系列 HAL 统一入口（SPI/GPIO 句柄类型） */
#include "oop_spi.h"        /* SPI 传输（device 层经 oop_spi 管理，不碰 HAL） */
#include "oop_gpio_drv.h"   /* CS 片选（device 层经 oop_gpio 管理，不碰 HAL） */

#define MCP42010_CMD_WR 0x10    /* 写命令 (高4位固定为0) */
#define MCP42010_CMD_RD 0x0C    /* 读 Wiper 0 命令 (0x0C) */

/* 数字电位器参考电压与抽头量程（mV / 抽头 0..255） */
#define MCP42010_VREF_MV    3300
#define MCP42010_WIPER_MAX  255

typedef struct {
    oop_spi_dev_t spi;       /* SPI 实例（oop_spi 管理，多 SPI 从设备可并存） */
    gpio_dev_t    cs;        /* CS 片选（oop_gpio 管理，低有效） */
} mcp42010_spi_t;

/**
 * @brief 注入 SPI 句柄与 CS 引脚（板级绑定，去 main.h 硬编码）
 * @param hspi     SPI 句柄（如 &hspi1）
 * @param cs_port  CS 引脚端口
 * @param cs_pin   CS 引脚
 */
void MCP42010_Init(SPI_HandleTypeDef *hspi, GPIO_TypeDef *cs_port, uint16_t cs_pin);

void MCP42010_Write(uint8_t chn, uint8_t value);
uint8_t MCP42010_Read(uint8_t chn);

/* 应用层换算：以 mV 设定参考电压（内部换算为抽头 0..255） */
void MCP42010_SetLevel(uint8_t chn, uint16_t mv);

/* 应用层换算：线性扫电压（校准/演示用） */
void MCP42010_Sweep(uint8_t chn, uint16_t from_mv, uint16_t to_mv, uint16_t steps);

#endif // __MCP42010_H__
