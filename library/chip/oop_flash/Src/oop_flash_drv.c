/**
 * @file    oop_flash_drv.c
 * @brief   内部 Flash OOP 驱动实现
 * @version V1.0
 * @date    2026-09-18
 *
 * 结构：公共核心（几何查询 / 擦写展开 / 安全闸 / 读回校验）+ 四个系列钩子
 *       （plat_geom_init / plat_unit_get / plat_unit_erase / plat_unit_program）。
 *       新增系列只需补这四个钩子，公共核心一行不动。
 */

#include <string.h>
#include "oop_flash_drv.h"

/* ============================================================
 * 公共状态
 * ============================================================ */

static oop_flash_info_t s_info;
static bool             s_ready;

static uint32_t s_guard_begin;
static uint32_t s_guard_end;      /* <= begin 视为不设限 */

/**
 * @brief 读 FLASH_SIZE 寄存器（所有 STM32 都在该地址放 16 位容量，单位 KB）
 * @note  不同系列这个寄存器的地址不同，但宏名统一为 FLASHSIZE_BASE。
 */
static uint32_t plat_size_kb(void)
{
#if defined(FLASHSIZE_BASE)
    return (uint32_t)(*(volatile uint16_t *)FLASHSIZE_BASE);
#else
    return 0u;                     /* 探测不到时由各自的 geom_init 兜底 */
#endif
}

/* ============================================================
 * 系列钩子：F4（变长扇区，单 Bank）
 * ============================================================ */

#if HAL_PLATFORM_F4

/* 双 Bank 器件（F427/F437/F429/F439/F469/F479）的 HAL 在
 * FLASH_Erase_Sector() 里对扇号 > FLASH_SECTOR_11 有一个「SNB 偏移 +4」的
 * 特殊规则，本驱动未实现该映射。与其静默算错地址擦错扇区，不如在编译期拦下。 */
#ifdef FLASH_BANK_2
#error "oop_flash: 双 Bank 器件（F42x/F43x/F469/F479）的扇区号->Bank 映射未实现，请先补 FLASH_Erase_Sector 的 SNB 偏移规则再启用本模块"
#endif

/* F2/F4/F7 单 Bank 扇区布局（最多 12 扇区 = 1 MB）：
 *     扇区 0~3  = 16 KB
 *     扇区 4    = 64 KB
 *     扇区 5~11 = 128 KB
 * 不同容量的器件共用这张表的「前缀」，实际扇区数由 FLASH_SIZE 截断
 * （512 KB 器件 = 前 8 项，1 MB 器件 = 全部 12 项）。 */
static const oop_flash_sector_t s_sectors_f4[] = {
    {  0u, 0x08000000u,  16u * 1024u },
    {  1u, 0x08004000u,  16u * 1024u },
    {  2u, 0x08008000u,  16u * 1024u },
    {  3u, 0x0800C000u,  16u * 1024u },
    {  4u, 0x08010000u,  64u * 1024u },
    {  5u, 0x08020000u, 128u * 1024u },
    {  6u, 0x08040000u, 128u * 1024u },
    {  7u, 0x08060000u, 128u * 1024u },
    {  8u, 0x08080000u, 128u * 1024u },
    {  9u, 0x080A0000u, 128u * 1024u },
    { 10u, 0x080C0000u, 128u * 1024u },
    { 11u, 0x080E0000u, 128u * 1024u },
};
#define OOP_FLASH_MAX_UNITS  ((uint32_t)(sizeof(s_sectors_f4) / sizeof(s_sectors_f4[0])))

static void plat_geom_init(oop_flash_info_t *info)
{
    uint32_t want = plat_size_kb() * 1024u;
    uint32_t n    = 0u;
    uint32_t sum  = 0u;

    while (n < OOP_FLASH_MAX_UNITS && (sum + s_sectors_f4[n].size) <= want) {
        sum += s_sectors_f4[n].size;
        n++;
    }
    if (n == 0u) {                       /* 容量寄存器读到 0：按 F4 最小形态 512 KB / 8 扇区兜底 */
        n   = 8u;
        sum = 512u * 1024u;
    }

    info->base         = FLASH_BASE;
    info->size         = sum;
    info->sector_count = n;
    info->unit_size    = 0u;             /* 变长扇区，无单一单位大小 */
    info->write_gran   = 4u;             /* WORD 编程：三种电压范围都支持，不必看电压 */
    info->uniform      = false;
}

