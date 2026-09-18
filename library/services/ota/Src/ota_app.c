/**
 * @file    ota_app.c
 * @brief   OTA APP 外壳实现
 * @version V1.0
 * @date    2026-09-18
 */

#include <stddef.h>
#include <string.h>
#include "ota_app.h"
#include "oop_boot.h"

/* ---------------- 内部 ---------------- */

/** @brief 把 ota_flow 的上报转给上层注册的回调 */
static void flow_report_bridge(ota_flow_t *f, void *user)
{
    ota_app_t *a = (ota_app_t *)user;

    if (a != NULL && a->report != NULL) {
        a->report(f, a->report_user);
    }
}

/* ---------------- 早期初始化 ---------------- */

int ota_app_early_init(ota_app_t *a, uint32_t self_base)
{
    if (a == NULL || self_base == 0u) {
        return OTA_ERR_PARAM;
    }

    (void)memset(a, 0u, sizeof(*a));
    a->self_base = self_base;
    a->self_slot = OTA_SLOT_NONE;

    /* 只做两件事，所以能在 HAL_Init 之前跑：
     *   ① 向量表指向自己 —— 从 BL 跳过来时 VTOR 还指着对方，不改的话
     *      每一个中断都会跳到另一个槽的 handler（同一份源码两次链接，行为"看起来正常"）
     *   ② 开中断 —— BL 跳转前已 cpsie i，这里是兜底（调试器改 PC 等方式进来时）
     */
    oop_boot_set_vtor(self_base);
    __enable_irq();
    return OTA_OK;
}

uint8_t ota_app_self_slot(const ota_app_t *a)
{
    if (a == NULL || a->self_base == 0u) {
        return OTA_SLOT_NONE;
    }

    for (uint8_t s = 0u; s < OTA_SLOT_COUNT; s++) {
        const ota_area_t *ar = ota_area_slot(s);

        if (ar != NULL && ota_area_cpu_addr(ar) == a->self_base) {
            return s;
        }
    }
    return OTA_SLOT_NONE;
}

/* ---------------- 升级流程 ---------------- */

int ota_app_start(ota_app_t *a, ota_source_t *src, uint8_t *buf, uint32_t buf_len)
{
    uint8_t slot;
    int     rc;

    if (a == NULL || src == NULL || buf == NULL) {
        return OTA_ERR_PARAM;
    }
    if (a->busy != 0u) {
        return OTA_ERR_STATE;
    }

    slot = ota_app_self_slot(a);
    if (slot == OTA_SLOT_NONE) {
        /* 链接基址不在任何运行槽里 —— 分区表或 -DOTA_SELF_BASE 写错了。
         * 这时候错的是「我们以为自己是谁」，必须先修，不能继续。 */
        return OTA_ERR_NO_AREA;
    }
    a->self_slot = slot;

    a->fcfg.source       = src;
    a->fcfg.buf          = buf;
    a->fcfg.buf_len      = buf_len;
    a->fcfg.running_slot = slot;
    a->fcfg.report       = flow_report_bridge;
    a->fcfg.user         = a;

    rc = ota_flow_start(&a->flow, &a->fcfg);
    if (rc != OTA_OK) {
        return rc;
    }

    a->busy = 1u;
    return OTA_OK;
}

int ota_app_process(ota_app_t *a)
{
    ota_flow_ret_t r;

    if (a == NULL) {
        return OTA_APP_ERR;
    }
    if (a->busy == 0u) {
        return OTA_APP_IDLE;
    }

    r = ota_flow_step(&a->flow);
    if (r == OTA_FLOW_OK) {
        a->busy = 0u;
        return OTA_APP_DONE;
    }
    if (r == OTA_FLOW_ERR) {
        a->busy = 0u;
        return OTA_APP_ERR;
    }
    return OTA_APP_BUSY;
}

int ota_app_abort(ota_app_t *a)
{
    if (a == NULL) {
        return OTA_ERR_PARAM;
    }
    if (a->busy == 0u) {
        return OTA_ERR_STATE;
    }

    (void)ota_flow_abort(&a->flow);
    a->busy = 0u;
    return OTA_OK;
}

uint8_t ota_app_progress(const ota_app_t *a)
{
    if (a == NULL || a->busy == 0u) {
        return 0u;
    }
    return ota_flow_progress(&a->flow);
}

ota_flow_state_t ota_app_state(const ota_app_t *a)
{
    return (a != NULL) ? a->flow.state : OTA_FLOW_IDLE;
}

/* ---------------- 试运行确认 ---------------- */

int ota_app_is_trial(const ota_app_t *a)
{
    ota_cfg_t c;
    uint8_t   slot;

    if (a == NULL) {
        return 0;
    }
    if (ota_cfg_load(&c) != OTA_OK) {
        return 0;                       /* 状态区不可读 → 不声称自己在试运行 */
    }
    if (c.trial_slot == OTA_SLOT_NONE) {
        return 0;
    }

    slot = ota_app_self_slot(a);
    if (slot == OTA_SLOT_NONE) {
        return 0;
    }
    return (c.trial_slot == slot) ? 1 : 0;
}

int ota_app_confirm(ota_app_t *a)
{
    ota_cfg_t c;
    uint8_t   slot;
    int       rc;

    if (a == NULL) {
        return OTA_ERR_PARAM;
    }

    slot = ota_app_self_slot(a);
    if (slot == OTA_SLOT_NONE) {
        return OTA_ERR_NO_AREA;
    }

    rc = ota_cfg_load_or_default(&c);
    if (rc != OTA_OK) {
        return rc;
    }

    c.active_slot    = slot;
    c.trial_slot     = OTA_SLOT_NONE;
    c.boot_try       = 0u;
    c.run_slot       = slot;
    c.pending_action = (uint8_t)OTA_ACT_NONE;
    c.move_progress  = 0u;
    c.factory_flag   = 0u;              /* 状态被真正提交过，不再是出厂态 */

    return ota_cfg_save(&c);
}

void ota_app_set_health_cb(ota_app_t *a, ota_app_health_cb cb, void *user)
{
    if (a == NULL) {
        return;
    }
    a->health      = cb;
    a->health_user = user;
}

int ota_app_trial_check(ota_app_t *a)
{
    if (a == NULL) {
        return OTA_ERR_PARAM;
    }
    if (a->health == NULL) {
        return OTA_OK;                  /* 没注册自检 → 不自动确认，等人工调 confirm */
    }
    if (ota_app_is_trial(a) == 0) {
        return OTA_OK;                  /* 不在试运行 → 什么都不做 */
    }
    if (a->health(a->health_user) == 0) {
        return OTA_OK;                  /* 还没通过，下次再说 */
    }
    return ota_app_confirm(a);
}

/* ---------------- 杂项 ---------------- */

void ota_app_set_report_cb(ota_app_t *a, ota_app_report_cb cb, void *user)
{
    if (a == NULL) {
        return;
    }
    a->report      = cb;
    a->report_user = user;
}

void ota_app_reboot(void)
{
    oop_boot_system_reset();
}
