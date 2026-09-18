/**
 * @file    oop_flash_drv.h
 * @brief   内部 Flash OOP 驱动（唯一 HAL_FLASH_* 入口）
 * @version V1.0
 * @date    2026-09-18
 *
 * 设计：
 *  - **板无关**：内部 Flash 无需注入句柄，本层直接坐 HAL_FLASH_* 之上。
 *    「哪一段属于谁、能不能擦」由上层决定（services.ota_core + 工程侧 area 表）。
 *  - **扇区几何按系列内置**：F4 扇区大小不等（16K/64K/128K，含双 Bank 扇号 12~23），
 *    F1/G4 为等长页（F1：≤128 KB 器件 1 KB/页、>128 KB 器件 2 KB/页；G4：2 KB/页）。
 *    oop_flash_erase() 收 [addr, len)，**本层自动按扇区/页展开** —— 上层不必知道粒度，
 *    否则分区表逻辑会被扇区粒度污染。
 *  - **安全闸** oop_flash_set_guard()：收窄允许擦写的窗口，防止上层地址算错时把
 *    Bootloader 自己擦掉。默认不限。
 *  - **不打印日志**（chip 层零 SEGGER 依赖），只返回错误码。
 *
 * 限制（重要）：
 *  - 不处理 D-Cache：F1/F4/G4 无 D-Cache，读回天然一致。将来加 F7/H7 时必须补
 *    SCB_CleanDCache_by_Addr / InvalidateDCache_by_Addr。
 *  - 只支持 F1 / F4 / G4 三个系列（其余系列在编译期 #error，禁止静默产出错误地址）。
 */

#ifndef __OOP_FLASH_DRV_H
#define __OOP_FLASH_DRV_H

#include <stdint.h>
#include <stdbool.h>
#include "hal_platform.h"

#if !(HAL_PLATFORM_F1 || HAL_PLATFORM_F4 || HAL_PLATFORM_G4)
#error "oop_flash: 内部 Flash 几何只实现了 F1 / F4 / G4 三个系列，请先在 oop_flash_drv.c 补该系列的扇区表与擦除分支"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 返回码（负数为错误） */
typedef enum {
    OOP_FLASH_OK         =  0,
    OOP_FLASH_ERR_PARAM  = -1,   /*!< 空指针 / 长度非法 */
    OOP_FLASH_ERR_RANGE  = -2,   /*!< 越出内部 Flash 范围或被安全闸拦下 */
    OOP_FLASH_ERR_ALIGN  = -3,   /*!< 未擦除的数据非法（非 0xFF 区域重复写会失败） */
    OOP_FLASH_ERR_ERASE  = -4,   /*!< HAL_FLASHEx_Erase 失败 */
    OOP_FLASH_ERR_WRITE  = -5,   /*!< HAL_FLASH_Program 失败 */
    OOP_FLASH_ERR_MISMATCH = -6, /*!< 读回比对不一致 */
} oop_flash_ret_t;

/** @brief 一段扇区/页的几何信息 */
typedef struct {
    uint32_t index;   /*!< 扇区（F4）或页（F1/G4）序号，从 0 起 */
    uint32_t base;    /*!< 绝对地址（CPU 地址空间，0x08xxxxxx） */
    uint32_t size;    /*!< 字节数 */
} oop_flash_sector_t;

/** @brief 整片 Flash 几何（由 oop_flash_info() 惰性探测并缓存） */
typedef struct {
    uint32_t base;            /*!< 起址，通常 0x08000000 */
    uint32_t size;            /*!< 总字节数 */
    uint32_t sector_count;    /*!< 扇区/页总数 */
    uint32_t unit_size;       /*!< 等长时的单位大小（仅 uniform==true 有意义） */
    uint32_t write_gran;      /*!< 最小写入单位（字节）：F1=2，F4=4，G4=8 */
    bool     uniform;         /*!< true = 等长页（F1/G4）；false = 变长扇区（F4） */
} oop_flash_info_t;

/* ---------------- 几何查询 ---------------- */

/**
 * @brief  取整片 Flash 几何（首次调用时探测并缓存）
 * @return 静态常驻结构，永不返回 NULL
 */
