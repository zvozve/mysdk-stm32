/**
 * @file    ota_flash.h
 * @brief   介质抽象：内部 Flash 与外挂 Flash 统一（BL 与 APP 共用）
 * @version V1.0
 * @date    2026-09-18
 *
 * 抹平介质差异，让「暂存在内」和「暂存在外」走同一份代码。
 *
 * ⚠ **偏移一律相对介质起点**（不是 CPU 地址）：
 *    - 内部 Flash：off=0 表示 0x08000000；CPU 地址 = `oop_flash_info()->base + off`
 *    - 外挂 SPI NOR：本来就不在 CPU 地址空间，只有介质内偏移
 *   需要 CPU 绝对地址的地方（跳转、VTOR）用 ota_area_cpu_addr()。
 *
 * 内部介质实现（ota_flash_int_get）坐 chip.oop_flash 之上，是 `HAL_FLASH_*` 唯一入口；
 * 本层（services）不出现任何 HAL 符号。外挂介质的适配由可选模块 services.ota_flash_ext 提供。
 */

#ifndef __OTA_FLASH_H
#define __OTA_FLASH_H

#include <stdint.h>
#include "ota_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 介质实例（由调用方持有，可多片并存） */
typedef struct ota_flash_s {
    const char *name;      /*!< 诊断用名字，如 "int" / "w25q64" */
    ota_media_t media;     /*!< 介质类型 */
    uint32_t    base_addr; /*!< CPU 基址（内部 Flash = 0x08000000）；0 = 不可直接寻址 */
    uint32_t    size;      /*!< 容量（字节）；0 = 未知 */

    int      (*read) (void *ctx, uint32_t off, void *buf, uint32_t len);
    int      (*erase)(void *ctx, uint32_t off, uint32_t len);
    int      (*write)(void *ctx, uint32_t off, const void *buf, uint32_t len);

    /**
     * @brief 该偏移处的擦除单位（字节）
     * @note  可为 NULL（此时按 OTA_FLASH_DEFAULT_ERASE_UNIT 逐块擦）。
     *        「擦除进度可中断」靠它推进：ota_flow 每次 step 只擦一个单位，
     *        于是 448 KB 的长擦除被切成多步，步与步之间上层可以喂狗。
     */
    uint32_t (*erase_unit)(void *ctx, uint32_t off);

    void *ctx;             /*!< 传给上面四个回调的上下文 */
} ota_flash_t;

/** @brief erase_unit 缺失时的兜底粒度 */
#define OTA_FLASH_DEFAULT_ERASE_UNIT  4096u

/**
 * @brief  取「内部 Flash」介质实例（单例，惰性初始化）
 * @param  out  输出；调用方持有
 * @return OTA_OK / OTA_ERR_PARAM
 */
int ota_flash_int_get(ota_flash_t *out);

/**
 * @brief  取某个偏移处的擦除单位（回退到默认值）
 */
uint32_t ota_flash_erase_unit(const ota_flash_t *f, uint32_t off);

/** @brief 介质是否可用（回调齐全且容量已知） */
int ota_flash_check(const ota_flash_t *f);

#ifdef __cplusplus
}
#endif

#endif /* __OTA_FLASH_H */
