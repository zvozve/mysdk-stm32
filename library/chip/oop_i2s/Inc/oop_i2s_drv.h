#ifndef __OOP_I2S_DRV_H__
#define __OOP_I2S_DRV_H__

#include <stdint.h>
#include <stdbool.h>
#include "hal_platform.h"   /* chip 层允许直接包含 HAL 统一入口 */
#include "SEGGER_RTT_Log.h"

#ifndef I2S_LOG_ENABLE
    #define I2S_LOG_ENABLE    1
#endif
#define I2S_LOG(fmt, ...)    RTT_LOG_TAG(I2S_LOG_ENABLE, "I2S", fmt, ##__VA_ARGS__)

/*
 * I2S OOP 驱动 HAL 后端（chip 层）。
 * 仅本模块（chip）直接调用 HAL_I2S_* 与定义 HAL_I2S_TxCpltCallback；
 * device 层（如 tm8211）只调用 oop_i2s_*，不碰 HAL。
 * 符合"只有 chip 才能直接用 HAL 库"的分层铁律。
 */

/* I2S 发送完成回调类型：整帧 DMA 发送完成后由底层桥接调用（参数即注册时传入的 arg） */
typedef void (*oop_i2s_tx_cplt_cb_t)(void *arg);

typedef struct {
    I2S_HandleTypeDef *hi2s;          /* 注入的 HAL 句柄（chip 层持有，符合"仅 chip 用 HAL"） */
    oop_i2s_tx_cplt_cb_t tx_cplt_cb;  /* 发送完成回调（由 device 注册） */
    void *cb_arg;
} oop_i2s_dev_t;

/* 注入 HAL 句柄并登记为当前唯一 I2S 设备（HAL_I2S_TxCpltCallback 据此派发） */
void oop_i2s_init(oop_i2s_dev_t *dev, I2S_HandleTypeDef *hi2s);

/* 设置发送完成回调（device 注册重填缓冲等逻辑；arg 通常为 device 实例） */
void oop_i2s_set_tx_cplt_cb(oop_i2s_dev_t *dev, oop_i2s_tx_cplt_cb_t cb, void *arg);

/* 启动一帧 DMA 发送 */
bool oop_i2s_transmit_dma(oop_i2s_dev_t *dev, uint16_t *buf, uint16_t len);

/* 停止 DMA 发送 */
bool oop_i2s_stop_dma(oop_i2s_dev_t *dev);

#endif /* __OOP_I2S_DRV_H__ */
