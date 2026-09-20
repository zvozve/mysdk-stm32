/**
 * @file    spi_nor_flash.c
 * @brief   通用 SPI NOR Flash 驱动实现
 * @version V1.0
 * @date    2026-09-18
 *
 * 只用 chip 层 oop_* 原语：SPI 走 oop_spi_*，CS/WP 走 oop_gpio_*，计时走 oop_dwt。
 * 本文件不得出现任何 HAL_SPI_* / HAL_GPIO_* 调用（sdk-check-oop --strict 会拦）。
 */

#include <string.h>
#include "spi_nor_flash.h"
#include "oop_dwt.h"        /* oop_DelayUS / oop_GetTickMS */

/* ---------------- 命令集 ---------------- */
#define NOR_CMD_WREN        0x06u   /* 写使能 */
#define NOR_CMD_RDSR1       0x05u   /* 读状态寄存器 1 */
#define NOR_CMD_READ        0x03u   /* 读（无 dummy） */
#define NOR_CMD_FAST_READ   0x0Bu   /* 快速读（8 dummy 时钟） */
#define NOR_CMD_PP          0x02u   /* 页编程（≤ 页大小，不跨页） */
#define NOR_CMD_SE          0x20u   /* 扇区擦 4 KB */
#define NOR_CMD_BE          0xD8u   /* 块擦 64 KB */
#define NOR_CMD_CE          0xC7u   /* 整片擦 */
#define NOR_CMD_RDID        0x9Fu   /* JEDEC ID */
#define NOR_CMD_DP          0xB9u   /* 进深睡眠 */
#define NOR_CMD_RDP         0xABu   /* 释放深睡眠 */

#define NOR_SR1_WIP         0x01u
#define NOR_SR1_WEL         0x02u

#define NOR_DEFAULT_PAGE      256u
#define NOR_DEFAULT_SECTOR   4096u
#define NOR_DEFAULT_BLOCK   65536u
#define NOR_DEFAULT_TIMEOUT  2000u
#define NOR_CHIP_ERASE_TIMEOUT_MS  300000u   /* 整片擦可达数十秒 */
#define NOR_READ_CHUNK       4096u           /* 顺序读分块（仅受 oop_spi 的 uint16 长度限制） */
#define NOR_VERIFY_CHUNK      256u           /* 读回比对用的栈缓冲 */

/* ---------------- 低层原语 ---------------- */

static void cs_assert(spi_nor_dev_t *dev)
{
    if (dev->cs != NULL) {
        oop_gpio_write(dev->cs, true);      /* CS 低有效：true = 选中 */
    }
}

static void cs_release(spi_nor_dev_t *dev)
{
    if (dev->cs != NULL) {
        oop_gpio_write(dev->cs, false);
    }
}

/** @brief 组装「命令 + 地址」，返回报文长度 */
static uint8_t put_cmd_addr(uint8_t *hdr, uint8_t cmd, uint32_t addr, uint8_t addr_bytes)
{
    hdr[0] = cmd;

    if (addr_bytes == 4u) {
        hdr[1] = (uint8_t)(addr >> 24);
        hdr[2] = (uint8_t)(addr >> 16);
        hdr[3] = (uint8_t)(addr >> 8);
        hdr[4] = (uint8_t)(addr);
        return 5u;
    }

    hdr[1] = (uint8_t)(addr >> 16);
    hdr[2] = (uint8_t)(addr >> 8);
    hdr[3] = (uint8_t)(addr);
    return 4u;
}

/** @brief 读一个寄存器字节序列（单命令、全双工） */
static int read_reg(spi_nor_dev_t *dev, uint8_t cmd, uint8_t *out, uint16_t len)
{
    HAL_StatusTypeDef st;

    cs_assert(dev);
    st = oop_spi_transmit_receive(dev->spi, &cmd, out, len);
    cs_release(dev);

    return (st == HAL_OK) ? SPI_NOR_OK : SPI_NOR_ERR_IO;
}

static bool addr_valid(const spi_nor_dev_t *dev, uint32_t addr, uint32_t len)
{
    if (len == 0u) {
        return true;
    }
    if (addr >= dev->size) {
        return false;
    }
    return (len <= (dev->size - addr));
}

