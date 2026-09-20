/**
 * @file    ota_flow.c
 * @brief   流程骨架实现（非阻塞步进式）
 * @version V1.0
 * @date    2026-09-18
 */

#include <string.h>
#include "ota_flow.h"

/* ---------------- 内部工具 ---------------- */

static void flow_fail(ota_flow_t *f, ota_ret_t err)
{
    f->err   = err;
    f->state = OTA_FLOW_FAILED;

    if (f->cfg != NULL && f->cfg->source != NULL && f->cfg->source->close != NULL) {
        f->cfg->source->close(f->cfg->source->ctx);
    }
}

static void flow_report(ota_flow_t *f)
{
    if (f->cfg != NULL && f->cfg->report != NULL) {
        f->cfg->report(f, f->cfg->user);
    }
}

/** @brief 把流定位到 off（能 seek 就 seek，否则顺序丢弃） */
static ota_ret_t flow_seek_to(ota_flow_t *f, uint32_t off)
{
    ota_source_t *s = f->cfg->source;

    if (s->seek != NULL) {
        if (s->seek(s->ctx, off) != OTA_OK) {
            return OTA_ERR_SOURCE;
        }
        f->bytes_in = off;
        return OTA_OK;
    }

    if (f->bytes_in > off) {
        return OTA_ERR_UNSUPPORTED;      /* 流式通道不能回退 */
    }

    while (f->bytes_in < off) {
        uint32_t want = off - f->bytes_in;
        uint32_t got  = 0u;

        if (want > f->cfg->buf_len) {
            want = f->cfg->buf_len;
        }
        if (s->read(s->ctx, f->cfg->buf, want, &got) != OTA_OK || got == 0u) {
            return OTA_ERR_SOURCE;
        }
        f->bytes_in += got;
    }
    return OTA_OK;
}

/* ---------------- 各状态 ---------------- */

static void do_open(ota_flow_t *f)
{
    ota_src_info_t info;

    (void)memset(&info, 0, sizeof(info));

    if (f->cfg->source->open(f->cfg->source->ctx, &info) != OTA_OK) {
        flow_fail(f, OTA_ERR_SOURCE);
        return;
    }
    if (info.fw_ver != 0u) {
        f->fw_ver = info.fw_ver;
    }
    f->src_total = info.total_size;

    f->state = OTA_FLOW_HDR;
    flow_report(f);
}

static void do_hdr(ota_flow_t *f)
{
    uint32_t done = 0u;

    /* 无论 otapkg 还是裸 bin，都先读满 OTA_PKG_HDR_SIZE 字节：
     * - otapkg：这正是 80 字节包头，解析后丢弃；
     * - 裸 bin：这 80 字节就是镜像头部，必须原样写回（见 do_write 的 pending 缓冲）。 */
    while (done < OTA_PKG_HDR_SIZE) {
        uint32_t got = 0u;

        if (f->cfg->source->read(f->cfg->source->ctx,
                                 f->cfg->buf + done,
                                 OTA_PKG_HDR_SIZE - done, &got) != OTA_OK ||
            got == 0u) {
            flow_fail(f, OTA_ERR_SOURCE);
            return;
        }
        done += got;
    }
    f->bytes_in += done;

    {
        const ota_pkg_hdr_t *hp = (const ota_pkg_hdr_t *)f->cfg->buf;

        if (hp->magic == OTA_PKG_MAGIC) {
            /* ---- .otapkg 旧格式 ---- */
            f->raw = 0u;
            if (ota_image_parse(f->cfg->buf, done, &f->hdr) != OTA_OK) {
                flow_fail(f, OTA_ERR_IMAGE);
                return;
            }
            if (f->hdr.fw_ver != 0u) {
                f->fw_ver = f->hdr.fw_ver;
            }
        } else {
            /* ---- 裸 bin：没有包头，整段即镜像 ---- */
            f->raw = 1u;
            (void)memset(&f->hdr, 0, sizeof(f->hdr));
            (void)memcpy(f->pending, f->cfg->buf, done);
            f->pending_len = done;
            f->pending_off = 0u;
        }
    }

    f->state = OTA_FLOW_DECIDE;
    flow_report(f);
}

