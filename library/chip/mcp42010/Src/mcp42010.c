/* ============================================================
 * mcp42010.c - MCP42010 数字电位器驱动实现 (SPI)
 *
 * SDK 版：由工程 Hardware/mcp42010 迁入 chip 层。去掉 CubeMX SPI 头
 * 与 extern hspi1 依赖，改用 MCP42010_Init 注入的模块级 g_mcp。
 * 直调 HAL_SPI_Transmit / HAL_GPIO_WritePin 合法（chip 层）。
 * ============================================================ */

#include "mcp42010.h"
#include "hal_platform.h"
#include "SEGGER_RTT_Log.h"

/* 模块级 SPI 上下文（由 MCP42010_Init 注入，去 extern hspi1 硬编码） */
static mcp42010_spi_t g_mcp = {0};

void MCP42010_Init(SPI_HandleTypeDef *hspi, GPIO_TypeDef *cs_port, uint16_t cs_pin) {
    g_mcp.hspi    = hspi;
    g_mcp.cs_port = cs_port;
    g_mcp.cs_pin  = cs_pin;
}

void MCP42010_Write(uint8_t chn, uint8_t value) {
    uint8_t data[2];
    data[0] = MCP42010_CMD_WR | (chn + 1);
    data[1] = value;
    HAL_GPIO_WritePin(g_mcp.cs_port, g_mcp.cs_pin, GPIO_PIN_RESET);
    HAL_SPI_Transmit(g_mcp.hspi, data, 2, HAL_MAX_DELAY);
    HAL_GPIO_WritePin(g_mcp.cs_port, g_mcp.cs_pin, GPIO_PIN_SET);
}

uint8_t MCP42010_Read(uint8_t chn) {
    uint8_t cmd = MCP42010_CMD_RD | (chn & 0x01); /* 0x0C 读 Wiper 0, 0x0D 读 Wiper 1 */
    uint8_t data = 0xFF; /* 存储读取值 */

    HAL_GPIO_WritePin(g_mcp.cs_port, g_mcp.cs_pin, GPIO_PIN_RESET);
    HAL_SPI_Transmit(g_mcp.hspi, &cmd, 1, HAL_MAX_DELAY);
    HAL_SPI_Receive(g_mcp.hspi, &data, 1, HAL_MAX_DELAY);
    HAL_GPIO_WritePin(g_mcp.cs_port, g_mcp.cs_pin, GPIO_PIN_SET);

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
