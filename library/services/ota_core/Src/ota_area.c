/**
 * @file    ota_area.c
 * @brief   分区表注册 / 查询 / 目标区选择
 * @version V1.0
 * @date    2026-09-18
 */

#include <string.h>
#include "ota_area.h"

static const ota_area_t  *s_areas;
static uint8_t            s_area_count;
static const ota_flash_t *s_flashes;
static uint8_t            s_flash_count;

/* ---------------- 注册 ---------------- */

int ota_area_validate(void)
{
    if (s_areas == NULL || s_area_count == 0u || s_flashes == NULL || s_flash_count == 0u) {
        return OTA_ERR_NO_AREA;
    }

    for (uint8_t i = 0u; i < s_area_count; i++) {
        const ota_area_t  *a = &s_areas[i];
        const ota_flash_t *f;

        if (a->flash_id >= s_flash_count) {
            return OTA_ERR_NO_AREA;
        }
        f = &s_flashes[a->flash_id];
        if (ota_flash_check(f) != OTA_OK) {
            return OTA_ERR_MEDIA;
        }
        if (a->size == 0u) {
            return OTA_ERR_NO_AREA;
        }
        if ((a->base + a->size) > f->size) {          /* 越出介质容量 */
            return OTA_ERR_NO_AREA;
        }

        /* 同一介质内不允许重叠 */
        for (uint8_t j = 0u; j < i; j++) {
            const ota_area_t *b = &s_areas[j];
            if (b->flash_id != a->flash_id) {
                continue;
            }
            if (a->base < (b->base + b->size) && b->base < (a->base + a->size)) {
                return OTA_ERR_NO_AREA;
            }
        }
    }

    /* 运行槽号不得重复、且必须在 0..OTA_SLOT_COUNT-1 */
    for (uint8_t i = 0u; i < s_area_count; i++) {
        if (s_areas[i].role != OTA_AREA_ROLE_RUN) {
            continue;
        }
        if (s_areas[i].slot_id >= OTA_SLOT_COUNT) {
            return OTA_ERR_NO_AREA;
        }
        for (uint8_t j = 0u; j < i; j++) {
            if (s_areas[j].role == OTA_AREA_ROLE_RUN &&
                s_areas[j].slot_id == s_areas[i].slot_id) {
                return OTA_ERR_NO_AREA;
            }
        }
    }

    return OTA_OK;
}

int ota_area_init(const ota_area_t *areas, uint8_t area_count,
                  const ota_flash_t *flashes, uint8_t flash_count)
{
    if (areas == NULL || area_count == 0u || flashes == NULL || flash_count == 0u) {
        return OTA_ERR_PARAM;
    }

    s_areas       = areas;
    s_area_count  = area_count;
    s_flashes     = flashes;
    s_flash_count = flash_count;

    return ota_area_validate();
}

/* ---------------- 查询 ---------------- */

uint8_t ota_area_count_role(ota_area_role_t role)
{
    uint8_t n = 0u;

    for (uint8_t i = 0u; i < s_area_count; i++) {
        if (s_areas[i].role == role) {
            n++;
        }
    }
    return n;
}

const ota_area_t *ota_area_role_at(ota_area_role_t role, uint8_t nth)
{
    uint8_t k = 0u;

    for (uint8_t i = 0u; i < s_area_count; i++) {
        if (s_areas[i].role == role) {
            if (k == nth) {
                return &s_areas[i];
            }
            k++;
        }
    }
    return (const ota_area_t *)0;
}

const ota_area_t *ota_area_by_id(uint8_t id)
{
    if (s_areas == NULL || id >= s_area_count) {
        return (const ota_area_t *)0;
    }
    return &s_areas[id];
}

uint8_t ota_area_id(const ota_area_t *a)
{
    if (a == NULL || s_areas == NULL) {
        return OTA_AREA_ID_NONE;
    }
    if (a < s_areas || a >= (s_areas + s_area_count)) {
        return OTA_AREA_ID_NONE;
    }
    return (uint8_t)(a - s_areas);
}

const ota_flash_t *ota_area_flash(const ota_area_t *a)
{
    if (a == NULL || s_flashes == NULL || a->flash_id >= s_flash_count) {
        return (const ota_flash_t *)0;
    }
    return &s_flashes[a->flash_id];
}

uint32_t ota_area_cpu_addr(const ota_area_t *a)
{
    const ota_flash_t *f = ota_area_flash(a);

    if (a == NULL || f == NULL) {
        return 0u;
    }
    if (f->base_addr == 0u) {
        return 0u;                       /* 不可直接寻址（外挂 SPI NOR） */
    }
    return f->base_addr + a->base;       /* 介质内偏移 -> CPU 绝对地址 */
}

const ota_area_t *ota_area_slot(uint8_t slot_id)
{
    for (uint8_t i = 0u; i < s_area_count; i++) {
        if (s_areas[i].role == OTA_AREA_ROLE_RUN && s_areas[i].slot_id == slot_id) {
            return &s_areas[i];
        }
    }
    return (const ota_area_t *)0;
}

uint8_t ota_area_run_count(void)
{
    return ota_area_count_role(OTA_AREA_ROLE_RUN);
}

/* ---------------- 目标区选择 ---------------- */

int ota_area_select_target(uint8_t running_slot, ota_target_t *out)
{
    uint8_t runs;

    if (out == NULL) {
        return OTA_ERR_PARAM;
    }

    (void)memset(out, 0, sizeof(*out));
    out->act          = OTA_ACT_NONE;
    out->target_slot  = OTA_SLOT_NONE;
    out->target_stage = OTA_AREA_ID_NONE;

    runs = ota_area_run_count();

    if (runs == 2u) {
        /* 内部 A/B：目标 = 另一个运行槽，生效方式是「改 4 个字节」 */
        uint8_t other = (running_slot == OTA_SLOT_A) ? OTA_SLOT_B : OTA_SLOT_A;
        const ota_area_t *a = ota_area_slot(other);

        if (a == NULL) {
            return OTA_ERR_NO_AREA;
        }
        out->act          = OTA_ACT_SWITCH;
        out->target       = a;
        out->run_target   = a;
        out->target_slot  = other;
        return OTA_OK;
    }

    if (runs == 1u) {
        /* 单运行槽 + 暂存：目标 = 第一个 STAGE 段，生效方式是「BL 逐扇区搬」 */
        const ota_area_t *stage = ota_area_role_at(OTA_AREA_ROLE_STAGE, 0u);
        const ota_area_t *run   = ota_area_role_at(OTA_AREA_ROLE_RUN, 0u);

        if (stage == NULL || run == NULL) {
            return OTA_ERR_NO_AREA;
        }
        out->act          = OTA_ACT_MOVE;
        out->target       = stage;
        out->run_target   = run;
        out->target_slot  = OTA_SLOT_NONE;
        out->target_stage = ota_area_id(stage);
        return OTA_OK;
    }

    return OTA_ERR_NO_AREA;
}
