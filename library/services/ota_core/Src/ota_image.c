/**
 * @file    ota_image.c
 * @brief   .otapkg 包头解析 + CRC32 校验器
 * @version V1.0
 * @date    2026-09-18
 */

#include <string.h>
#include "ota_image.h"
#include "ota_crc32.h"

/* 包头必须**正好** 80 字节、且无隐式填充，否则 PC 侧 tools/fw-ota-pack.py 打出来的包
 * 与固件侧解析会错位。所有字段都天然对齐，所以这里应当恒成立；一旦有人调整字段
 * 顺序或类型，编译期就会拦下。 */
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(sizeof(ota_pkg_hdr_t) == OTA_PKG_HDR_SIZE,
               "ota_pkg_hdr_t 必须正好 80 字节（与 tools/fw-ota-pack.py 的头部布局对齐）");
#endif

/** @brief 段数据在包内的偏移（前序段各按 4 字节对齐） */
static uint32_t seg_data_off(const ota_pkg_hdr_t *h, uint8_t idx)
{
    uint32_t off = OTA_PKG_HDR_SIZE;

    for (uint8_t i = 0u; i < idx; i++) {
        off += (h->seg[i].size + 3u) & ~3u;
    }
    return off;
}

int ota_image_parse(const void *buf, uint32_t len, ota_pkg_hdr_t *out)
{
    const ota_pkg_hdr_t *h = (const ota_pkg_hdr_t *)buf;
    uint32_t calc;
    uint32_t data_end;

    if (buf == NULL || out == NULL) {
        return OTA_ERR_PARAM;
    }
    if (len < OTA_PKG_HDR_SIZE) {
        return OTA_ERR_IMAGE;
    }

    (void)memcpy(out, buf, OTA_PKG_HDR_SIZE);

    if (out->magic != OTA_PKG_MAGIC) {
        return OTA_ERR_IMAGE;
    }
    if (out->hdr_ver != OTA_PKG_HDR_VER) {
        return OTA_ERR_IMAGE;
    }
    if (out->hdr_size < OTA_PKG_HDR_SIZE) {
        return OTA_ERR_IMAGE;
    }
    if (out->seg_count == 0u || out->seg_count > OTA_PKG_SEG_MAX) {
        return OTA_ERR_IMAGE;
    }

    /* 头部自校验（前 hdr_size-4 字节） */
    calc = ota_crc32(0u, buf, (uint32_t)(out->hdr_size - 4u));
    if (calc != out->hdr_crc32) {
        return OTA_ERR_IMAGE;
    }

    /* 段表合理性 + 数据区不能超出声明长度 */
    data_end = OTA_PKG_HDR_SIZE;
    for (uint8_t i = 0u; i < out->seg_count; i++) {
        if (out->seg[i].size == 0u) {
            return OTA_ERR_IMAGE;
        }
        data_end = seg_data_off(out, i) + out->seg[i].size;
    }
    if (out->pkg_size < data_end) {
        return OTA_ERR_IMAGE;
    }
    if (len < out->pkg_size) {
        return OTA_ERR_IMAGE;                    /* 包被截断 */
    }

    (void)h;
    return OTA_OK;
}

int ota_image_find_seg(const ota_pkg_hdr_t *h, uint32_t load_addr,
                       const ota_seg_t **seg, uint32_t *data_off)
{
    if (h == NULL || seg == NULL || data_off == NULL) {
        return OTA_ERR_PARAM;
    }

    for (uint8_t i = 0u; i < h->seg_count; i++) {
        if (h->seg[i].load_addr == load_addr) {
            *seg      = &h->seg[i];
            *data_off = seg_data_off(h, i);
            return OTA_OK;
        }
    }
    return OTA_ERR_SEG;                          /* 包里没有本机要的那一段 */
}

/** @brief 把 0~255 按十进制写进 buf（无前导零），返回新的写入位置 */
static uint32_t put_u8(char *buf, uint32_t o, uint32_t v)
{
    if (v >= 100u) {
        buf[o++] = (char)('0' + ((v / 100u) % 10u));
    }
    if (v >= 10u) {
        buf[o++] = (char)('0' + ((v / 10u) % 10u));
    }
    buf[o++] = (char)('0' + (v % 10u));
    return o;
}

void ota_image_ver_str(uint32_t fw_ver, char *buf, uint32_t buf_len)
{
    uint32_t o = 0u;
    uint32_t d;

    if (buf == NULL || buf_len < 16u) {
        return;
    }

    /* 手写而非 sprintf：BL 里不引 libc 的格式化，省几百字节文本 */
    o = put_u8(buf, o, (fw_ver >> 24) & 0xFFu);
    buf[o++] = '.';
    o = put_u8(buf, o, (fw_ver >> 16) & 0xFFu);
    buf[o++] = '.';
    o = put_u8(buf, o, (fw_ver >> 8) & 0xFFu);

    d = fw_ver & 0xFFu;
    if (d != 0u) {
        buf[o++] = '.';
        o = put_u8(buf, o, d);
    }
    buf[o] = '\0';
}

/* ---------------- CRC32 校验器 ---------------- */

static int crc32_v_init(void *ctx, uint32_t expect, uint32_t size)
{
    ota_crc32_state_t *s = (ota_crc32_state_t *)ctx;

    if (s == NULL) {
        return OTA_ERR_PARAM;
    }
    s->running = 0u;
    s->expect  = expect;
    s->total   = size;
    s->done    = 0u;
    return OTA_OK;
}

static int crc32_v_update(void *ctx, const void *buf, uint32_t len)
{
    ota_crc32_state_t *s = (ota_crc32_state_t *)ctx;

    if (s == NULL) {
        return OTA_ERR_PARAM;
    }
    s->running = ota_crc32(s->running, buf, len);
    s->done   += len;
    return OTA_OK;
}

static int crc32_v_finish(void *ctx)
{
    ota_crc32_state_t *s = (ota_crc32_state_t *)ctx;

    if (s == NULL) {
        return OTA_ERR_PARAM;
    }
    if (s->total != 0u && s->done != s->total) {
        return OTA_ERR_VERIFY;                   /* 长度不符（被截断/多收） */
    }
    if (s->running != s->expect) {
        return OTA_ERR_VERIFY;
    }
    return OTA_OK;
}

void ota_verifier_crc32(ota_verifier_t *v, ota_crc32_state_t *st, uint32_t expect)
{
    if (v == NULL) {
        return;
    }
    v->kind   = OTA_VERIFY_CRC32;
    v->ctx    = (void *)st;
    v->init   = crc32_v_init;
    v->update = crc32_v_update;
    v->finish = crc32_v_finish;

    if (st != NULL) {
        (void)v->init(st, expect, 0u);           /* 长度由 ota_flow 另行校验 */
    }
}
