/**
 * @file    boot_activate.c
 * @brief   生效动作实现：SWITCH / MOVE
 * @version V1.0
 * @date    2026-09-18
 */

#include <stddef.h>
#include <string.h>
#include "boot_activate.h"
#include "ota_crc32.h"

/* ---------------- 内部工具 ---------------- */

static void act_fail(boot_act_t *a, ota_ret_t err)
{
    a->err = err;
    a->st  = BACT_FAILED;
}

static void act_prog(boot_act_t *a)
{
    if (a->prog != NULL) {
        a->prog(a->user, a->st, a->cur, a->total);
    }
}

/** @brief 解析 cfg 并填上下文（真正的准备工作） */
static int act_prepare(boot_act_t *a)
{
    ota_cfg_t *cfg = a->cfg;

    a->act  = (ota_act_t)cfg->pending_action;
    a->slot = OTA_SLOT_NONE;

    if (a->act == OTA_ACT_SWITCH) {
        const ota_area_t *t = ota_area_slot(cfg->target_slot);

        if (t == NULL) {
            return OTA_ERR_NO_AREA;
        }
        if (cfg->image_size == 0u) {
            return OTA_ERR_IMAGE;
        }
        if (cfg->image_size > t->size) {
            return OTA_ERR_NOSPACE;
        }

        a->chk   = t;
        a->dst   = t;
        a->slot  = cfg->target_slot;
        a->total = cfg->image_size;
        a->st    = BACT_VERIFY;
        return OTA_OK;
    }

    if (a->act == OTA_ACT_MOVE) {
        const ota_area_t *stage = ota_area_by_id(cfg->stage_area);
        const ota_area_t *run   = ota_area_by_id(cfg->target_area);

        if (stage == NULL || run == NULL) {
            return OTA_ERR_NO_AREA;
        }
        if (a->buf == NULL || a->buf_len < BOOT_ACT_MIN_BUF) {
            return OTA_ERR_PARAM;          /* MOVE 必须给搬运缓冲 */
        }
        if (cfg->image_size == 0u) {
            return OTA_ERR_IMAGE;
        }
        if (cfg->image_size > run->size || cfg->image_size > stage->size) {
            return OTA_ERR_NOSPACE;
        }
        if (run->slot_id >= OTA_SLOT_COUNT) {
            return OTA_ERR_NO_AREA;
        }

        a->chk   = stage;
        a->src   = stage;
        a->dst   = run;
        a->slot  = run->slot_id;
        a->total = cfg->image_size;
        a->st    = BACT_VERIFY;
        return OTA_OK;
    }

    return OTA_ERR_PARAM;
}

/** @brief 解析三处区所在介质（全部拿到才算准备好） */
static int act_resolve(boot_act_t *a)
{
    a->f_chk = ota_area_flash(a->chk);
    if (a->f_chk == NULL || ota_flash_check(a->f_chk) != OTA_OK) {
        return OTA_ERR_MEDIA;
    }
    if (a->src != NULL) {
        a->f_src = ota_area_flash(a->src);
        if (a->f_src == NULL || ota_flash_check(a->f_src) != OTA_OK) {
            return OTA_ERR_MEDIA;
        }
    }
    if (a->dst != NULL) {
        a->f_dst = ota_area_flash(a->dst);
        if (a->f_dst == NULL || ota_flash_check(a->f_dst) != OTA_OK) {
            return OTA_ERR_MEDIA;
        }
    }
    return OTA_OK;
}

/* ---------------- 各阶段 ---------------- */

/**
 * @brief 校验下载到位的镜像：读回 [区起点, 区起点 + image_size) 重算 CRC32
 * @note  这是「下载完但还没生效」时的最后一关。长度已由 image_size 固定，
 *        所以只需比对 CRC。
 */
