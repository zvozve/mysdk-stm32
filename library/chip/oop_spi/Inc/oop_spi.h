/* ============================================================
 * oop_spi.h - SPI 总线 OOP 封装（HAL_SPI_* 唯一入口，chip 层）
 *
 * 定位：与 oop_i2c / oop_uart 同构的片上外设 HAL 封装层。device 层
 * （如 mcp42010 数字电位器）统一走本封装，不直调 HAL_SPI_*。
 * 句柄由 oop_spi_init 注入（板级绑定），本模块不 extern 任何全局句柄。
 * ============================================================ */

#ifndef __OOP_SPI_H__
#define __OOP_SPI_H__

#include <stdint.h>
#include "hal_platform.h"   /* STM32 系列 HAL 统一入口（SPI 句柄类型） */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 注入 SPI 句柄（板级绑定，去 extern hspi1 硬编码）
 * @param hspi  SPI 句柄（如 &hspi1）
 */
void oop_spi_init(SPI_HandleTypeDef *hspi);

/**
 * @brief 阻塞发送（内部 HAL_SPI_Transmit，超时 HAL_MAX_DELAY）
 * @return HAL 状态（句柄未注入返回 HAL_ERROR）
 */
HAL_StatusTypeDef oop_spi_transmit(const uint8_t *tx, uint16_t len);

/**
 * @brief 阻塞接收（内部 HAL_SPI_Receive，超时 HAL_MAX_DELAY）
 * @return HAL 状态（句柄未注入返回 HAL_ERROR）
 */
HAL_StatusTypeDef oop_spi_receive(uint8_t *rx, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* __OOP_SPI_H__ */