static void plat_unit_get(uint32_t index, oop_flash_sector_t *out)
{
    *out = s_sectors_f4[index];
}

static int plat_unit_erase(uint32_t index, uint32_t addr)
{
    FLASH_EraseInitTypeDef init = {0};
    uint32_t sector_err = 0u;
    (void)addr;

    init.TypeErase    = FLASH_TYPEERASE_SECTORS;
    init.Banks        = FLASH_BANK_1;             /* 单 Bank 器件的扇区擦除不校验本字段 */
    init.Sector       = index;
    init.NbSectors    = 1u;                       /* 一次一个扇区：绝不跨 Bank 批量擦 */
    init.VoltageRange = FLASH_VOLTAGE_RANGE_3;    /* 2.7~3.6 V，按 WORD 并行度擦 */

    return (HAL_FLASHEx_Erase(&init, &sector_err) == HAL_OK) ? OOP_FLASH_OK : OOP_FLASH_ERR_ERASE;
}

static int plat_unit_program(uint32_t addr, const uint8_t *bytes)
{
    uint32_t word = (uint32_t)bytes[0]
                  | ((uint32_t)bytes[1] << 8)
                  | ((uint32_t)bytes[2] << 16)
                  | ((uint32_t)bytes[3] << 24);   /* STM32 小端：bytes[0] 是 LSB */

    return (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr, (uint64_t)word) == HAL_OK)
           ? OOP_FLASH_OK : OOP_FLASH_ERR_WRITE;
}

/* 清 FLASH 错误标志：上一段程序（如 BL 写 CFG）可能留下 OPERR/WRPERR/PGAERR/
 * PGPERR/PGSERR，而 HAL 的 FLASH_WaitForLastOperation() 遇到这些标志会**立刻**
 * 判错（且它只清 EOP、不清错误位）—— 现象就是「本轮第一次擦除在 µs 级失败」。 */
static void plat_clear_flags(void)
{
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
                           FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);
}

#endif /* HAL_PLATFORM_F4 */

/* ============================================================
 * 系列钩子：F1（等长页，1 KB 或 2 KB）
 * ============================================================ */

#if HAL_PLATFORM_F1

/* F1 页大小按密度分档，与 ST 的 FLASH_PAGE_SIZE 定义一致：
 *   低/中密度（<=128 KB，如 F103C8/CB、F100）→ 1 KB
 *   高/超高密度（>128 KB，如 F103ZE、F103ZG、F105/107）→ 2 KB
 * 这里按容量推导而不依赖 FLASH_PAGE_SIZE 宏，避免不同 HAL 版本宏缺失。 */
static uint32_t plat_f1_page_size(uint32_t kb)
{
    return (kb > 128u) ? 2048u : 1024u;
}

static void plat_geom_init(oop_flash_info_t *info)
{
    uint32_t kb = plat_size_kb();
    uint32_t page;

    if (kb == 0u) {
        kb = 64u;                        /* 探测失败兜底：按 F103C8 的 64 KB */
    }
    page = plat_f1_page_size(kb);

    info->base         = FLASH_BASE;
    info->size         = kb * 1024u;
    info->sector_count = info->size / page;
    info->unit_size    = page;
    info->write_gran   = 2u;             /* HALFWORD 编程 */
    info->uniform      = true;
}

static void plat_unit_get(uint32_t index, oop_flash_sector_t *out)
{
    const oop_flash_info_t *info = oop_flash_info();

    out->index = index;
    out->base  = info->base + index * info->unit_size;
    out->size  = info->unit_size;
}

static int plat_unit_erase(uint32_t index, uint32_t addr)
{
    FLASH_EraseInitTypeDef init = {0};
    uint32_t page_err = 0u;
    (void)index;

    init.TypeErase   = FLASH_TYPEERASE_PAGES;
    init.PageAddress = addr;             /* F1 用「页地址」而非页号 */
    init.NbPages     = 1u;

    return (HAL_FLASHEx_Erase(&init, &page_err) == HAL_OK) ? OOP_FLASH_OK : OOP_FLASH_ERR_ERASE;
}