/** @brief 等待 WIP 清零，超时 timeout_ms，期间调用 idle_cb */
static int nor_wait_ready_to(spi_nor_dev_t *dev, uint32_t timeout_ms)
{
    uint32_t t0 = oop_GetTickMS();

    for (;;) {
        uint8_t sr = 0u;
        int rc = read_reg(dev, NOR_CMD_RDSR1, &sr, 1u);
        if (rc != SPI_NOR_OK) {
            return rc;
        }
        if ((sr & NOR_SR1_WIP) == 0u) {
            return SPI_NOR_OK;
        }
        if (dev->idle_cb != NULL && !dev->idle_cb(dev->idle_user)) {
            return SPI_NOR_ERR_ABORT;
        }
        if ((uint32_t)(oop_GetTickMS() - t0) > timeout_ms) {
            return SPI_NOR_ERR_TIMEOUT;
        }
    }
}

/** @brief 擦除类命令的公共流程：WREN → 命令 → 等 WIP */
static int nor_erase_unit(spi_nor_dev_t *dev, uint8_t cmd, uint32_t addr)
{
    uint8_t hdr[5];
    uint8_t n;
    int rc = spi_nor_write_enable(dev);

    if (rc != SPI_NOR_OK) {
        return rc;
    }

    n = put_cmd_addr(hdr, cmd, addr, dev->addr_bytes);

    cs_assert(dev);
    if (oop_spi_transmit(dev->spi, hdr, n) != HAL_OK) {
        cs_release(dev);
        return SPI_NOR_ERR_IO;
    }
    cs_release(dev);

    return nor_wait_ready_to(dev, dev->timeout_ms);
}

/* ---------------- 生命周期 ---------------- */

int spi_nor_read_id(spi_nor_dev_t *dev, spi_nor_id_t *out)
{
    uint8_t cmd = NOR_CMD_RDID;
    uint8_t buf[3];
    HAL_StatusTypeDef st;
    uint32_t size = 0u;

    if (dev == NULL || out == NULL || dev->spi == NULL || dev->cs == NULL) {
        return SPI_NOR_ERR_PARAM;
    }

    cs_assert(dev);
    st = oop_spi_transmit_receive(dev->spi, &cmd, buf, 3u);
    cs_release(dev);
    if (st != HAL_OK) {
        return SPI_NOR_ERR_IO;
    }

    /* 容量码 → 字节数：2^code。上限取 2^30（1 GB）以免 uint32 溢出；
     * 超出范围的由 cfg.size 兜底。 */
    if (buf[2] >= 0x10u && buf[2] <= 0x1Eu) {
        size = (uint32_t)1u << buf[2];
    }

    out->mfr           = buf[0];
    out->mem_type      = buf[1];
    out->capacity_code = buf[2];
    out->size          = size;
    return SPI_NOR_OK;
}

int spi_nor_create(spi_nor_dev_t *dev, const spi_nor_cfg_t *cfg)
{
    int rc;

    if (dev == NULL || cfg == NULL || cfg->spi == NULL || cfg->cs == NULL) {
        return SPI_NOR_ERR_PARAM;
    }

    memset(dev, 0, sizeof(*dev));
    dev->spi        = cfg->spi;
    dev->cs         = cfg->cs;
    dev->wp         = cfg->wp;
    dev->fast_read  = cfg->fast_read;
    dev->addr_bytes = (cfg->addr_bytes == 4u) ? 4u : 3u;
    dev->page_size  = (cfg->page_size   != 0u) ? cfg->page_size   : NOR_DEFAULT_PAGE;
    dev->sector_size = (cfg->sector_size != 0u) ? cfg->sector_size : NOR_DEFAULT_SECTOR;
    dev->block_size = (cfg->block_size  != 0u) ? cfg->block_size  : NOR_DEFAULT_BLOCK;
    dev->timeout_ms = (cfg->timeout_ms  != 0u) ? cfg->timeout_ms  : NOR_DEFAULT_TIMEOUT;
    dev->ready      = false;

    cs_release(dev);

    /* 器件可能还停在深睡眠（上次掉电前被要求睡眠），先唤醒再探测 */
    cs_assert(dev);
    {
        uint8_t cmd = NOR_CMD_RDP;
        HAL_StatusTypeDef st = oop_spi_transmit(dev->spi, &cmd, 1u);
        cs_release(dev);
        if (st != HAL_OK) {
            return SPI_NOR_ERR_IO;
        }
    }
    oop_DelayUS(100u);              /* tRES1 典型 3 us，给足余量 */

    rc = spi_nor_read_id(dev, &dev->id);
    if (rc != SPI_NOR_OK) {
        return rc;
    }

    dev->size = (dev->id.size != 0u) ? dev->id.size : cfg->size;
    if (dev->size == 0u) {
        /* 容量既推不出、调用方也没给 → 拒绝放行，避免后续按错误容量擦写 */
        return SPI_NOR_ERR_PARAM;
    }

    dev->ready = true;
    return SPI_NOR_OK;
}

