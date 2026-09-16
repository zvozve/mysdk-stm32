/* ============================================================
 * mcp42010.h - MCP42010 数字电位器驱动 (SPI)
 *
 * SDK 版：由工程 Hardware/mcp42010 迁入 chip 层。去掉对 CubeMX main 头
 * 与 extern hspi1 硬编码的依赖，改为 MCP42010_Init(hspi, cs_port, cs_pin)
 * 注入 SPI 句柄与 CS 引脚（板级绑定）。直调 HAL_SPI_*/HAL_GPIO_* 合法
 * （chip 层）。MCP42010_* 名称保持，工程调用点仅增加一次 Init 调用。
 * ============================================================ */

#ifndef __MCP42010_H__
#define __MCP42010_H__

#include "hal_platform.h"   /* STM32 系列 HAL 统一入口（SPI/GPIO 句柄类型） */

#define MCP42010_CMD_WR 0x10    /* 写命令 (高4位固定为0) */
#define MCP42010_CMD_RD 0x0C    /* 读 Wiper 0 命令 (0x0C) */

/* 数字电位器参考电压与抽头量程（mV / 抽头 0..255） */
#define MCP42010_VREF_MV    3300
#define MCP42010_WIPER_MAX  255

typedef struct {
    SPI_HandleTypeDef *hspi;      /* SPI 句柄 */
    GPIO_TypeDef      *cs_port;   /* CS 引脚端口 */
    uint16_t           cs_pin;    /* CS 引脚 */
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