static int plat_unit_program(uint32_t addr, const uint8_t *bytes)
{
    uint16_t half = (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));

    return (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, addr, (uint64_t)half) == HAL_OK)
           ? OOP_FLASH_OK : OOP_FLASH_ERR_WRITE;
}

/* F1：标准标志名（EOP/PGERR/WRPRTERR）。 */
static void plat_clear_flags(void)
{
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_PGERR | FLASH_FLAG_WRPRTERR);
}

#endif /* HAL_PLATFORM_F1 */

/* ============================================================
 * 系列钩子：G4（等长页 2 KB，双 Bank 各 256 KB）
 * ============================================================ */

#if HAL_PLATFORM_G4

/* G4：页 2 KB，每 Bank 256 KB。
 *   Bank1 页号 0..127 → 0x08000000..0x0803FFFF
 *   Bank2 页号 0..127 → 0x08040000..0x0807FFFF（页号在 Bank 内重新从 0 起）
 * ⚠ 本分支未上板验证（SDK 当前无 G4 的 OTA 用例）：首次在 G4 上使用前，
 *   请先用 oop_flash_sector_at() 打印几何，并用一个 scratch 页做擦-写-读回验证。 */
#define OOP_FLASH_G4_PAGE      2048u
#define OOP_FLASH_G4_BANK_SIZE (256u * 1024u)

static void plat_geom_init(oop_flash_info_t *info)
{
    uint32_t kb = plat_size_kb();

    if (kb == 0u) {
        kb = 128u;                       /* 探测失败兜底：G431 的 128 KB */
    }

    info->base         = FLASH_BASE;
    info->size         = kb * 1024u;
    info->sector_count = info->size / OOP_FLASH_G4_PAGE;
    info->unit_size    = OOP_FLASH_G4_PAGE;
    info->write_gran   = 8u;             /* DOUBLEWORD 编程 */
    info->uniform      = true;
}

static void plat_unit_get(uint32_t index, oop_flash_sector_t *out)
{
    const oop_flash_info_t *info = oop_flash_info();

    out->index = index;
    out->base  = info->base + index * info->unit_size;
    out->size  = info->unit_size;
}

static int plat_unit_erase(uint32_t index, uint32_t addr)
{
    FLASH_EraseInitTypeDef init = {0};
    uint32_t page_err = 0u;
    uint32_t offset;
    (void)index;

    if (addr >= (FLASH_BASE + OOP_FLASH_G4_BANK_SIZE)) {
        init.Banks = FLASH_BANK_2;
        offset     = addr - (FLASH_BASE + OOP_FLASH_G4_BANK_SIZE);
    } else {
        init.Banks = FLASH_BANK_1;
        offset     = addr - FLASH_BASE;
    }

    init.TypeErase = FLASH_TYPEERASE_PAGES;
    init.Page      = offset / OOP_FLASH_G4_PAGE;   /* 页号在 Bank 内从 0 起 */
    init.NbPages   = 1u;

    return (HAL_FLASHEx_Erase(&init, &page_err) == HAL_OK) ? OOP_FLASH_OK : OOP_FLASH_ERR_ERASE;
}

static int plat_unit_program(uint32_t addr, const uint8_t *bytes)
{
    uint64_t dword = 0u;

    for (uint32_t i = 0u; i < 8u; i++) {
        dword |= ((uint64_t)bytes[i]) << (8u * i);
    }

    return (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr, dword) == HAL_OK)
           ? OOP_FLASH_OK : OOP_FLASH_ERR_WRITE;
}

/* G4：标志名未逐条核实（该分支本就未上板验证），保守不动以免误用不存在的宏。
 * 首次在 G4 上做 OTA 前，请按 G4 HAL 补 FLASH_FLAG_* 再启用。 */
static void plat_clear_flags(void)
{
}

#endif /* HAL_PLATFORM_G4 */

/* ============================================================
 * 公共核心
 * ============================================================ */

static bool guard_active(void)
{
    return (s_guard_end > s_guard_begin);
}

const oop_flash_info_t *oop_flash_info(void)
{
    if (!s_ready) {
        plat_geom_init(&s_info);
        s_ready = true;
    }
    return &s_info;
}