void spi_nor_set_idle_cb(spi_nor_dev_t *dev, spi_nor_idle_cb_t cb, void *user)
{
    if (dev == NULL) {
        return;
    }
    dev->idle_cb   = cb;
    dev->idle_user = user;
}

/* ---------------- 查询 ---------------- */

uint32_t spi_nor_size(const spi_nor_dev_t *dev)
{
    return (dev != NULL && dev->ready) ? dev->size : 0u;
}

int spi_nor_read_status(spi_nor_dev_t *dev, uint8_t *sr1)
{
    if (dev == NULL || sr1 == NULL || !dev->ready) {
        return SPI_NOR_ERR_PARAM;
    }
    return read_reg(dev, NOR_CMD_RDSR1, sr1, 1u);
}

bool spi_nor_is_busy(spi_nor_dev_t *dev)
{
    uint8_t sr = 0u;

    if (dev == NULL || !dev->ready) {
        return false;
    }
    if (read_reg(dev, NOR_CMD_RDSR1, &sr, 1u) != SPI_NOR_OK) {
        return false;
    }
    return ((sr & NOR_SR1_WIP) != 0u);
}

int spi_nor_write_enable(spi_nor_dev_t *dev)
{
    uint8_t cmd = NOR_CMD_WREN;
    uint8_t sr = 0u;

    if (dev == NULL || !dev->ready) {
        return SPI_NOR_ERR_PARAM;
    }

    cs_assert(dev);
    if (oop_spi_transmit(dev->spi, &cmd, 1u) != HAL_OK) {
        cs_release(dev);
        return SPI_NOR_ERR_IO;
    }
    cs_release(dev);

    /* 回读 WEL：总线不可信 / 器件被写保护时立刻暴露，而不是等到擦写完才发现 */
    if (read_reg(dev, NOR_CMD_RDSR1, &sr, 1u) != SPI_NOR_OK) {
        return SPI_NOR_ERR_IO;
    }
    return ((sr & NOR_SR1_WEL) != 0u) ? SPI_NOR_OK : SPI_NOR_ERR_WEL;
}

int spi_nor_wait_ready(spi_nor_dev_t *dev)
{
    if (dev == NULL || !dev->ready) {
        return SPI_NOR_ERR_PARAM;
    }
    return nor_wait_ready_to(dev, dev->timeout_ms);
}

/* ---------------- 读 ---------------- */

int spi_nor_read(spi_nor_dev_t *dev, uint32_t addr, void *buf, uint32_t len)
{
    uint8_t *dst = (uint8_t *)buf;
    uint8_t hdr[6];
    uint8_t n;
    uint32_t done = 0u;

    if (len == 0u) {
        return SPI_NOR_OK;
    }
    if (dev == NULL || !dev->ready || buf == NULL) {
        return SPI_NOR_ERR_PARAM;
    }
    if (!addr_valid(dev, addr, len)) {
        return SPI_NOR_ERR_RANGE;
    }

    n = put_cmd_addr(hdr, dev->fast_read ? NOR_CMD_FAST_READ : NOR_CMD_READ,
                     addr, dev->addr_bytes);
    if (dev->fast_read) {
        hdr[n] = 0xFFu;             /* 8 个 dummy 时钟 = 1 字节 */
        n++;
    }

    cs_assert(dev);
    if (oop_spi_transmit(dev->spi, hdr, n) != HAL_OK) {
        cs_release(dev);
        return SPI_NOR_ERR_IO;
    }

    /* 顺序读可跨页跨扇区，一次 CS 拉低即可读完整段；只按 uint16 长度分块 */
    while (done < len) {
        uint32_t chunk = len - done;
        if (chunk > NOR_READ_CHUNK) {
            chunk = NOR_READ_CHUNK;
        }
        if (oop_spi_receive(dev->spi, dst + done, (uint16_t)chunk) != HAL_OK) {
            cs_release(dev);
            return SPI_NOR_ERR_IO;
        }
        done += chunk;
    }

    cs_release(dev);
    return SPI_NOR_OK;
}

