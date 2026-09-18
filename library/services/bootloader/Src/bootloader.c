/**
 * @file    bootloader.c
 * @brief   BL 启动决策状态机
 * @version V1.0
 * @date    2026-09-18
 */

#include <stddef.h>
#include <string.h>
#include "bootloader.h"
#include "boot_health.h"
#include "oop_boot.h"
#include "oop_flash_drv.h"

static const boot_cfg_t *s_cfg;

/* ---------------- 内部工具 ---------------- */

static void report(boot_ctx_t *ctx, boot_event_t ev, uint32_t arg)
{
    (void)ctx;
    if (s_cfg != NULL && s_cfg->report != NULL) {
        s_cfg->report(ev, arg, s_cfg->user);
    }
}

static uint8_t max_try(void)
{
    if (s_cfg != NULL && s_cfg->max_try != 0u) {
        return s_cfg->max_try;
    }
    return BOOT_DEFAULT_MAX_TRY;
}

/** @brief 生效失败后的收场：清待生效标记，回落已知好槽 */
static void act_give_up(boot_ctx_t *ctx)
{
    ctx->cfg.pending_action = (uint8_t)OTA_ACT_NONE;
    ctx->cfg.move_progress  = 0u;
    ctx->cfg.trial_slot     = OTA_SLOT_NONE;   /* 镜像没能生效，试运行无从谈起 */
    ctx->cfg.boot_try       = 0u;
    ctx->cfg.active_slot    = (ctx->cfg.active_slot < OTA_SLOT_COUNT)
                              ? ctx->cfg.active_slot : (uint8_t)OTA_SLOT_A;
    ctx->cfg.run_slot       = ctx->cfg.active_slot;
    (void)ota_cfg_save(&ctx->cfg);
}

static void act_prog(void *user, boot_act_st_t st, uint32_t done, uint32_t total)
{
    (void)total;
    if (st == BACT_MOVE_ERASE || st == BACT_MOVE_COPY || st == BACT_MOVE_VERIFY) {
        report((boot_ctx_t *)user, BOOT_EV_MOVE_PROGRESS, done);
    }
}

/* ---------------- 各阶段 ---------------- */

static boot_step_ret_t st_cfg(boot_ctx_t *ctx)
{
    int rc = ota_cfg_load(&ctx->cfg);

    if (rc == OTA_ERR_CFG_EMPTY) {
        /* 出厂态：两份都坏。用默认值继续，且**不落盘** —— 没东西可写之前不碰 Flash，
         * 否则每次上电都会白擦一次状态区。factory_flag 会一直保持出厂值，
         * 这正是「从未成功提交过状态」的正确表达。 */
        ota_core_default_cfg(&ctx->cfg);
        report(ctx, BOOT_EV_CFG_FACTORY, 0u);
    } else if (rc != OTA_OK) {
        ctx->err   = (ota_ret_t)rc;
        ctx->state = BOOT_ST_FAILED;
        return BOOT_STEP_ERR;
    }

    ctx->state = BOOT_ST_ACTIVATE;
    return BOOT_STEP_BUSY;
}

static boot_step_ret_t st_activate(boot_ctx_t *ctx)
{
    int r;

    if (ctx->cfg.pending_action == (uint8_t)OTA_ACT_NONE) {
        ctx->state = BOOT_ST_HEALTH;
        return BOOT_STEP_BUSY;
    }

    if (ctx->act.st == BACT_IDLE) {
        r = boot_act_start(&ctx->act, &ctx->cfg,
                           s_cfg->buf, s_cfg->buf_len, act_prog, ctx);
        if (r != OTA_OK) {
            report(ctx, BOOT_EV_ACT_FAIL, (uint32_t)(-r));
            act_give_up(ctx);
            ctx->state = BOOT_ST_HEALTH;
            return BOOT_STEP_BUSY;
        }
        if (ctx->act.act == OTA_ACT_SWITCH) {
            report(ctx, BOOT_EV_ACTIVATE_SWITCH, ctx->act.slot);
        } else {
            report(ctx, BOOT_EV_ACTIVATE_MOVE, ctx->act.total);
        }
    }

    r = boot_act_step(&ctx->act);
    if (r > 0) {
        ctx->state = BOOT_ST_HEALTH;
        return BOOT_STEP_BUSY;
    }
    if (r < 0) {
        report(ctx, BOOT_EV_ACT_FAIL, (uint32_t)(-(int)ctx->act.err));
        act_give_up(ctx);
        ctx->state = BOOT_ST_HEALTH;
        return BOOT_STEP_BUSY;
    }
    return BOOT_STEP_BUSY;
}

