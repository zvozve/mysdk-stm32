/**
 * @file    boot_health.c
 * @brief   试运行计数与回滚判定
 * @version V1.0
 * @date    2026-09-18
 */

#include <stddef.h>
#include "boot_health.h"

/** @brief 把槽号收敛到合法范围（状态记录被写坏时的兜底） */
static uint8_t slot_clamp(uint8_t s)
{
    return (s < OTA_SLOT_COUNT) ? s : (uint8_t)OTA_SLOT_A;
}

int boot_health_apply(ota_cfg_t *cfg, uint8_t max_try, uint8_t *out_slot, int *out_rolled)
{
    if (cfg == NULL || out_slot == NULL) {
        return OTA_ERR_PARAM;
    }
    if (max_try == 0u) {
        max_try = BOOT_HEALTH_DEFAULT_MAX_TRY;
    }
    if (out_rolled != NULL) {
        *out_rolled = 0;
    }

    /* ---- 已确认状态：跳 active_slot，不写状态区（正常开机零擦写） ---- */
    if (cfg->trial_slot == OTA_SLOT_NONE) {
        *out_slot = slot_clamp(cfg->active_slot);
        return 0;
    }

    /* ---- 试运行已用尽次数 → 回滚 ---- */
    if (cfg->boot_try >= max_try) {
        cfg->trial_slot     = OTA_SLOT_NONE;
        cfg->boot_try       = 0u;
        cfg->active_slot    = slot_clamp(cfg->active_slot);
        cfg->run_slot       = cfg->active_slot;
        cfg->pending_action = (uint8_t)OTA_ACT_NONE;
        cfg->move_progress  = 0u;

        *out_slot = cfg->run_slot;
        if (out_rolled != NULL) {
            *out_rolled = 1;
        }
        return 1;
    }

    /* ---- 还有次数：计数 +1，继续试 ---- */
    cfg->boot_try++;
    cfg->trial_slot = slot_clamp(cfg->trial_slot);
    cfg->run_slot   = cfg->trial_slot;

    *out_slot = cfg->run_slot;
    return 1;
}