static void do_decide(ota_flow_t *f)
{
    const ota_seg_t  *seg = (const ota_seg_t *)0;
    const ota_area_t *run;
    uint32_t          load_addr;
    uint32_t          unit;
    uint32_t          area_end;
    ota_ret_t         rc;

    rc = ota_area_select_target(f->cfg->running_slot, &f->tgt);
    if (rc != OTA_OK) {
        flow_fail(f, rc);
        return;
    }

    /* 段表校验用「最终运行区」的 CPU 地址：MOVE 拓扑下镜像仍是按运行槽地址链接的
     * （这正是 MOVE 保留单一链接地址的原因） */
    run = f->tgt.run_target;
    load_addr = ota_area_cpu_addr(run);
    if (load_addr == 0u) {
        flow_fail(f, OTA_ERR_UNSUPPORTED);
        return;
    }

    if (f->raw) {
        /* 裸 bin：整段即镜像，从偏移 0 起，长度 = 源声明的总字节数 */
        if (f->src_total == 0u) {
            flow_fail(f, OTA_ERR_IMAGE);     /* 流式源拿不到长度，无法定位结束 */
            return;
        }
        f->seg.load_addr = load_addr;
        f->seg.size      = f->src_total;
        f->seg.crc32     = 0u;              /* 无外部 CRC；第 2 关改为「内存 CRC == 闪存 CRC」 */
        f->data_off      = 0u;
    } else {
        rc = ota_image_find_seg(&f->hdr, load_addr, &seg, &f->data_off);
        if (rc != OTA_OK) {
            flow_fail(f, rc);
            return;
        }
        f->seg = *seg;
    }

    if (f->seg.size > f->tgt.target->size) {
        flow_fail(f, OTA_ERR_NOSPACE);
        return;
    }

    f->dst_flash = ota_area_flash(f->tgt.target);
    if (f->dst_flash == NULL || ota_flash_check(f->dst_flash) != OTA_OK) {
        flow_fail(f, OTA_ERR_MEDIA);
        return;
    }
    f->dst_off = f->tgt.target->base;

    f->act         = f->tgt.act;
    f->target_slot = f->tgt.target_slot;
    f->target_area = ota_area_id(f->tgt.run_target);
    f->stage_area  = f->tgt.target_stage;
    f->total       = f->seg.size;

    if (!f->raw) {
        /* 裸 bin：头部已在 do_hdr 读进 pending，源游标已停在镜像偏移 OTA_PKG_HDR_SIZE，
         * 不能回退；do_write 会先把 pending 吐回再继续读源。 */
        rc = flow_seek_to(f, f->data_off);
        if (rc != OTA_OK) {
            flow_fail(f, rc);
            return;
        }
    }

    /* 擦除终点：按擦除单位上取整，但不越过目标区末端 */
    f->erase_cur = f->dst_off;
    f->erase_end = f->dst_off + f->seg.size;
    unit = ota_flash_erase_unit(f->dst_flash, f->dst_off);
    if (unit != 0u && (f->erase_end % unit) != 0u) {
        f->erase_end += unit - (f->erase_end % unit);
    }
    area_end = f->tgt.target->base + f->tgt.target->size;
    if (f->erase_end > area_end) {
        f->erase_end = area_end;
    }

    ota_verifier_crc32(&f->v_mem, &f->st_mem, f->seg.crc32);
    (void)f->v_mem.init(f->v_mem.ctx, f->seg.crc32, f->seg.size);
    ota_verifier_crc32(&f->v_flash, &f->st_flash, f->seg.crc32);
    (void)f->v_flash.init(f->v_flash.ctx, f->seg.crc32, f->seg.size);

    f->state = OTA_FLOW_ERASE;
    flow_report(f);
}

static void do_erase(ota_flow_t *f)
{
    uint32_t unit;

    if (f->erase_cur >= f->erase_end) {
        f->state = OTA_FLOW_WRITE;
        flow_report(f);
        return;
    }

    unit = ota_flash_erase_unit(f->dst_flash, f->erase_cur);
    if (unit == 0u) {
        flow_fail(f, OTA_ERR_MEDIA);
        return;
    }
    if ((f->erase_cur + unit) > f->erase_end) {
        unit = f->erase_end - f->erase_cur;      /* 收尾不足一个单位 */
    }
    if (f->dst_flash->erase(f->dst_flash->ctx, f->erase_cur, unit) != OTA_OK) {
        flow_fail(f, OTA_ERR_MEDIA);
        return;
    }

    f->erase_cur += unit;
    f->erased     = f->erase_cur - f->dst_off;

    if (f->erase_cur >= f->erase_end) {
        f->state = OTA_FLOW_WRITE;
    }
    flow_report(f);                              /* 每擦一个单位就有一次喂狗机会 */
}