/* ---------------- 写 ---------------- */

int spi_nor_write(spi_nor_dev_t *dev, uint32_t addr, const void *buf, uint32_t len)
{
    const uint8_t *src = (const uint8_t *)buf;
    uint32_t done = 0u;

    if (len == 0u) {
        return SPI_NOR_OK;
    }
    if (dev == NULL || !dev->ready || buf == NULL) {
        return SPI_NOR_ERR_PARAM;
    }
    if (!addr_valid(dev, addr, len)) {
        return SPI_NOR_ERR_RANGE;
    }

    while (done < len) {
        uint32_t page_off = (addr + done) % dev->page_size;
        uint32_t chunk    = dev->page_size - page_off;      /* 一页之内，绝不跨页 */
        uint8_t  hdr[5];
        uint8_t  n;
        int      rc;

        if (chunk > (len - done)) {
            chunk = len - done;
        }

        rc = spi_nor_write_enable(dev);
        if (rc != SPI_NOR_OK) {
            return rc;
        }

        n = put_cmd_addr(hdr, NOR_CMD_PP, addr + done, dev->addr_bytes);

        cs_assert(dev);
        if (oop_spi_transmit(dev->spi, hdr, n) != HAL_OK ||
            oop_spi_transmit(dev->spi, src + done, (uint16_t)chunk) != HAL_OK) {
            cs_release(dev);
            return SPI_NOR_ERR_IO;
        }
        cs_release(dev);

        rc = nor_wait_ready_to(dev, dev->timeout_ms);
        if (rc != SPI_NOR_OK) {
            return rc;
        }

        done += chunk;
    }

    return SPI_NOR_OK;
}

/* ---------------- 擦 ---------------- */

int spi_nor_erase_sector(spi_nor_dev_t *dev, uint32_t addr)
{
    if (dev == NULL || !dev->ready) {
        return SPI_NOR_ERR_PARAM;
    }
    if (!addr_valid(dev, addr, 1u) || (addr % dev->sector_size) != 0u) {
        return SPI_NOR_ERR_RANGE;
    }
    return nor_erase_unit(dev, NOR_CMD_SE, addr);
}

int spi_nor_erase_block(spi_nor_dev_t *dev, uint32_t addr)
{
    if (dev == NULL || !dev->ready) {
        return SPI_NOR_ERR_PARAM;
    }
    if (!addr_valid(dev, addr, 1u) || (addr % dev->block_size) != 0u) {
        return SPI_NOR_ERR_RANGE;
    }
    return nor_erase_unit(dev, NOR_CMD_BE, addr);
}

int spi_nor_erase_chip(spi_nor_dev_t *dev)
{
    uint8_t cmd = NOR_CMD_CE;
    int rc;

    if (dev == NULL || !dev->ready) {
        return SPI_NOR_ERR_PARAM;
    }

    rc = spi_nor_write_enable(dev);
    if (rc != SPI_NOR_OK) {
        return rc;
    }

    cs_assert(dev);
    if (oop_spi_transmit(dev->spi, &cmd, 1u) != HAL_OK) {
        cs_release(dev);
        return SPI_NOR_ERR_IO;
    }
    cs_release(dev);

    return nor_wait_ready_to(dev, NOR_CHIP_ERASE_TIMEOUT_MS);
}

