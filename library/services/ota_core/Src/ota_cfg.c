/**
 * @file    ota_cfg.c
 * @brief   状态参数区实现（双份乒乓 + 整体 CRC32）
 * @version V1.0
 * @date    2026-09-18
 */

#include <stdbool.h>
#include <string.h>
#include "ota_cfg.h"
#include "ota_crc32.h"

static const ota_area_t *s_area;      /* CFG 区 */
static ota_flash_t      *s_flash;     /* CFG 区所在介质（非 const：回调要 ctx） */
static uint32_t          s_half;      /* 每一份的大小 */
static uint32_t          s_off[2];    /* 两份在介质内的偏移 */
static uint8_t           s_next;      /* 下一份要写的份号 */
static bool              s_ready;

/* ---------------- 内部：读一份 ---------------- */

static bool copy_valid(const ota_cfg_t *c)
{
    uint32_t calc;

    if (c->magic != OTA_CFG_MAGIC) {
        return false;
    }
    if (c->struct_ver != OTA_CFG_STRUCT_VER) {
        return false;
    }
    if (c->size != (uint16_t)sizeof(ota_cfg_t)) {
        return false;
    }
    calc = ota_crc32(0u, c, OTA_CFG_CRC_LEN);
    return (calc == c->crc32);
}

static bool copy_read(uint8_t idx, ota_cfg_t *out)
{
    if (s_flash->read(s_flash->ctx, s_off[idx], out, (uint32_t)sizeof(ota_cfg_t)) != OTA_OK) {
        return false;
    }
    return copy_valid(out);
}

/* ---------------- 生命周期 ---------------- */

int ota_cfg_init(void)
{
    const ota_area_t *a = ota_area_role_at(OTA_AREA_ROLE_CFG, 0u);
    const ota_flash_t *f;
    uint32_t unit;

    if (a == NULL) {
        return OTA_ERR_NO_AREA;
    }
    f = ota_area_flash(a);
    if (f == NULL || ota_flash_check(f) != OTA_OK) {
        return OTA_ERR_NO_AREA;
    }
    if (a->size < OTA_CFG_MIN_AREA_SIZE || a->size < (2u * sizeof(ota_cfg_t))) {
        return OTA_ERR_NO_AREA;
    }

    s_half = (a->size / 2u) & ~3u;        /* 半区，且 4 字节对齐 */

    /* 半区必须是擦除单位的整数倍，否则擦第二份会连带擦掉第一份 */
    unit = ota_flash_erase_unit(f, a->base);
    if (unit == 0u || (s_half % unit) != 0u) {
        return OTA_ERR_NO_AREA;
    }
    if (s_half < (uint32_t)sizeof(ota_cfg_t)) {
        return OTA_ERR_NO_AREA;
    }

    s_area  = a;
    s_flash = (ota_flash_t *)f;
    s_off[0] = a->base;
    s_off[1] = a->base + s_half;
    s_next   = 0u;
    s_ready  = true;

    return OTA_OK;
}

uint32_t ota_cfg_area_size(void)
{
    return (s_area != NULL) ? s_area->size : 0u;
}

/* ---------------- 读写 ---------------- */

void ota_cfg_defaults(ota_cfg_t *out)
{
    if (out == NULL) {
        return;
    }

    (void)memset(out, 0, sizeof(*out));
    out->magic          = OTA_CFG_MAGIC;
    out->struct_ver     = OTA_CFG_STRUCT_VER;
    out->size           = (uint16_t)sizeof(ota_cfg_t);
    out->seq            = 0u;
    out->active_slot    = OTA_SLOT_A;
    out->trial_slot     = OTA_SLOT_NONE;
    out->boot_try       = 0u;
    out->run_slot       = OTA_SLOT_A;
    out->pending_action = (uint8_t)OTA_ACT_NONE;
    out->target_slot    = OTA_SLOT_NONE;
    out->stage_area     = OTA_AREA_ID_NONE;
    out->target_area    = OTA_AREA_ID_NONE;
    out->factory_flag   = OTA_CFG_FACTORY_MAGIC;
}

