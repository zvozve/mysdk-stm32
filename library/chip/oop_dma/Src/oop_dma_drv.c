/**
 * @file    oop_dma_drv.c
 * @brief   OOP DMA 驱动封装实现
 * @version V1.0
 * @date    2026-09-16
 */

#include "oop_dma_drv.h"

/* ========== 回调登记表（小表，ISR 内线性查找安全） ========== */
#define OOP_DMA_MAX_CB  8

typedef struct {
    DMA_HandleTypeDef  *hdma;
    oop_dma_cplt_cb_t   cb;
    void               *user;
    bool                used;
} oop_dma_cb_entry_t;

static oop_dma_cb_entry_t g_dma_cbs[OOP_DMA_MAX_CB] = {0};

/* HAL 完成回调（统一入口，查表分发到用户 cb） */
static void oop_dma_hal_cplt_cb(DMA_HandleTypeDef *hdma)
{
    if (hdma == NULL) return;
    for (int i = 0; i < OOP_DMA_MAX_CB; i++) {
        if (g_dma_cbs[i].used && g_dma_cbs[i].hdma == hdma) {
            if (g_dma_cbs[i].cb) {
                g_dma_cbs[i].cb(g_dma_cbs[i].user);
            }
            return;
        }
    }
}

bool oop_dma_start_it(DMA_HandleTypeDef *hdma, uint32_t src, uint32_t dst, uint32_t len)
{
    if (hdma == NULL) return false;
    return (HAL_DMA_Start_IT(hdma, src, dst, len) == HAL_OK);
}

bool oop_dma_abort(DMA_HandleTypeDef *hdma)
{
    if (hdma == NULL) return false;
    return (HAL_DMA_Abort(hdma) == HAL_OK);
}

bool oop_dma_register_callback(DMA_HandleTypeDef *hdma,
                               oop_dma_cplt_cb_t cb, void *user)
{
    if (hdma == NULL) return false;

    int idx = -1;
    for (int i = 0; i < OOP_DMA_MAX_CB; i++) {
        if (g_dma_cbs[i].used && g_dma_cbs[i].hdma == hdma) { idx = i; break; }
    }
    if (idx < 0) {
        for (int i = 0; i < OOP_DMA_MAX_CB; i++) {
            if (!g_dma_cbs[i].used) { idx = i; break; }
        }
    }
    if (idx < 0) return false;

    g_dma_cbs[idx].hdma = hdma;
    g_dma_cbs[idx].cb   = cb;
    g_dma_cbs[idx].user = user;
    g_dma_cbs[idx].used = true;

    return (HAL_DMA_RegisterCallback(hdma, HAL_DMA_XFER_CPLT_CB_ID,
                                     oop_dma_hal_cplt_cb) == HAL_OK);
}