static boot_step_ret_t st_health(boot_ctx_t *ctx)
{
    uint8_t slot   = (uint8_t)OTA_SLOT_A;
    int     rolled = 0;
    int     r;

    r = boot_health_apply(&ctx->cfg, max_try(), &slot, &rolled);
    if (r < 0) {
        ctx->err   = OTA_ERR_PARAM;
        ctx->state = BOOT_ST_FAILED;
        return BOOT_STEP_ERR;
    }

    if (r > 0) {
        if (rolled != 0) {
            report(ctx, BOOT_EV_ROLLBACK, slot);
        } else {
            report(ctx, BOOT_EV_TRIAL, ctx->cfg.boot_try);
        }

        /* 计数必须在跳转前落盘，否则每次复位都从 0 开始，回滚机制等于不存在 */
        if (ota_cfg_save(&ctx->cfg) != OTA_OK) {
            report(ctx, BOOT_EV_CFG_FAIL, 0u);
            /* 状态写不进去 → 计数回滚失效。保守起见不去跑没确认过的新槽，
             * 直接退回 active_slot（已知好）。 */
            if (slot == ctx->cfg.trial_slot) {
                slot = (ctx->cfg.active_slot < OTA_SLOT_COUNT)
                       ? ctx->cfg.active_slot : (uint8_t)OTA_SLOT_A;
            }
        }
    }

    if (rolled != 0) {
        ctx->rolled_back = 1u;
    }
    ctx->target_slot = slot;
    ctx->state       = BOOT_ST_VERIFY;
    return BOOT_STEP_BUSY;
}

static boot_step_ret_t st_verify(boot_ctx_t *ctx)
{
    const ota_area_t *a;
    uint32_t          vec;

    a = ota_area_slot(ctx->target_slot);
    if (a != NULL) {
        vec = ota_area_cpu_addr(a);
        if (vec != 0u && oop_boot_vector_check(vec, NULL, NULL) == OOP_BOOT_OK) {
            ctx->state = BOOT_ST_READY;
            return BOOT_STEP_READY;
        }
    }
    report(ctx, BOOT_EV_SLOT_BAD, ctx->target_slot);

    /* 目标槽不可跳 → 看看另一个运行槽（只有 A/B 拓扑才有第二个）。
     * 这里**不**做完整镜像 CRC：旧槽是上次跑通过的，结构合法就够，
     * 每上电全扫一遍几百 KB 换不来额外保障。 */
    for (uint8_t s = 0u; s < OTA_SLOT_COUNT; s++) {
        const ota_area_t *b;

        if (s == ctx->target_slot) {
            continue;
        }
        b = ota_area_slot(s);
        if (b == NULL) {
            continue;
        }
        vec = ota_area_cpu_addr(b);
        if (vec == 0u || oop_boot_vector_check(vec, NULL, NULL) != OOP_BOOT_OK) {
            continue;
        }

        /* 降级到另一个槽：把状态区一并改过去，免得下次上电又走一遍这条路径 */
        ctx->target_slot        = s;
        ctx->cfg.active_slot    = s;
        ctx->cfg.trial_slot     = OTA_SLOT_NONE;
        ctx->cfg.boot_try       = 0u;
        ctx->cfg.run_slot       = s;
        ctx->cfg.pending_action = (uint8_t)OTA_ACT_NONE;
        ctx->cfg.move_progress  = 0u;
        if (ota_cfg_save(&ctx->cfg) != OTA_OK) {
            report(ctx, BOOT_EV_CFG_FAIL, 0u);
        }
        ctx->rolled_back = 1u;
        report(ctx, BOOT_EV_ROLLBACK, s);

        ctx->state = BOOT_ST_READY;
        return BOOT_STEP_READY;
    }

    ctx->err   = OTA_ERR_NO_AREA;
    ctx->state = BOOT_ST_RESCUE;
    report(ctx, BOOT_EV_RESCUE, 0u);
    return BOOT_STEP_RESCUE;
}

/* ---------------- 对外接口 ---------------- */

