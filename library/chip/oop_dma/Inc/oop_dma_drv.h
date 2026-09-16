/**
 * @file    oop_dma_drv.h
 * @brief   OOP DMA 驱动封装（HAL_DMA_* 的唯一入口）
 * @version V1.0
 * @date    2026-09-16
 *
 * 设计：device / 其他 chip 模块统一走本封装，禁止直调 HAL_DMA_* 或裸写
 * DMA_Channel_TypeDef->CCR/CPAR/CMAR/CNDTR。DMA 句柄由调用方（board_cfg /
 * 设备注册）注入，本层只做板无关的 OOP 封装。
 *
 * 完成回调：HAL_DMA 仅支持 void(*)(DMA_HandleTypeDef*) 一种签名，本封装用
 * 内部表把 (hdma -> cb,user) 关联起来，对外暴露 void(*)(void*user) 风格的
 * 注册接口，ISR 内查表分发（表小，线性查找安全）。
 */

#ifndef __OOP_DMA_DRV_H
#define __OOP_DMA_DRV_H

#include <stdint.h>
#include <stdbool.h>
#include "hal_platform.h"   /* DMA_HandleTypeDef / HAL_DMA_*（不依赖工程 dma.h） */

#ifdef __cplusplus
extern "C" {
#endif

/* ========== DMA 完成回调（用户态，带 user 指针） ========== */
typedef void (*oop_dma_cplt_cb_t)(void *user);

/* ========== 突发/单次传输启动（IT 模式，配置并立即使能） ========== */
bool oop_dma_start_it(DMA_HandleTypeDef *hdma, uint32_t src, uint32_t dst, uint32_t len);

/* ========== 中止传输 ========== */
bool oop_dma_abort(DMA_HandleTypeDef *hdma);

/* ========== 注册完成回调 ========== */
bool oop_dma_register_callback(DMA_HandleTypeDef *hdma,
                               oop_dma_cplt_cb_t cb, void *user);

#ifdef __cplusplus
}
#endif

#endif /* __OOP_DMA_DRV_H */
