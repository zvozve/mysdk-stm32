/* ============================================================
 * mcp42010.c - MCP42010 数字电位器驱动实现 (SPI)
 *
 * SDK 版：由工程 Hardware/mcp42010 迁入 chip 层。去掉 CubeMX SPI 头
 * 与 extern hspi1 依赖，改用 MCP42010_Init 注入的模块级 g_mcp。
 * 直调 HAL_SPI_Transmit / HAL_GPIO_WritePin 合法（chip 层）。
 * ============================================================ */

#include "mcp42010.h"
#include "hal_platform.h"
#include "oop_spi.h"          /* SPI HAL 封装（HAL_SPI_* 唯一入口，chip 层） */
#include "oop_gpio_drv.h"     /* CS 片选（device 层经 oop_gpio 管理） */
#include "SEGGER_RTT_Log.h"

/* 模块级上下文：CS 由 oop_gpio 管理；SPI 句柄注入 oop_spi（去 extern hspi1 硬编码） */
static mcp42010_spi_t g_mcp = {0};

void MCP42010_Init(SPI_HandleTypeDef *hspi, GPIO_TypeDef *cs_port, uint16_t cs_pin) {
    oop_spi_init(hspi);
    oop_gpio_init_output(&g_mcp.cs, cs_port, cs_pin, true);
}

void MCP42010_Write(uint8_t chn, uint8_t value) {
    uint8_t data[2];
    data[0] = MCP42010_CMD_WR | (chn + 1);
    data[1] = value;
    oop_gpio_set_low(&g_mcp.cs);
    oop_spi_transmit(data, 2);
    oop_gpio_set_high(&g_mcp.cs);
}

uint8_t MCP42010_Read(uint8_t chn) {
    uint8_t cmd = MCP42010_CMD_RD | (chn & 0x01); /* 0x0C 读 Wiper 0, 0x0D 读 Wiper 1 */
    uint8_t data = 0xFF; /* 存储读取值 */

    oop_gpio_set_low(&g_mcp.cs);
    oop_spi_transmit(&cmd, 1);
    oop_spi_receive(&data, 1);
    oop_gpio_set_high(&g_mcp.cs);

    return data;
}

/* 参考电压(mV) → 抽头 0..255，再写入 */
void MCP42010_SetLevel(uint8_t chn, uint16_t mv) {
    uint8_t wiper;
    if (mv >= MCP42010_VREF_MV) {
        wiper = MCP42010_WIPER_MAX;
    } else {
        wiper = (uint8_t)((uint32_t)mv * MCP42010_WIPER_MAX / MCP42010_VREF_MV);
    }
    MCP42010_Write(chn, wiper);
}

/* 线性扫电压（校准/演示用） */
void MCP42010_Sweep(uint8_t chn, uint16_t from_mv, uint16_t to_mv, uint16_t steps) {
    if (steps == 0) return;
    for (uint16_t i = 0; i <= steps; i++) {
        uint32_t t = (uint32_t)i * (to_mv - from_mv) + (uint32_t)from_mv * steps;
        uint16_t mv = (uint16_t)(t / steps);
        MCP42010_SetLevel(chn, mv);
    }
}
