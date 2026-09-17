/* ============================================================
 * oop_spi.c - SPI 总线 OOP 封装实现（HAL_SPI_* 唯一入口，chip 层）
 *
 * 与 oop_i2c / oop_uart 同构：单一 SPI 实例句柄注入，device 层只用
 * oop_spi_transmit / oop_spi_receive，不碰 HAL。CS 片选由调用方（device）
 * 经 oop_gpio 自行管理，本模块只负责 SPI 外设传输。
 * ============================================================ */

#include "oop_spi.h"

/* 模块级 SPI 上下文（由 oop_spi_init 注入，去 extern hspi1 硬编码） */
static SPI_HandleTypeDef *g_hspi = NULL;

void oop_spi_init(SPI_HandleTypeDef *hspi) {
    g_hspi = hspi;
}

HAL_StatusTypeDef oop_spi_transmit(const uint8_t *tx, uint16_t len) {
    if (!g_hspi || !tx || len == 0) return HAL_ERROR;
    return HAL_SPI_Transmit(g_hspi, (uint8_t *)tx, len, HAL_MAX_DELAY);
}

HAL_StatusTypeDef oop_spi_receive(uint8_t *rx, uint16_t len) {
    if (!g_hspi || !rx || len == 0) return HAL_ERROR;
    return HAL_SPI_Receive(g_hspi, rx, len, HAL_MAX_DELAY);
}
