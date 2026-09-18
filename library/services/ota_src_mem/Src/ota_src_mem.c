/**
 * @file    ota_src_mem.c
 * @brief   取数后端：内存/RAM 实现
 * @version V1.0
 * @date    2026-09-18
 */

#include <string.h>
#include "ota_src_mem.h"

static int mem_open(void *ctx, ota_src_info_t *info)
{
    ota_src_mem_t *st = (ota_src_mem_t *)ctx;

    if (st == NULL || st->data == NULL || st->size == 0u) {
        return OTA_ERR_SOURCE;
    }

    st->pos = 0u;

    if (info != NULL) {
        info->total_size = st->size;
        info->fw_ver     = 0u;
        info->seekable   = st->seekable;
    }
    return OTA_OK;
}

static int mem_read(void *ctx, void *buf, uint32_t len, uint32_t *got)
{
    ota_src_mem_t *st = (ota_src_mem_t *)ctx;
    uint32_t       n;

    if (st == NULL || buf == NULL || got == NULL) {
        return OTA_ERR_PARAM;
    }

    *got = 0u;
    if (st->pos >= st->size) {
        return OTA_OK;                       /* EOF：got == 0 */
    }

    n = st->size - st->pos;
    if (n > len) {
        n = len;
    }
    (void)memcpy(buf, st->data + st->pos, n);
    st->pos += n;
    *got = n;
    return OTA_OK;
}

static int mem_seek(void *ctx, uint32_t off)
{
    ota_src_mem_t *st = (ota_src_mem_t *)ctx;

    if (st == NULL) {
        return OTA_ERR_PARAM;
    }
    if (st->seekable == 0u) {
        return OTA_ERR_UNSUPPORTED;          /* 模拟流式通道 */
    }
    if (off > st->size) {
        return OTA_ERR_SOURCE;
    }
    st->pos = off;
    return OTA_OK;
}

static void mem_close(void *ctx)
{
    ota_src_mem_t *st = (ota_src_mem_t *)ctx;

    if (st != NULL) {
        st->pos = 0u;
    }
}

void ota_src_mem_setup(ota_source_t *src, ota_src_mem_t *st,
                       const uint8_t *data, uint32_t size, uint8_t seekable)
{
    if (src == NULL || st == NULL) {
        return;
    }

    st->data     = data;
    st->size     = size;
    st->pos      = 0u;
    st->seekable = (seekable != 0u) ? 1u : 0u;

    src->name  = "mem";
    src->ctx   = (void *)st;
    src->open  = mem_open;
    src->read  = mem_read;
    src->close = mem_close;
    src->seek  = (st->seekable != 0u) ? mem_seek : (int (*)(void *, uint32_t))0;
}