static void step_verify(boot_act_t *a)
{
    uint32_t want;

    if (a->cur < a->total) {
        want = a->total - a->cur;
        if (want > a->buf_len) {
            want = a->buf_len;
        }
        if (a->f_chk->read(a->f_chk->ctx, a->chk->base + a->cur, a->buf, want) != OTA_OK) {
            act_fail(a, OTA_ERR_MEDIA);
            return;
        }
        a->crc = ota_crc32(a->crc, a->buf, want);
        a->cur += want;
        act_prog(a);
        return;
    }

    if (a->cfg->image_crc32 != 0u && a->crc != a->cfg->image_crc32) {
        act_fail(a, OTA_ERR_VERIFY);
        return;
    }

    /* 校验通过 → 分流。注意 cur / crc 要给下一阶段留干净的初值 */
    a->cur = 0u;
    if (a->act == OTA_ACT_SWITCH) {
        a->st = BACT_SWITCH_COMMIT;
    } else {
        a->crc      = 0u;
        a->unit_end = 0u;
        a->st       = BACT_MOVE_ERASE;
    }
    act_prog(a);
}

/** @brief MOVE：擦运行区（每次一个擦除单位，幂等，不留断点） */
static void step_move_erase(boot_act_t *a)
{
    uint32_t unit;

    if (a->cur >= a->total) {
        a->cur      = 0u;
        a->unit_end = 0u;
        a->st       = BACT_MOVE_COPY;
        act_prog(a);
        return;
    }

    /* 取一个**完整**擦除单位：erase 内部会向扇区边界扩展，给不足一个单位的话
     * 扩展出来的部分会落到运行区之外（擦坏邻居），所以这里必须补齐并按整单位擦。 */
    unit = ota_flash_erase_unit(a->f_dst, a->dst->base + a->cur);
    if (unit == 0u) {
        act_fail(a, OTA_ERR_MEDIA);
        return;
    }
    if ((a->cur + unit) > a->dst->size) {
        /* 运行区容量不是擦除单位整数倍（或在区尾附近收不了尾）。
         * 宁可拒绝生效，也不要把邻居区擦掉。 */
        act_fail(a, OTA_ERR_NOSPACE);
        return;
    }
    if (a->f_dst->erase(a->f_dst->ctx, a->dst->base + a->cur, unit) != OTA_OK) {
        act_fail(a, OTA_ERR_MEDIA);
        return;
    }

    a->cur += unit;
    act_prog(a);
}

/** @brief MOVE：逐块搬运（不跨擦除单位，便于按单位落盘断点） */
static void step_move_copy(boot_act_t *a)
{
    uint32_t want;

    if (a->cur >= a->total) {
        a->cur = 0u;
        a->crc = 0u;
        a->st  = BACT_MOVE_VERIFY;
        act_prog(a);
        return;
    }

    /* 每跨一个擦除单位就重算终点；到一个单位末尾就落盘一次进度 */
    if (a->unit_end == 0u) {
        uint32_t unit = ota_flash_erase_unit(a->f_dst, a->dst->base + a->cur);

        if (unit == 0u) {
            act_fail(a, OTA_ERR_MEDIA);
            return;
        }
        a->unit_end = a->cur + unit;
        if (a->unit_end > a->total) {
            a->unit_end = a->total;
        }
    }

    want = a->unit_end - a->cur;
    if (want > a->buf_len) {
        want = a->buf_len;
    }

    if (a->f_src->read(a->f_src->ctx, a->src->base + a->cur, a->buf, want) != OTA_OK) {
        act_fail(a, OTA_ERR_MEDIA);
        return;
    }
    if (a->f_dst->write(a->f_dst->ctx, a->dst->base + a->cur, a->buf, want) != OTA_OK) {
        act_fail(a, OTA_ERR_MEDIA);
        return;
    }
    a->cur += want;

    if (a->cur >= a->unit_end) {
        /* 先写数据、后写进度：掉电在这一步之前 → 该单位重搬；之后 → 从下一单位开始。
         * 两种结果都正确（源只读，重搬幂等）。进度写失败也不致命，只是多搬一点。 */
        a->cfg->move_progress = a->cur;
        (void)ota_cfg_save(a->cfg);
        a->unit_end = 0u;
    }

    act_prog(a);
}

