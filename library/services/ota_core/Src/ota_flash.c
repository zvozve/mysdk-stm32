/**
 * @file    ota_flash.c
 * @brief   介质抽象实现：内部 Flash 适配（坐 chip.oop_flash）
 * @version V1.0
 * @date    2026-09-18
 */

#include "ota_flash.h"
#include "oop_flash_drv.h"

/* ---------------- 内部 Flash 适配 ---------------- */

/* 不直接用 FLASH_BASE 这类设备宏：基址从 chip.oop_flash 的几何里取，
 * 这样 services 层不出现任何 CMSIS/HAL 符号。 */
static uint32_t int_base(void)
{
    return oop_flash_info()->base;
}

static int int_read(void *ctx, uint32_t off, void *buf, uint32_t len)
{
    (void)ctx;
    return (oop_flash_read(int_base() + off, buf, len) == OOP_FLASH_OK) ? OTA_OK : OTA_ERR_MEDIA;
}

static int int_erase(void *ctx, uint32_t off, uint32_t len)
{
    (void)ctx;
    return (oop_flash_erase(int_base() + off, len) == OOP_FLASH_OK) ? OTA_OK : OTA_ERR_MEDIA;
}

static int int_write(void *ctx, uint32_t off, const void *buf, uint32_t len)
{
    (void)ctx;
    return (oop_flash_write(int_base() + off, buf, len) == OOP_FLASH_OK) ? OTA_OK : OTA_ERR_MEDIA;
}

static uint32_t int_erase_unit(void *ctx, uint32_t off)
{
    (void)ctx;
    return oop_flash_sector_size(int_base() + off);
}

int ota_flash_int_get(ota_flash_t *out)
{
    const oop_flash_info_t *fi;

    if (out == NULL) {
        return OTA_ERR_PARAM;
    }

    fi = oop_flash_info();
    if (fi->size == 0u) {
        return OTA_ERR_MEDIA;
    }

    out->name        = "int";
    out->media       = OTA_MEDIA_INT;
    out->base_addr   = fi->base;        /* 内部介质的 CPU 基址（0x08000000） */
    out->size        = fi->size;
    out->read        = int_read;
    out->erase       = int_erase;
    out->write       = int_write;
    out->erase_unit  = int_erase_unit;
    out->ctx         = (void *)0;
    return OTA_OK;
}

/* ---------------- 通用 ---------------- */

uint32_t ota_flash_erase_unit(const ota_flash_t *f, uint32_t off)
{
    uint32_t u;

    if (f == NULL || f->erase_unit == NULL) {
        return OTA_FLASH_DEFAULT_ERASE_UNIT;
    }

    u = f->erase_unit(f->ctx, off);
    return (u != 0u) ? u : OTA_FLASH_DEFAULT_ERASE_UNIT;
}

int ota_flash_check(const ota_flash_t *f)
{
    if (f == NULL || f->read == NULL || f->erase == NULL || f->write == NULL) {
        return OTA_ERR_PARAM;
    }
    if (f->size == 0u) {
        return OTA_ERR_MEDIA;
    }
    return OTA_OK;
}
