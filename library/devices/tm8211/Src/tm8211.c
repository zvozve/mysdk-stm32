#include "tm8211.h"
#include <math.h>

#define TM8211_PI 3.1415926f

/* 单实例：TM8211_Init 时登记，HAL I2S 回调据此派发（与 oop_uart 的 HAL_UART_*Callback 桥接一致） */
static TM8211_DeviceTypeDef *s_tm8211_dev = NULL;

/* 按当前 freq/amp/volt_coeff 重算一帧正弦波写入 buf */
static void tm8211_gen_wave(TM8211_DeviceTypeDef *dev)
{
    dev->curr_points = (uint32_t)(dev->sample_rate / dev->freq);
    if (dev->curr_points > TM8211_MAX_POINTS)
        dev->curr_points = TM8211_MAX_POINTS;

    dev->dma_len = dev->curr_points * 2;

    float final_amp = dev->amp * dev->volt_coeff;

    for (uint32_t i = 0; i < dev->curr_points; i++)
    {
        float rad = 2.0f * TM8211_PI * i / dev->curr_points;
        int16_t s = (int16_t)(final_amp * sinf(rad));
        dev->buf->buf[2 * i]     = s;   /* L */
        dev->buf->buf[2 * i + 1] = s;   /* R */
    }
}

void TM8211_Init(
    TM8211_DeviceTypeDef *dev,
    I2S_HandleTypeDef *hi2s,
    TM8211_BufferTypeDef *buf,
    uint32_t sample_rate,
    float volt_coeff
)
{
    s_tm8211_dev = dev;
    dev->hi2s = hi2s;
    dev->buf = buf;
    dev->sample_rate = sample_rate;
    dev->volt_coeff = volt_coeff;

    dev->freq = 50.0f;
    dev->amp  = 20000.0f;
    dev->update_flag = UPDATE_NONE;
}

void TM8211_Start(TM8211_DeviceTypeDef *dev)
{
    tm8211_gen_wave(dev);
    HAL_I2S_Transmit_DMA(dev->hi2s, (uint16_t *)dev->buf->buf, dev->dma_len);
}

void TM8211_SetFreqAmp(TM8211_DeviceTypeDef *dev, float freq, float amp)
{
    if (amp < 0)    amp = 0;
    if (amp > 32767) amp = 32767;

    dev->target_freq = freq;
    dev->target_amp  = amp;

    if (freq != dev->freq)
        dev->update_flag = UPDATE_FREQ;
    else if (amp != dev->amp)
        dev->update_flag = UPDATE_AMP;
}

/* DMA 整帧发送完成：按 update_flag 重新生成波形并重启 DMA */
static void tm8211_on_tx_cplt(TM8211_DeviceTypeDef *dev)
{
    if (dev->update_flag == UPDATE_NONE) return;

    HAL_I2S_DMAStop(dev->hi2s);

    if (dev->update_flag == UPDATE_AMP)
    {
        dev->amp = dev->target_amp;
        float final_amp = dev->amp * dev->volt_coeff;

        for (uint32_t i = 0; i < dev->curr_points; i++)
        {
            float rad = 2.0f * TM8211_PI * i / dev->curr_points;
            int16_t s = (int16_t)(final_amp * sinf(rad));
            dev->buf->buf[2 * i]     = s;
            dev->buf->buf[2 * i + 1] = s;
        }
    }
    else
    {
        dev->freq = dev->target_freq;
        dev->amp  = dev->target_amp;
        tm8211_gen_wave(dev);
    }

    HAL_I2S_Transmit_DMA(dev->hi2s, (uint16_t *)dev->buf->buf, dev->dma_len);
    dev->update_flag = UPDATE_NONE;
}

/* HAL I2S 发送完成回调（全局弱符号，由本驱动桥接，按句柄派发到登记的设备） */
void HAL_I2S_TxCpltCallback(I2S_HandleTypeDef *hi2s)
{
    if (s_tm8211_dev && hi2s == s_tm8211_dev->hi2s) {
        tm8211_on_tx_cplt(s_tm8211_dev);
    }
}