int boot_init(const boot_cfg_t *cfg)
{
    const ota_area_t       *b;
    const oop_flash_info_t *fi;
    uint32_t                guard_begin;

    if (cfg == NULL) {
        return OTA_ERR_PARAM;
    }
    if (cfg->buf != NULL && cfg->buf_len < BOOT_MIN_MOVE_BUF) {
        return OTA_ERR_PARAM;
    }
    if (ota_area_run_count() == 0u) {
        return OTA_ERR_NO_AREA;            /* 还没 ota_init() */
    }

    /* 安全闸：闸门语义是「允许擦写的窗口」，所以从 BOOT 区末尾起、到内部 Flash 末尾。
     * 之后无论上层地址怎么算错，都擦不到 BL 自己。 */
    b = ota_area_role_at(OTA_AREA_ROLE_BOOT, 0u);
    if (b == NULL) {
        return OTA_ERR_NO_AREA;            /* BL 分区未登记 → 无法自我保护，拒绝启动 */
    }
    guard_begin = ota_area_cpu_addr(b);
    if (guard_begin == 0u) {
        return OTA_ERR_NO_AREA;
    }
    guard_begin += b->size;

    fi = oop_flash_info();
    if (fi->size == 0u) {
        return OTA_ERR_MEDIA;
    }
    if (guard_begin >= (fi->base + fi->size)) {
        return OTA_ERR_NO_AREA;
    }
    oop_flash_set_guard(guard_begin, fi->base + fi->size);

    s_cfg = cfg;
    return OTA_OK;
}

void boot_start(boot_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    (void)memset(ctx, 0u, sizeof(*ctx));
    ctx->state       = BOOT_ST_CFG;
    ctx->err         = OTA_OK;
    ctx->target_slot = OTA_SLOT_NONE;
    ctx->act.st      = BACT_IDLE;
}

boot_step_ret_t boot_step(boot_ctx_t *ctx)
{
    if (ctx == NULL || s_cfg == NULL) {
        return BOOT_STEP_ERR;
    }

    switch (ctx->state) {
    case BOOT_ST_IDLE:
    case BOOT_ST_CFG:      return st_cfg(ctx);
    case BOOT_ST_ACTIVATE: return st_activate(ctx);
    case BOOT_ST_HEALTH:   return st_health(ctx);
    case BOOT_ST_VERIFY:   return st_verify(ctx);
    case BOOT_ST_READY:    return BOOT_STEP_READY;
    case BOOT_ST_RESCUE:   return BOOT_STEP_RESCUE;
    default:               return BOOT_STEP_ERR;
    }
}

int boot_jump(boot_ctx_t *ctx)
{
    const ota_area_t *a;
    uint32_t          vec;

    if (ctx == NULL || s_cfg == NULL) {
        return OTA_ERR_PARAM;
    }
    if (ctx->state != BOOT_ST_READY) {
        return OTA_ERR_STATE;
    }

    a = ota_area_slot(ctx->target_slot);
    if (a == NULL) {
        return OTA_ERR_NO_AREA;
    }
    vec = ota_area_cpu_addr(a);
    if (vec == 0u) {
        return OTA_ERR_NO_AREA;
    }

    /* run_slot 只在变化时写：已确认状态下正常开机是零擦写（保护 Flash 寿命）。
     * 写失败也照跳，只上报。 */
    if (ctx->cfg.run_slot != ctx->target_slot) {
        ctx->cfg.run_slot = ctx->target_slot;
        if (ota_cfg_save(&ctx->cfg) != OTA_OK) {
            report(ctx, BOOT_EV_CFG_FAIL, 0u);
        }
    }

    report(ctx, BOOT_EV_JUMP, ctx->target_slot);
    return oop_boot_jump(vec);             /* 成功不返回 */
}

const char *boot_state_name(boot_state_t s)
{
    switch (s) {
    case BOOT_ST_IDLE:     return "idle";
    case BOOT_ST_CFG:      return "cfg";
    case BOOT_ST_ACTIVATE: return "activate";
    case BOOT_ST_HEALTH:   return "health";
    case BOOT_ST_VERIFY:   return "verify";
    case BOOT_ST_READY:    return "ready";
    case BOOT_ST_RESCUE:   return "rescue";
    case BOOT_ST_FAILED:   return "failed";
    default:               return "?";
    }
}