/** @brief MOVE：读回运行区重算 CRC32 */
static void step_move_verify(boot_act_t *a)
{
    uint32_t want;

    if (a->cur < a->total) {
        want = a->total - a->cur;
        if (want > a->buf_len) {
            want = a->buf_len;
        }
        if (a->f_dst->read(a->f_dst->ctx, a->dst->base + a->cur, a->buf, want) != OTA_OK) {
            act_fail(a, OTA_ERR_MEDIA);
            return;
        }
        a->crc = ota_crc32(a->crc, a->buf, want);
        a->cur += want;
        act_prog(a);
        return;
    }

    if (a->cfg->image_crc32 != 0u && a->crc != a->cfg->image_crc32) {
        act_fail(a, OTA_ERR_VERIFY);
        return;
    }

    a->st = BACT_MOVE_COMMIT;
    act_prog(a);
}

/** @brief 两个分支共用的收尾：把状态区改成「新槽已生效、待试运行」 */
static void step_commit(boot_act_t *a)
{
    ota_cfg_t *c = a->cfg;

    c->active_slot    = a->slot;
    c->trial_slot     = a->slot;
    c->boot_try       = 0u;
    c->run_slot       = a->slot;
    c->pending_action = (uint8_t)OTA_ACT_NONE;
    c->move_progress  = 0u;

    if (ota_cfg_save(c) != OTA_OK) {
        act_fail(a, OTA_ERR_MEDIA);
        return;
    }

    a->st = BACT_DONE;
    act_prog(a);
}

/* ---------------- 对外接口 ---------------- */

int boot_act_start(boot_act_t *a, ota_cfg_t *cfg, uint8_t *buf, uint32_t buf_len,
                   boot_act_prog_cb prog, void *user)
{
    int rc;

    if (a == NULL || cfg == NULL) {
        return OTA_ERR_PARAM;
    }

    (void)memset(a, 0u, sizeof(*a));
    a->cfg     = cfg;
    a->buf     = buf;
    a->buf_len = buf_len;
    a->prog    = prog;
    a->user    = user;
    a->err     = OTA_OK;
    a->slot    = OTA_SLOT_NONE;

    rc = act_prepare(a);
    if (rc == OTA_OK) {
        rc = act_resolve(a);
    }
    if (rc != OTA_OK) {
        a->st = BACT_FAILED;               /* 区分「未启动」与「启动失败」 */
        return rc;
    }
    return OTA_OK;
}

int boot_act_step(boot_act_t *a)
{
    if (a == NULL) {
        return -1;
    }

    switch (a->st) {
    case BACT_VERIFY:       step_verify(a);      break;
    case BACT_SWITCH_COMMIT:
    case BACT_MOVE_COMMIT:  step_commit(a);      break;
    case BACT_MOVE_ERASE:   step_move_erase(a);  break;
    case BACT_MOVE_COPY:    step_move_copy(a);   break;
    case BACT_MOVE_VERIFY:  step_move_verify(a); break;
    case BACT_DONE:         return 1;
    case BACT_FAILED:       return -1;
    default:
        act_fail(a, OTA_ERR_STATE);
        return -1;
    }

    if (a->st == BACT_FAILED) {
        return -1;
    }
    if (a->st == BACT_DONE) {
        return 1;
    }
    return 0;
}

const char *boot_act_st_name(boot_act_st_t s)
{
    switch (s) {
    case BACT_IDLE:        return "idle";
    case BACT_VERIFY:      return "verify-image";
    case BACT_SWITCH_COMMIT: return "switch-commit";
    case BACT_MOVE_ERASE:  return "move-erase";
    case BACT_MOVE_COPY:   return "move-copy";
    case BACT_MOVE_VERIFY: return "move-verify";
    case BACT_MOVE_COMMIT: return "move-commit";
    case BACT_DONE:        return "done";
    case BACT_FAILED:      return "failed";
    default:               return "?";
    }
}
