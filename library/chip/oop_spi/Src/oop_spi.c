/* ============================================================
 * oop_spi.c - SPI 总线 OOP 封装实现（HAL_SPI_* 唯一入口，chip 层）
 *
 * 实例化模型：每个 SPI 从设备持有自己的 oop_spi_dev_t，句柄与超时随实例
 * 走。模块内不保留任何静态状态，故 hspi2 / hspi3 等不同 SPI 外设可并存。
 * CS 片选由调用方（device）经 oop_gpio 自行管理，本模块只负责 SPI 外设传输。
 * ============================================================ */

#include "oop_spi.h"

void oop_spi_dev_init(oop_spi_dev_t *dev, SPI_HandleTypeDef *hspi) {
    if (!dev) return;
    dev->hspi    = hspi;
    dev->timeout = HAL_MAX_DELAY;
}

HAL_StatusTypeDef oop_spi_transmit(oop_spi_dev_t *dev, const uint8_t *tx, uint16_t len) {
    if (!dev || !dev->hspi || !tx || len == 0) return HAL_ERROR;
    return HAL_SPI_Transmit(dev->hspi, (uint8_t *)tx, len, dev->timeout);
}

HAL_StatusTypeDef oop_spi_receive(oop_spi_dev_t *dev, uint8_t *rx, uint16_t len) {
    if (!dev || !dev->hspi || !rx || len == 0) return HAL_ERROR;
    return HAL_SPI_Receive(dev->hspi, rx, len, dev->timeout);
}

HAL_StatusTypeDef oop_spi_transmit_receive(oop_spi_dev_t *dev,
                                           const uint8_t *tx, uint8_t *rx, uint16_t len) {
    if (!dev || !dev->hspi || !tx || !rx || len == 0) return HAL_ERROR;
    return HAL_SPI_TransmitReceive(dev->hspi, (uint8_t *)tx, rx, len, dev->timeout);
}
