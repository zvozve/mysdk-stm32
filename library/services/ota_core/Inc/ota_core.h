/**
 * @file    ota_core.h
 * @brief   ota_core 模块入口：注册分区表与介质表
 * @version V1.0
 * @date    2026-09-18
 *
 * BL 与 APP 都只调这一个初始化函数，把工程侧的板级数据交给 SDK：
 *
 *     static ota_flash_t   s_flash[1];
 *     static const ota_area_t s_areas[] = {
 *         { "boot",  OTA_AREA_ROLE_BOOT,  OTA_SLOT_NONE, 0u, 0x00000u,  32u * 1024u },
 *         { "cfg",   OTA_AREA_ROLE_CFG,   OTA_SLOT_NONE, 0u, 0x08000u,  32u * 1024u },
 *         { "slotA", OTA_AREA_ROLE_RUN,   OTA_SLOT_A,    0u, 0x10000u, 448u * 1024u },
 *         { "slotB", OTA_AREA_ROLE_RUN,   OTA_SLOT_B,    0u, 0x80000u, 512u * 1024u },
 *     };
 *
 *     ota_env_t env = { s_areas, 4u, s_flash, 1u };
 *     ota_flash_int_get(&s_flash[0]);
 *     ota_init(&env);
 *
 * 注意 base 是**介质内偏移**（内部 Flash 的偏移 0 == 0x08000000），不是 CPU 地址。
 */

#ifndef __OTA_CORE_H
#define __OTA_CORE_H

#include <stdint.h>
#include "ota_common.h"
#include "ota_flash.h"
#include "ota_area.h"
#include "ota_cfg.h"
#include "ota_image.h"
#include "ota_source.h"
#include "ota_flow.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 环境：分区表 + 介质表（都须常驻，SDK 只存指针） */
typedef struct {
    const ota_area_t  *areas;
    uint8_t            area_count;
    const ota_flash_t *flashes;
    uint8_t            flash_count;
} ota_env_t;

/**
 * @brief  注册环境并初始化各子模块
 * @return OTA_OK；否则第一个出错子模块的返回码
 *         （OTA_ERR_NO_AREA = 分区表校验不过或缺 CFG 区；OTA_ERR_MEDIA = 介质不可用）
 * @note   内部顺序：ota_area_init（含 validate）→ ota_cfg_init（含 CFG 区可对半分校验）。
 */
int ota_init(const ota_env_t *env);

/* ---------------- 常用只读查询 ---------------- */

/** @brief 镜像体积上限 = min(各 RUN 槽容量)，用于打包前预估与下载前拦截 */
uint32_t ota_core_image_max(void);

/** @brief 是否处于「A/B 双槽」拓扑（RUN 有 2 个 → 生效方式是 SWITCH） */
int ota_core_is_ab(void);

/**
 * @brief  生成一份「出厂默认」状态（不落盘）
 * @note   给 BL 首次上电用：CFG 两份都无效时以它为准，而不是当成坏固件去回滚。
 */
void ota_core_default_cfg(ota_cfg_t *out);

#ifdef __cplusplus
}
#endif

#endif /* __OTA_CORE_H */
