/**
 * @file    ota_crc32.h
 * @brief   CRC-32/ISO-HDLC（与 PC 端 zlib.crc32 完全一致）
 * @version V1.0
 * @date    2026-09-18
 *
 * 参数**写死**为 poly=0x04C11DB7 / init=0xFFFFFFFF / refin=true / refout=true /
 * xorout=0xFFFFFFFF —— 也就是 `zlib.crc32()`／`crc32` 命令／绝大多数上位机库的结果。
 * 不要在这里改参数、也不要在工程侧另写一份变体：refin 或 init 写错是「两边算不对」
 * 最经典的来源，而它不会报错，只会让你怀疑数据。
 *
 * 用法（可流式累加，初值传 0）：
 *   uint32_t c = ota_crc32(0, buf1, n1);
 *   c = ota_crc32(c, buf2, n2);          // 与 ota_crc32(0, 整段, n1+n2) 等值
 */

#ifndef __OTA_CRC32_H
#define __OTA_CRC32_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 查表版 CRC32（约 1 KB 表在 Flash，速度快，默认用这个）
 * @param crc  上一次的返回值；首次调用传 0
 * @param buf  数据
 * @param len  字节数
 * @return 累加后的 CRC32
 */
uint32_t ota_crc32(uint32_t crc, const void *buf, uint32_t len);

/**
 * @brief 位运算版 CRC32（不占 1 KB 表，慢 6~8 倍）
 * @note  给 BL 这类体积敏感、而校验次数很少的场合用（448 KB 约多花十几 ms）。
 *        结果与 ota_crc32() 完全相同。
 */
uint32_t ota_crc32_bitwise(uint32_t crc, const void *buf, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* __OTA_CRC32_H */
