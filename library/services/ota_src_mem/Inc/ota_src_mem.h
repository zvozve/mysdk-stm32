/**
 * @file    ota_src_mem.h
 * @brief   取数后端：内存/RAM（单测与自检用，零依赖）
 * @version V1.0
 * @date    2026-09-18
 *
 * 它的价值不在产品功能，而在**把 OTA 逻辑与传输通道解耦开**：
 * 用一块内存冒充 .otapkg，就能在没有任何网络/串口协议栈的情况下跑通
 * 「写 → 双重校验 → 提交」全链路。调 YMODEM 时若出问题，可以确定问题在通道侧，
 * 而不是在 OTA 逻辑侧 —— 否则两类问题混在一起，排查成本翻几倍。
 *
 * `seekable = 0` 可模拟流式通道（串口），用来覆盖「不能回退 → 只能顺序丢弃」那条路径。
 */

#ifndef __OTA_SRC_MEM_H
#define __OTA_SRC_MEM_H

#include <stdint.h>
#include "ota_source.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 源状态（调用方持有） */
typedef struct {
    const uint8_t *data;      /*!< 包数据 */
    uint32_t       size;      /*!< 包字节数 */
    uint32_t       pos;       /*!< 当前读位置 */
    uint8_t        seekable;  /*!< 1 = 允许 seek；0 = 模拟流式 */
} ota_src_mem_t;

/**
 * @brief  绑定一块内存作为固件包源
 * @param  src      输出：源实例
 * @param  st       调用方持有的状态
 * @param  data     包数据（须常驻）
 * @param  size     字节数
 * @param  seekable 0 = 模拟不可回退的流式通道
 */
void ota_src_mem_setup(ota_source_t *src, ota_src_mem_t *st,
                       const uint8_t *data, uint32_t size, uint8_t seekable);

#ifdef __cplusplus
}
#endif

#endif /* __OTA_SRC_MEM_H */
