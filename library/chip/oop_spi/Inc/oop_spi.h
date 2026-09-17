/* ============================================================
 * oop_spi.h - SPI 总线 OOP 封装（HAL_SPI_* 唯一入口，chip 层）
 *
 * 定位：与 oop_i2c / oop_uart 同构的片上外设 HAL 封装层。device 层
 * （mcp42010 数字电位器、ch9434 串口扩展、ch374u USB Host）统一走
 * 本封装，不直调 HAL_SPI_*。
 *
 * 实例化模型：SPI 句柄装在调用方持有的 oop_spi_dev_t 中（板级绑定），
 * 本模块不 extern 任何全局句柄、也不持有模块级单例 —— 多个 SPI 从设备
 * 挂在不同 SPI 外设上（如 hspi2 挂 CH374、hspi3 挂 CH9434）互不干扰。
 * CS 片选由调用方（device）经 oop_gpio 自行管理，本模块只负责传输。
 * ============================================================ */

#ifndef __OOP_SPI_H__
#define __OOP_SPI_H__

#include <stdint.h>
#include "hal_platform.h"   /* STM32 系列 HAL 统一入口（SPI 句柄类型） */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief SPI 设备实例（调用方持有，保证多 SPI 外设可并存）
 */
typedef struct {
    SPI_HandleTypeDef *hspi;      /* 注入的 SPI 句柄（如 &hspi2） */
    uint32_t           timeout;   /* HAL 阻塞超时(ms)，默认 HAL_MAX_DELAY */
} oop_spi_dev_t;

/**
 * @brief 注入 SPI 句柄（板级绑定，去 extern hspiX 硬编码）
 * @param dev   SPI 设备实例
 * @param hspi  SPI 句柄（如 &hspi1）
 * @note  timeout 默认置 HAL_MAX_DELAY；需要有限超时的调用方在 init 后
 *        直接改 dev->timeout（如 CH374 用 100ms 容错）。
 */
void oop_spi_dev_init(oop_spi_dev_t *dev, SPI_HandleTypeDef *hspi);

/**
 * @brief 阻塞发送（内部 HAL_SPI_Transmit）
 * @return HAL 状态（dev 或 hspi 为空返回 HAL_ERROR）
 */
HAL_StatusTypeDef oop_spi_transmit(oop_spi_dev_t *dev, const uint8_t *tx, uint16_t len);

/**
 * @brief 阻塞接收（内部 HAL_SPI_Receive）
 * @return HAL 状态（dev 或 hspi 为空返回 HAL_ERROR）
 */
HAL_StatusTypeDef oop_spi_receive(oop_spi_dev_t *dev, uint8_t *rx, uint16_t len);

/**
 * @brief 阻塞全双工收发（内部 HAL_SPI_TransmitReceive）
 * @note  单字节寄存器式 SPI 器件（CH374 / CH9434）依赖此接口交换命令与数据
 * @return HAL 状态（dev 或 hspi 为空返回 HAL_ERROR）
 */
HAL_StatusTypeDef oop_spi_transmit_receive(oop_spi_dev_t *dev,
                                           const uint8_t *tx, uint8_t *rx, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* __OOP_SPI_H__ */