static void do_write(ota_flow_t *f)
{
    uint32_t want;
    uint32_t got = 0u;

    if (f->written >= f->total) {
        f->state = OTA_FLOW_VERIFY_MEM;
        flow_report(f);
        return;
    }

    /* 裸 bin：do_hdr 多读的那 OTA_PKG_HDR_SIZE 字节镜像头部，先原样写回 flash */
    if (f->raw && f->pending_off < f->pending_len) {
        uint32_t avail = f->pending_len - f->pending_off;
        uint32_t take  = f->total - f->written;

        if (take > avail) {
            take = avail;
        }
        if (take > f->cfg->buf_len) {
            take = f->cfg->buf_len;          /* pending 本就 <= 80，这里只是保险 */
        }
        if (f->dst_flash->write(f->dst_flash->ctx, f->dst_off + f->written,
                                f->pending + f->pending_off, take) != OTA_OK) {
            flow_fail(f, OTA_ERR_MEDIA);
            return;
        }
        (void)f->v_mem.update(f->v_mem.ctx, f->pending + f->pending_off, take);
        f->pending_off += take;
        f->written     += take;
        if (f->written >= f->total) {
            f->state = OTA_FLOW_VERIFY_MEM;
        }
        flow_report(f);
        return;
    }

    want = f->total - f->written;
    if (want > f->cfg->buf_len) {
        want = f->cfg->buf_len;
    }

    if (f->cfg->source->read(f->cfg->source->ctx, f->cfg->buf, want, &got) != OTA_OK ||
        got == 0u) {
        flow_fail(f, OTA_ERR_SOURCE);            /* 流被截断 */
        return;
    }
    f->bytes_in += got;

    if (f->dst_flash->write(f->dst_flash->ctx, f->dst_off + f->written,
                            f->cfg->buf, got) != OTA_OK) {
        flow_fail(f, OTA_ERR_MEDIA);
        return;
    }
    (void)f->v_mem.update(f->v_mem.ctx, f->cfg->buf, got);
    f->written += got;

    if (f->written >= f->total) {
        f->state = OTA_FLOW_VERIFY_MEM;
    }
    flow_report(f);
}

static void do_verify_mem(ota_flow_t *f)
{
    int rc;

    if (f->raw) {
        /* 裸 bin 无外部 CRC 可比对，第 2 关只累计内存 CRC，留待与第 3 关（闪存回读）互校 */
        f->crc_mem = f->st_mem.running;
        rc = OTA_OK;
    } else {
        /* 第 2 关：内存累计 CRC + 长度，与包头段表里的 CRC32 比对。证明「收到的报文对」 */
        rc = f->v_mem.finish(f->v_mem.ctx);
        f->crc_mem = f->st_mem.running;
    }

    if (rc != OTA_OK) {
        flow_fail(f, OTA_ERR_VERIFY);
        return;
    }

    f->verified = 0u;
    f->state    = OTA_FLOW_VERIFY_FLASH;
    flow_report(f);
}

static void do_verify_flash(ota_flow_t *f)
{
    uint32_t want;

    if (f->verified >= f->total) {
        int rc;

        if (f->raw) {
            /* 裸 bin：第 3 关读回 CRC 与第 2 关内存 CRC 互校，证明「落到 Flash 的字节对」 */
            f->crc_flash = f->st_flash.running;
            rc = (f->crc_flash == f->crc_mem) ? OTA_OK : OTA_ERR_VERIFY;
        } else {
            /* 第 3 关：读回 Flash 重算 CRC，与包头段表 CRC32 比对。证明「落到 Flash 的字节对」 */
            rc = f->v_flash.finish(f->v_flash.ctx);
            f->crc_flash = f->st_flash.running;
        }

        if (rc != OTA_OK) {
            flow_fail(f, OTA_ERR_VERIFY);
            return;
        }
        f->state = OTA_FLOW_COMMIT;
        flow_report(f);
        return;
    }

    want = f->total - f->verified;
    if (want > f->cfg->buf_len) {
        want = f->cfg->buf_len;
    }

    if (f->dst_flash->read(f->dst_flash->ctx, f->dst_off + f->verified,
                           f->cfg->buf, want) != OTA_OK) {
        flow_fail(f, OTA_ERR_MEDIA);
        return;
    }
    (void)f->v_flash.update(f->v_flash.ctx, f->cfg->buf, want);
    f->verified += want;

    flow_report(f);
}

