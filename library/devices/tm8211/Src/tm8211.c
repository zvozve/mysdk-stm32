#include "tm8211.h"
#include <math.h>

#define TM8211_PI 3.1415926f

/* DMA 整帧发送完成回调（经 oop_i2s 桥接派发，device 层不直调 HAL） */
static void tm8211_on_tx_cplt(TM8211_DeviceTypeDef *dev);

static void tm8211_tx_cplt_trampoline(void *arg)
{
    tm8211_on_tx_cplt((TM8211_DeviceTypeDef *)arg);
}

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
    oop_i2s_dev_t *i2s,
    TM8211_BufferTypeDef *buf,
    uint32_t sample_rate,
    float volt_coeff
)
{
    /* 调用方须已 oop_i2s_init(i2s, &hi2sX)，本驱动只登记发送完成回调 */
    dev->i2s = i2s;
    oop_i2s_set_tx_cplt_cb(dev->i2s, tm8211_tx_cplt_trampoline, dev);

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
    oop_i2s_transmit_dma(dev->i2s, (uint16_t *)dev->buf->buf, dev->dma_len);
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

    oop_i2s_stop_dma(dev->i2s);

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

    oop_i2s_transmit_dma(dev->i2s, (uint16_t *)dev->buf->buf, dev->dma_len);
    dev->update_flag = UPDATE_NONE;
}
