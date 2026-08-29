#ifndef __TM8211_H__
#define __TM8211_H__

#include "hal_platform.h"
#include <stdint.h>

/*
 * TM8211（双 16-bit DAC）驱动 —— I2S + DMA 后端。
 * 板无关：I2S 句柄由调用方注入（dev->hi2s），缓冲区由调用方提供，
 * 驱动不引用任何具体外设全局 / CubeMX 宏。
 * 波形由 DMA 自动循环发送，频率/幅度通过 TM8211_SetFreqAmp() 浮点接口实时更新。
 */

#define TM8211_MAX_POINTS 2048

/* 应用提供的音频缓冲（双声道 L/R 交错，单声道 TM8211_MAX_POINTS 点） */
typedef struct {
    int16_t buf[TM8211_MAX_POINTS * 2];
} TM8211_BufferTypeDef;

typedef enum {
    UPDATE_NONE = 0,
    UPDATE_AMP,
    UPDATE_FREQ
} TM8211_UpdateTypeDef;

typedef struct {
    I2S_HandleTypeDef *hi2s;
    TM8211_BufferTypeDef *buf;

    uint32_t sample_rate;

    float freq;           /* 浮点频率 */
    float amp;            /* 浮点幅度 */
    float volt_coeff;     /* 每设备独立电压校正系数 */

    uint32_t curr_points;
    uint32_t dma_len;

    TM8211_UpdateTypeDef update_flag;
    float target_freq;
    float target_amp;
} TM8211_DeviceTypeDef;

/*
 * 初始化：注入 I2S 句柄、音频缓冲、采样率与电压系数。
 * 默认 50Hz / 20000 幅度，调用 TM8211_SetFreqAmp() 可改。
 */
void TM8211_Init(
    TM8211_DeviceTypeDef *dev,
    I2S_HandleTypeDef *hi2s,
    TM8211_BufferTypeDef *buf,
    uint32_t sample_rate,
    float volt_coeff
);

/* 生成波形并启动 DMA 循环发送 */
void TM8211_Start(TM8211_DeviceTypeDef *dev);

/* 浮点输入接口：实时设定目标频率/幅度（下次 DMA 整帧完成回调时生效） */
void TM8211_SetFreqAmp(TM8211_DeviceTypeDef *dev, float freq, float amp);

#endif