int spi_nor_erase(spi_nor_dev_t *dev, uint32_t addr, uint32_t len)
{
    uint32_t pos;
    uint32_t end;

    if (len == 0u) {
        return SPI_NOR_OK;
    }
    if (dev == NULL || !dev->ready) {
        return SPI_NOR_ERR_PARAM;
    }
    if (!addr_valid(dev, addr, len)) {
        return SPI_NOR_ERR_RANGE;
    }

    pos = addr - (addr % dev->sector_size);              /* 向低对齐 */
    end = addr + len;
    if ((end % dev->sector_size) != 0u) {                /* 向高对齐 */
        end += dev->sector_size - (end % dev->sector_size);
    }
    if (end > dev->size) {
        end = dev->size;
    }

    while (pos < end) {
        int rc;
        if (((pos % dev->block_size) == 0u) && ((end - pos) >= dev->block_size)) {
            rc = nor_erase_unit(dev, NOR_CMD_BE, pos);   /* 能整块擦就不逐扇区擦 */
            if (rc != SPI_NOR_OK) {
                return rc;
            }
            pos += dev->block_size;
        } else {
            rc = nor_erase_unit(dev, NOR_CMD_SE, pos);
            if (rc != SPI_NOR_OK) {
                return rc;
            }
            pos += dev->sector_size;
        }
    }

    return SPI_NOR_OK;
}

/* ---------------- 校验 ---------------- */

int spi_nor_is_erased(spi_nor_dev_t *dev, uint32_t addr, uint32_t len)
{
    uint8_t chunk[NOR_VERIFY_CHUNK];
    uint32_t done = 0u;

    if (len == 0u) {
        return SPI_NOR_OK;
    }
    if (dev == NULL || !dev->ready) {
        return SPI_NOR_ERR_PARAM;
    }
    if (!addr_valid(dev, addr, len)) {
        return SPI_NOR_ERR_RANGE;
    }

    while (done < len) {
        uint32_t c = len - done;
        int rc;

        if (c > sizeof(chunk)) {
            c = sizeof(chunk);
        }
        rc = spi_nor_read(dev, addr + done, chunk, c);
        if (rc != SPI_NOR_OK) {
            return rc;
        }
        for (uint32_t i = 0u; i < c; i++) {
            if (chunk[i] != 0xFFu) {
                return SPI_NOR_ERR_MISMATCH;
            }
        }
        done += c;
    }

    return SPI_NOR_OK;
}

int spi_nor_verify(spi_nor_dev_t *dev, uint32_t addr, const void *buf, uint32_t len,
                   uint32_t *bad_addr)
{
    const uint8_t *exp = (const uint8_t *)buf;
    uint8_t chunk[NOR_VERIFY_CHUNK];
    uint32_t done = 0u;

    if (len == 0u) {
        return SPI_NOR_OK;
    }
    if (dev == NULL || !dev->ready || buf == NULL) {
        return SPI_NOR_ERR_PARAM;
    }
    if (!addr_valid(dev, addr, len)) {
        return SPI_NOR_ERR_RANGE;
    }

    while (done < len) {
        uint32_t c = len - done;
        int rc;

        if (c > sizeof(chunk)) {
            c = sizeof(chunk);
        }
        rc = spi_nor_read(dev, addr + done, chunk, c);
        if (rc != SPI_NOR_OK) {
            return rc;
        }
        for (uint32_t i = 0u; i < c; i++) {
            if (chunk[i] != exp[done + i]) {
                if (bad_addr != NULL) {
                    *bad_addr = addr + done + i;
                }
                return SPI_NOR_ERR_MISMATCH;
            }
        }
        done += c;
    }

    return SPI_NOR_OK;
}

/* ---------------- 功耗 ---------------- */

int spi_nor_deep_power_down(spi_nor_dev_t *dev)
{
    uint8_t cmd = NOR_CMD_DP;

    if (dev == NULL || !dev->ready) {
        return SPI_NOR_ERR_PARAM;
    }

    cs_assert(dev);
    if (oop_spi_transmit(dev->spi, &cmd, 1u) != HAL_OK) {
        cs_release(dev);
        return SPI_NOR_ERR_IO;
    }
    cs_release(dev);

    oop_DelayUS(5u);                /* tDP 典型 3 us */
    return SPI_NOR_OK;
}

int spi_nor_release_power_down(spi_nor_dev_t *dev)
{
    uint8_t cmd = NOR_CMD_RDP;

    if (dev == NULL || !dev->ready) {
        return SPI_NOR_ERR_PARAM;
    }

    cs_assert(dev);
    if (oop_spi_transmit(dev->spi, &cmd, 1u) != HAL_OK) {
        cs_release(dev);
        return SPI_NOR_ERR_IO;
    }
    cs_release(dev);

    oop_DelayUS(100u);              /* tRES1 典型 3 us，给足余量 */
    return SPI_NOR_OK;
}