bool oop_flash_in_range(uint32_t addr, uint32_t len)
{
    const oop_flash_info_t *info = oop_flash_info();

    if (len == 0u) {
        return true;
    }
    if (addr < info->base) {
        return false;
    }
    if (len > (0xFFFFFFFFu - addr)) {          /* 溢出保护 */
        return false;
    }
    return ((addr + len) <= (info->base + info->size));
}

int oop_flash_sector_at(uint32_t addr, oop_flash_sector_t *out)
{
    const oop_flash_info_t *info = oop_flash_info();
    oop_flash_sector_t sec;

    if (addr < info->base || addr >= (info->base + info->size)) {
        return OOP_FLASH_ERR_RANGE;
    }

    if (info->uniform) {
        plat_unit_get((addr - info->base) / info->unit_size, &sec);
    } else {
        uint32_t i;
        for (i = 0u; i < info->sector_count; i++) {
            plat_unit_get(i, &sec);
            if (addr >= sec.base && addr < (sec.base + sec.size)) {
                break;
            }
        }
        if (i == info->sector_count) {
            return OOP_FLASH_ERR_RANGE;
        }
    }

    if (out != NULL) {
        *out = sec;
    }
    return OOP_FLASH_OK;
}

uint32_t oop_flash_sector_size(uint32_t addr)
{
    oop_flash_sector_t sec;

    if (oop_flash_sector_at(addr, &sec) != OOP_FLASH_OK) {
        return 0u;
    }
    return sec.size;
}

void oop_flash_set_guard(uint32_t begin, uint32_t end)
{
    if (begin == 0u && end == 0u) {
        s_guard_begin = 0u;
        s_guard_end   = 0u;
        return;
    }
    s_guard_begin = begin;
    s_guard_end   = end;
}

int oop_flash_unlock(void)
{
    return (HAL_FLASH_Unlock() == HAL_OK) ? OOP_FLASH_OK : OOP_FLASH_ERR_WRITE;
}

int oop_flash_lock(void)
{
    return (HAL_FLASH_Lock() == HAL_OK) ? OOP_FLASH_OK : OOP_FLASH_ERR_WRITE;
}

int oop_flash_read(uint32_t addr, void *buf, uint32_t len)
{
    if (len == 0u) {
        return OOP_FLASH_OK;
    }
    if (buf == NULL) {
        return OOP_FLASH_ERR_PARAM;
    }
    if (!oop_flash_in_range(addr, len)) {
        return OOP_FLASH_ERR_RANGE;
    }
    memcpy(buf, (const void *)addr, len);
    return OOP_FLASH_OK;
}

int oop_flash_erase(uint32_t addr, uint32_t len)
{
    oop_flash_sector_t first;
    oop_flash_sector_t last;

    if (len == 0u) {
        return OOP_FLASH_OK;
    }
    if (!oop_flash_in_range(addr, len)) {
        return OOP_FLASH_ERR_RANGE;
    }

    if (oop_flash_sector_at(addr, &first) != OOP_FLASH_OK) {
        return OOP_FLASH_ERR_RANGE;
    }
    if (oop_flash_sector_at(addr + len - 1u, &last) != OOP_FLASH_OK) {
        return OOP_FLASH_ERR_RANGE;
    }

    /* 擦除会连带整扇区，因此把「展开后的完整扇区范围」交给安全闸复核，
     * 防止 [addr, addr+len) 在窗口内、但所在扇区横跨窗口外时误擦窗口外内容。 */
    if (guard_active() &&
        (first.base < s_guard_begin || (last.base + last.size) > s_guard_end)) {
        return OOP_FLASH_ERR_RANGE;
    }

    if (HAL_FLASH_Unlock() != HAL_OK) {
        return OOP_FLASH_ERR_WRITE;
    }
    plat_clear_flags();   /* 清 BL/上一次操作遗留的错误标志，否则 HAL 立即判错 */

    for (uint32_t i = first.index; i <= last.index; i++) {
        oop_flash_sector_t sec;
        int rc;

        plat_unit_get(i, &sec);
        rc = plat_unit_erase(i, sec.base);
        if (rc != OOP_FLASH_OK) {
            HAL_FLASH_Lock();
            return rc;
        }
    }

    HAL_FLASH_Lock();
    return OOP_FLASH_OK;
}

