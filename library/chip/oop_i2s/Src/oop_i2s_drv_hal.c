/**
 * @file    oop_i2s_drv_hal.c
 * @brief   I2S OOP 驱动 HAL 后端（chip 层）：封装 HAL I2S + DMA，桥接 HAL_I2S_TxCpltCallback
 * @note    仅本文件（chip 层）直接调用 HAL_I2S_* 与定义 HAL_I2S_TxCpltCallback；
 *          device 层（如 tm8211）只调用 oop_i2s_*，不碰 HAL。
 *          与 oop_uart 的 HAL_UART_*Callback 桥接完全一致。
 */

#include "oop_i2s_drv.h"

/* 单实例：oop_i2s_init 时登记，HAL I2S 回调据此派发（按句柄匹配，支持多实例扩展为表） */
static oop_i2s_dev_t *s_i2s_dev = NULL;

void oop_i2s_init(oop_i2s_dev_t *dev, I2S_HandleTypeDef *hi2s)
{
    s_i2s_dev = dev;
    dev->hi2s = hi2s;
    dev->tx_cplt_cb = NULL;
    dev->cb_arg = NULL;
}

void oop_i2s_set_tx_cplt_cb(oop_i2s_dev_t *dev, oop_i2s_tx_cplt_cb_t cb, void *arg)
{
    if (dev == NULL) return;
    dev->tx_cplt_cb = cb;
    dev->cb_arg = arg;
}

bool oop_i2s_transmit_dma(oop_i2s_dev_t *dev, uint16_t *buf, uint16_t len)
{
    if (dev == NULL || dev->hi2s == NULL) return false;
    return (HAL_I2S_Transmit_DMA(dev->hi2s, buf, len) == HAL_OK);
}

bool oop_i2s_stop_dma(oop_i2s_dev_t *dev)
{
    if (dev == NULL || dev->hi2s == NULL) return false;
    return (HAL_I2S_DMAStop(dev->hi2s) == HAL_OK);
}

/* HAL I2S 发送完成回调（全局弱符号，由本驱动桥接，按登记设备派发） */
void HAL_I2S_TxCpltCallback(I2S_HandleTypeDef *hi2s)
{
    if (s_i2s_dev && hi2s == s_i2s_dev->hi2s && s_i2s_dev->tx_cplt_cb) {
        s_i2s_dev->tx_cplt_cb(s_i2s_dev->cb_arg);
    }
}