const oop_flash_info_t *oop_flash_info(void);

/**
 * @brief  查某个地址所属的扇区/页
 * @param  addr  绝对地址
 * @param  out   输出（可为 NULL，仅做合法性校验）
 * @return OOP_FLASH_OK / OOP_FLASH_ERR_RANGE
 */
int oop_flash_sector_at(uint32_t addr, oop_flash_sector_t *out);

/**
 * @brief  取某个地址所在扇区/页的容量（字节）
 * @return >0 字节数；地址越界返回 0
 */
uint32_t oop_flash_sector_size(uint32_t addr);

/**
 * @brief  安全闸：收窄允许擦写的窗口
 * @param  begin,end  允许擦写区间 [begin, end)；传 (0, 0) 解除限制
 * @note   擦除会按扇区展开后再校验，因此「部分落在窗口外」的擦除也会被拒绝，
 *         避免把窗口外的内容连带擦掉。
 */
void oop_flash_set_guard(uint32_t begin, uint32_t end);

/* ---------------- 擦 / 写 / 读 ---------------- */

/**
 * @brief  读回（memcpy 语义，不做任何对齐或范围外的宽容）
 * @return OOP_FLASH_OK / OOP_FLASH_ERR_PARAM / OOP_FLASH_ERR_RANGE
 */
int oop_flash_read(uint32_t addr, void *buf, uint32_t len);

/**
 * @brief  擦除 [addr, addr+len) 覆盖到的所有扇区/页
 * @note   自动向两侧扩展到扇区边界 → **可能擦掉比你要求更多的内容**。
 *         上层若要精确控制，请自己把地址对齐到扇区边界。
 *         内部按扇区逐个擦除（不跨 Bank 批量），杜绝双 Bank 器件的 Bank 选错。
 * @return OOP_FLASH_OK / OOP_FLASH_ERR_PARAM / OOP_FLASH_ERR_RANGE / OOP_FLASH_ERR_ERASE
 */
int oop_flash_erase(uint32_t addr, uint32_t len);

/**
 * @brief  写入 [addr, addr+len)
 * @note   目标必须先擦除（Flash 只能 1→0）。首尾不足一个写入单位的字节用
 *         read-modify-write 补齐：读回该单位、替换其中的目标字节、整体重写。
 *         因此**同一单位内其它字节必须已是最终值**（已擦除或已按最终内容写过），
 *         否则会被本次读改写冲掉。OTA 场景（整区先擦、顺序写入）天然满足。
 * @return OOP_FLASH_OK / OOP_FLASH_ERR_PARAM / OOP_FLASH_ERR_RANGE / OOP_FLASH_ERR_WRITE
 */
int oop_flash_write(uint32_t addr, const void *buf, uint32_t len);

/* ---------------- 辅助 ---------------- */

/**
 * @brief  读回比对（逐字节）
 * @return OOP_FLASH_OK；不一致返回 OOP_FLASH_ERR_MISMATCH（并输出首个不一致的地址）
 * @note   整段完整性校验请用上层 CRC32（services.ota_core），本函数用于小段定位。
 */
int oop_flash_verify(uint32_t addr, const void *buf, uint32_t len, uint32_t *bad_addr);

/**
 * @brief  判断 [addr, addr+len) 是否全为 0xFF（已擦除）
 */
bool oop_flash_is_erased(uint32_t addr, uint32_t len);

/**
 * @brief  范围是否落在内部 Flash 内（不检查安全闸）
 */
bool oop_flash_in_range(uint32_t addr, uint32_t len);

/**
 * @brief  显式解锁 / 加锁
 * @note   擦除与写入函数内部已自动配对 unlock/lock，通常不需要调用。
 *         仅在一段很长的擦写序列希望少开关几次时使用；**不可嵌套**。
 * @return OOP_FLASH_OK / OOP_FLASH_ERR_WRITE
 */
int oop_flash_unlock(void);
int oop_flash_lock(void);

#ifdef __cplusplus
}
#endif

#endif /* __OOP_FLASH_DRV_H */