int ota_cfg_load(ota_cfg_t *out)
{
    ota_cfg_t c0;
    ota_cfg_t c1;
    bool v0;
    bool v1;

    if (out == NULL) {
        return OTA_ERR_PARAM;
    }
    if (!s_ready) {
        return OTA_ERR_STATE;
    }

    v0 = copy_read(0u, &c0);
    v1 = copy_read(1u, &c1);

    if (v0 && v1) {
        if (c0.seq >= c1.seq) {
            *out     = c0;
            s_next   = 1u;               /* 下次写另一份 */
        } else {
            *out     = c1;
            s_next   = 0u;
        }
        return OTA_OK;
    }
    if (v0) {
        *out   = c0;
        s_next = 1u;
        return OTA_OK;
    }
    if (v1) {
        *out   = c1;
        s_next = 0u;
        return OTA_OK;
    }

    return OTA_ERR_CFG_EMPTY;            /* 出厂态：两份都无效 */
}

int ota_cfg_load_or_default(ota_cfg_t *out)
{
    int rc = ota_cfg_load(out);

    if (rc == OTA_ERR_CFG_EMPTY || rc == OTA_ERR_STATE) {
        ota_cfg_defaults(out);
        return OTA_OK;
    }
    return rc;
}

int ota_cfg_save(ota_cfg_t *inout)
{
    ota_cfg_t cur;
    uint8_t   target;
    uint32_t  seq = 1u;

    if (inout == NULL) {
        return OTA_ERR_PARAM;
    }
    if (!s_ready) {
        return OTA_ERR_STATE;
    }

    /* seq 单调递增：比「当前有效的那份」大 1；无有效份则从 1 开始 */
    if (ota_cfg_load(&cur) == OTA_OK) {
        seq = cur.seq + 1u;
        if (seq == 0u) {
            seq = 1u;                    /* 回绕保护：不能让新记录看起来更旧 */
        }
    }
    /* 注意：ota_cfg_load() 已把 s_next 指向「另一份」，正是要写的目标 */
    target = s_next;

    inout->magic      = OTA_CFG_MAGIC;
    inout->struct_ver = OTA_CFG_STRUCT_VER;
    inout->size       = (uint16_t)sizeof(ota_cfg_t);
    inout->seq        = seq;
    inout->crc32      = ota_crc32(0u, inout, OTA_CFG_CRC_LEN);

    /* 先擦后写：擦的是「当前无效」那一份，有效份在此期间完好 */
    if (s_flash->erase(s_flash->ctx, s_off[target], s_half) != OTA_OK) {
        return OTA_ERR_MEDIA;
    }
    if (s_flash->write(s_flash->ctx, s_off[target], inout, (uint32_t)sizeof(ota_cfg_t)) != OTA_OK) {
        return OTA_ERR_MEDIA;
    }

    /* 回读确认落盘成功，再翻转下一份指针 */
    if (!copy_read(target, &cur)) {
        return OTA_ERR_MEDIA;
    }
    s_next = target ^ 1u;

    return OTA_OK;
}

/* ---------------- 便捷判定 ---------------- */

int ota_cfg_is_trial(const ota_cfg_t *c)
{
    if (c == NULL) {
        return 0;
    }
    return (c->trial_slot != OTA_SLOT_NONE) ? 1 : 0;
}

int ota_cfg_is_factory(const ota_cfg_t *c)
{
    if (c == NULL) {
        return 1;
    }
    return (c->factory_flag == OTA_CFG_FACTORY_MAGIC) ? 1 : 0;
}

uint8_t ota_cfg_run_slot(const ota_cfg_t *c)
{
    if (c == NULL || c->run_slot >= OTA_SLOT_COUNT) {
        return OTA_SLOT_A;
    }
    return c->run_slot;
}