static void do_commit(ota_flow_t *f)
{
    ota_cfg_t cfg;
    uint8_t   ver_slot;

    if (ota_cfg_load_or_default(&cfg) != OTA_OK) {
        flow_fail(f, OTA_ERR_MEDIA);
        return;
    }

    cfg.pending_action = (uint8_t)f->act;
    cfg.target_slot    = f->target_slot;
    cfg.stage_area     = f->stage_area;
    cfg.target_area    = f->target_area;
    cfg.image_size     = f->seg.size;
    cfg.image_crc32    = f->raw ? f->crc_mem : f->seg.crc32;
    cfg.move_progress  = 0u;
    cfg.boot_try       = 0u;
    cfg.factory_flag   = 0u;

    /* 本次要试运行的槽：SWITCH 是目标槽；MOVE 是那个唯一的运行区 */
    ver_slot = (f->act == OTA_ACT_SWITCH) ? f->target_slot
                                          : (uint8_t)f->tgt.run_target->slot_id;
    if (ver_slot >= OTA_SLOT_COUNT) {
        ver_slot = OTA_SLOT_A;
    }
    cfg.trial_slot = ver_slot;

    if (f->hdr.fw_ver != 0u) {
        cfg.fw_ver[ver_slot] = f->hdr.fw_ver;
    }

    /* active_slot 不由这里改：它只归 BL 所有（唯一持久化者） */

    if (ota_cfg_save(&cfg) != OTA_OK) {
        flow_fail(f, OTA_ERR_MEDIA);
        return;
    }

    if (f->cfg->source->close != NULL) {
        f->cfg->source->close(f->cfg->source->ctx);
    }

    f->state = OTA_FLOW_DONE;
    flow_report(f);
}

/* ---------------- 对外接口 ---------------- */

int ota_flow_start(ota_flow_t *f, const ota_flow_cfg_t *cfg)
{
    if (f == NULL || cfg == NULL || cfg->source == NULL || cfg->buf == NULL) {
        return OTA_ERR_PARAM;
    }
    if (cfg->buf_len < OTA_PKG_HDR_SIZE) {
        return OTA_ERR_PARAM;
    }
    if (ota_source_check(cfg->source) != OTA_OK) {
        return OTA_ERR_PARAM;
    }

    (void)memset(f, 0, sizeof(*f));
    f->cfg         = cfg;
    f->state       = OTA_FLOW_OPEN;
    f->err         = OTA_OK;
    f->target_slot = OTA_SLOT_NONE;
    f->target_area = OTA_AREA_ID_NONE;
    f->stage_area  = OTA_AREA_ID_NONE;
    return OTA_OK;
}

ota_flow_ret_t ota_flow_step(ota_flow_t *f)
{
    if (f == NULL || f->cfg == NULL) {
        return OTA_FLOW_ERR;
    }

    switch (f->state) {
    case OTA_FLOW_OPEN:         do_open(f);         break;
    case OTA_FLOW_HDR:          do_hdr(f);          break;
    case OTA_FLOW_DECIDE:       do_decide(f);       break;
    case OTA_FLOW_ERASE:        do_erase(f);        break;
    case OTA_FLOW_WRITE:        do_write(f);        break;
    case OTA_FLOW_VERIFY_MEM:   do_verify_mem(f);   break;
    case OTA_FLOW_VERIFY_FLASH: do_verify_flash(f); break;
    case OTA_FLOW_COMMIT:       do_commit(f);       break;
    default:                                        break;
    }

    if (f->state == OTA_FLOW_FAILED) {
        return OTA_FLOW_ERR;
    }
    if (f->state == OTA_FLOW_DONE) {
        return OTA_FLOW_OK;
    }
    return OTA_FLOW_BUSY;
}

uint8_t ota_flow_progress(const ota_flow_t *f)
{
    uint32_t p;

    if (f == NULL) {
        return 0u;
    }
    if (f->state == OTA_FLOW_DONE) {
        return 100u;
    }
    if (f->total == 0u) {
        return 0u;
    }

    if (f->state <= OTA_FLOW_WRITE) {
        p = (f->written * 70u) / f->total;                 /* 收写占 0~70% */
    } else if (f->state == OTA_FLOW_VERIFY_FLASH) {
        p = 70u + ((f->verified * 25u) / f->total);        /* 回读校验占 70~95% */
    } else {
        p = 100u;                                          /* 提交阶段 */
    }

    return (uint8_t)((p > 100u) ? 100u : p);
}

int ota_flow_abort(ota_flow_t *f)
{
    if (f == NULL) {
        return OTA_ERR_PARAM;
    }
    flow_fail(f, OTA_ERR_STATE);
    return OTA_OK;
}

const char *ota_flow_state_name(ota_flow_state_t s)
{
    switch (s) {
    case OTA_FLOW_IDLE:         return "idle";
    case OTA_FLOW_OPEN:         return "open";
    case OTA_FLOW_HDR:          return "hdr";
    case OTA_FLOW_DECIDE:       return "decide";
    case OTA_FLOW_ERASE:        return "erase";
    case OTA_FLOW_WRITE:        return "write";
    case OTA_FLOW_VERIFY_MEM:   return "verify_mem";
    case OTA_FLOW_VERIFY_FLASH: return "verify_flash";
    case OTA_FLOW_COMMIT:       return "commit";
    case OTA_FLOW_DONE:         return "done";
    case OTA_FLOW_FAILED:       return "failed";
    default:                    return "?";
    }
}
