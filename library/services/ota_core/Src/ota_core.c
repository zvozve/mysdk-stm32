/**
 * @file    ota_core.c
 * @brief   ota_core 入口实现
 * @version V1.0
 * @date    2026-09-18
 */

#include "ota_core.h"

int ota_init(const ota_env_t *env)
{
    int rc;

    if (env == NULL || env->areas == NULL || env->flashes == NULL) {
        return OTA_ERR_PARAM;
    }

    rc = ota_area_init(env->areas, env->area_count, env->flashes, env->flash_count);
    if (rc != OTA_OK) {
        return rc;
    }

    return ota_cfg_init();
}

uint32_t ota_core_image_max(void)
{
    uint8_t          n = ota_area_run_count();
    uint32_t         max = 0u;

    if (n == 0u) {
        return 0u;
    }
    for (uint8_t i = 0u; i < n; i++) {
        const ota_area_t *a = ota_area_role_at(OTA_AREA_ROLE_RUN, i);
        if (a == NULL) {
            continue;
        }
        if (max == 0u || a->size < max) {
            max = a->size;                 /* 取最小者：镜像必须能放进任一可运行槽 */
        }
    }
    return max;
}

int ota_core_is_ab(void)
{
    return (ota_area_run_count() == 2u) ? 1 : 0;
}

void ota_core_default_cfg(ota_cfg_t *out)
{
    ota_cfg_defaults(out);
}
