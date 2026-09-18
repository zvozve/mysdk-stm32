/**
 * @file    boot_health.h
 * @brief   试运行计数与回滚判定（纯逻辑，不碰 Flash —— 便于单测）
 * @version V1.0
 * @date    2026-09-18
 *
 * 「新固件好不好」只有一个判据：**APP 自己调 ota_confirm() 确认**。
 * BL 判不了业务是否正常，它能做的只有：给新固件若干次机会，超了就撤回旧槽。
 *
 *   trial_slot != NONE（新固件待确认）：
 *       boot_try < max_try  → boot_try++，继续跳 trial_slot
 *       boot_try >= max_try → 回滚：trial_slot 清空，跳 active_slot
 *
 *   trial_slot == NONE（已确认）：
 *       直接跳 active_slot，**不改状态记录**（正常开机零擦写，保护 Flash 寿命）
 *
 * 回滚是瞬时的：旧槽内容从头到尾没被动过，所以「回滚」只是把目标槽号换个值，
 * 不涉及任何搬运。
 *
 * 本文件只依赖 ota_cfg.h 的数据结构，不含 HAL / chip 依赖，可以放在 PC 上编译单测。
 */

#ifndef __BOOT_HEALTH_H
#define __BOOT_HEALTH_H

#include <stdint.h>
#include "ota_cfg.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief max_try 传 0 时使用的默认次数 */
#define BOOT_HEALTH_DEFAULT_MAX_TRY  3u

/**
 * @brief  决定本次该跳哪个槽，并更新试运行计数
 * @param  cfg        状态记录（**就地修改**）
 * @param  max_try    试运行最大尝试次数（0 = BOOT_HEALTH_DEFAULT_MAX_TRY）
 * @param  out_slot   输出：本次要跳的槽
 * @param  out_rolled 输出（可 NULL）：1 = 本次发生了回滚
 * @return  0  决策完成，cfg 未改动（**无需落盘**）
 *          1  决策完成，cfg 已改动（**调用方必须落盘**，否则计数不生效）
 *         <0  参数错误
 * @note   为什么要「返回 1 就必须落盘」：boot_try 不落盘的话，每次复位都从 0 开始，
 *         新固件永远达不到上限 —— 回滚机制等于不存在。
 */
int boot_health_apply(ota_cfg_t *cfg, uint8_t max_try, uint8_t *out_slot, int *out_rolled);

#ifdef __cplusplus
}
#endif

#endif /* __BOOT_HEALTH_H */