int oop_flash_write(uint32_t addr, const void *buf, uint32_t len)
{
    const oop_flash_info_t *info = oop_flash_info();
    const uint8_t *src = (const uint8_t *)buf;
    uint8_t  gran[8];                  /* 最大写入单位 = G4 的 8 字节 */
    uint32_t g;
    uint32_t done = 0u;
    uint32_t misalign;
    int      rc;

    if (len == 0u) {
        return OOP_FLASH_OK;
    }
    if (buf == NULL) {
        return OOP_FLASH_ERR_PARAM;
    }
    if (!oop_flash_in_range(addr, len)) {
        return OOP_FLASH_ERR_RANGE;
    }
    /* 写入不会波及范围外，故按实际范围校验安全闸 */
    if (guard_active() && (addr < s_guard_begin || (addr + len) > s_guard_end)) {
        return OOP_FLASH_ERR_RANGE;
    }

    g = info->write_gran;

    if (HAL_FLASH_Unlock() != HAL_OK) {
        return OOP_FLASH_ERR_WRITE;
    }
    plat_clear_flags();   /* 同上：写之前也清一次，避免上一次的错误标志连累本次 */

    /* 1) 头部不足一个写入单位：读回该单位 → 替换目标字节 → 整体重写 */
    misalign = addr % g;
    if (misalign != 0u) {
        uint32_t head = g - misalign;
        if (head > len) {
            head = len;
        }
        memcpy(gran, (const void *)(addr - misalign), g);
        memcpy(&gran[misalign], src, head);
        rc = plat_unit_program(addr - misalign, gran);
        if (rc != OOP_FLASH_OK) {
            HAL_FLASH_Lock();
            return rc;
        }
        done = head;
    }

    /* 2) 中间整单位：直接写 */
    while ((len - done) >= g) {
        rc = plat_unit_program(addr + done, src + done);
        if (rc != OOP_FLASH_OK) {
            HAL_FLASH_Lock();
            return rc;
        }
        done += g;
    }

    /* 3) 尾部不足一个单位：同样读-改-写 */
    if (done < len) {
        uint32_t tail = addr + done;
        memcpy(gran, (const void *)tail, g);
        memcpy(gran, src + done, len - done);
        rc = plat_unit_program(tail, gran);
        if (rc != OOP_FLASH_OK) {
            HAL_FLASH_Lock();
            return rc;
        }
    }

    HAL_FLASH_Lock();
    return OOP_FLASH_OK;
}

int oop_flash_verify(uint32_t addr, const void *buf, uint32_t len, uint32_t *bad_addr)
{
    const uint8_t *exp = (const uint8_t *)buf;

    if (len == 0u) {
        return OOP_FLASH_OK;
    }
    if (buf == NULL) {
        return OOP_FLASH_ERR_PARAM;
    }
    if (!oop_flash_in_range(addr, len)) {
        return OOP_FLASH_ERR_RANGE;
    }

    for (uint32_t i = 0u; i < len; i++) {
        if (*(volatile const uint8_t *)(addr + i) != exp[i]) {
            if (bad_addr != NULL) {
                *bad_addr = addr + i;
            }
            return OOP_FLASH_ERR_MISMATCH;
        }
    }
    return OOP_FLASH_OK;
}

bool oop_flash_is_erased(uint32_t addr, uint32_t len)
{
    uint32_t i = 0u;

    if (len == 0u) {
        return true;
    }
    if (!oop_flash_in_range(addr, len)) {
        return false;
    }

    /* 先按字节推进到 4 字节对齐，再按字比较：既快又不做非对齐访问 */
    while (i < len && ((addr + i) % 4u) != 0u) {
        if (*(volatile const uint8_t *)(addr + i) != 0xFFu) {
            return false;
        }
        i++;
    }
    for (; (i + 4u) <= len; i += 4u) {
        if (*(volatile const uint32_t *)(addr + i) != 0xFFFFFFFFu) {
            return false;
        }
    }
    for (; i < len; i++) {
        if (*(volatile const uint8_t *)(addr + i) != 0xFFu) {
            return false;
        }
    }
    return true;
}
