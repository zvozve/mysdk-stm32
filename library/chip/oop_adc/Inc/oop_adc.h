/**
 * @file    oop_adc.h
 * @brief   OOP 通用 ADC+DMA 采样与滤波抽象层
 * @note    由工程 BSP/bsp_adc 迁入 chip 层并重命名。本模块是 ADC+DMA 的
 *          唯一所有者，注册 HAL_ADC_ConvCpltCallback：
 *          - 在转换完成中断里对 DMA 缓冲做"去极值平均"滤波，并把每通道
 *            的快照（raw / mV）存好，供中断回调消费者在任务上下文安全读取；
 *          - 通过 OOP_ADC_SetBatchCb() 通知上层"一批评测完成"，由上层决定
 *            如何聚合（滤波/入环/写出），避免在中断里做重活。
 *          chip 层直调 HAL_ADCxx / HAL_DMAxx 合法。
 * @version V2.0
 * @date    2026-08-14
 */
#ifndef __OOP_ADC_H__
#define __OOP_ADC_H__

#include <stdint.h>
#include <stdbool.h>
#include "hal_platform.h"   /* STM32 系列 HAL 统一入口（ADC/DMA 句柄类型） */

#ifndef OOP_ADC_MAX_CH
#define OOP_ADC_MAX_CH 4
#endif
#ifndef OOP_ADC_MAX_SMP
#define OOP_ADC_MAX_SMP 16
#endif

/* ADC 原始满量程与参考电压（可在包含本头文件前覆盖） */
#ifndef OOP_ADC_RESOLUTION
#define OOP_ADC_RESOLUTION 4096   /* 12-bit */
#endif
#ifndef OOP_ADC_VREF_MV
#define OOP_ADC_VREF_MV   3300
#endif

/**
 * @brief  初始化 ADC 采样（绑定句柄、通道数与每通道采样数）
 * @param  hadc   ADC 句柄（如 &hadc1）
 * @param  hdma   DMA 句柄（如 &hdma_adc1）
 * @param  n_ch   通道数（交错存放于 DMA 缓冲）
 * @param  samples_per_ch 每通道采样次数
 */
void OOP_ADC_Init(ADC_HandleTypeDef *hadc, DMA_HandleTypeDef *hdma,
                  uint8_t n_ch, uint16_t samples_per_ch);

/** @brief 启动连续 DMA 采样（内部强制 circular 模式） */
void OOP_ADC_Start(void);
/** @brief 停止采样 */
void OOP_ADC_Stop(void);

/**
 * @brief 注册"一批采样完成"回调（在 ADC 转换完成中断上下文调用，100us 批次）
 * @note  回调承担强实时消费链（波形入环 / 反弹判定 / 脉冲），允许轻量
 *        RTT 打印与 GPIO 写；严禁 Modbus 直发（只可 *_async 入队）、
 *        严禁 I2C/EEPROM/阻塞等待。传 NULL 可取消回调。
 */
void OOP_ADC_SetBatchCb(void (*cb)(void));

/**
 * @brief 返回某通道去极值平均后的原始计数值(0..RES-1)
 * @note  现场对 DMA 缓冲做 stride 滤波，不依赖转换完成中断
 */
uint16_t OOP_ADC_FilteredRaw(uint8_t ch);

/**
 * @brief 读取某通道电压(mV)
 * @param  mv_out 输出电压值
 * @retval true=成功读取
 */
bool OOP_ADC_ReadChannel(uint8_t ch, uint16_t *mv_out);

/**
 * @brief 读取中断里已算好的快照（避免与 DMA 改写产生竞争）
 * @note  快照在 HAL_ADC_ConvCpltCallback 中刷新；任务里先读 Ready 判断有效性。
 */
bool     OOP_ADC_SnapshotReady(void);
uint16_t OOP_ADC_SnapshotRaw(uint8_t ch);
uint16_t OOP_ADC_SnapshotMV (uint8_t ch);

/* ============ 与具体 ADC 解耦的滤波/换算工具（可他处复用） ============ */

/** 跨步长去最大最小后取平均（src 按 stride 交错，取第 offset 路） */
uint16_t oop_adc_filter_stride_rm_extreme(const uint16_t *src, uint8_t count,
                                          uint8_t stride, uint8_t offset);

#endif // __OOP_ADC_H__
