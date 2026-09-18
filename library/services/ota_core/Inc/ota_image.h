/**
 * @file    ota_image.h
 * @brief   .otapkg 固件包格式（80 字节头）+ 校验器接口
 * @version V1.0
 * @date    2026-09-18
 *
 * 为什么要自己的包头（而不是「裸 bin + 侧信道元数据」）：
 * 元数据（长度 / CRC）放 MQTT JSON 或 HTTP 自定义头里，换个通道就没了。
 * 把元数据钉在包内，**任何传输通道都能自校验**，BL 与 APP 也能各自独立校验。
 * PC 侧由 tools/ota_pack.py 生成（见该脚本 --help）。
 *
 * 头布局（小端，共 80 字节；字段偏移与 doc/01 §3.1 一致）：
 *   0  magic(4)  4 hdr_ver(2)  6 hdr_size(2)  8 pkg_size(4)  12 fw_ver(4)  16 build_id(4)
 *   20 seg_count(1)  21 flags(1)  22 reserved16(2)
 *   24 seg[0] {load_addr,size,crc32}      36 seg[1] {load_addr,size,crc32}
 *   48 reserved[7] (28)                   76 hdr_crc32(4)
 *
 * CRC 参数在 ota_crc32.h 里写死（CRC-32/ISO-HDLC，= zlib.crc32）。
 */

#ifndef __OTA_IMAGE_H
#define __OTA_IMAGE_H

#include <stdint.h>
#include "ota_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_PKG_MAGIC     0x5041544Fu   /*!< 'O','T','A','P'（小端读作 "OTAP"） */
#define OTA_PKG_HDR_VER   1u
#define OTA_PKG_HDR_SIZE  80u
#define OTA_PKG_SEG_MAX   2u

#define OTA_PKG_FLAG_HAS_BL   0x01u
#define OTA_PKG_FLAG_SIGNED   0x02u

/** @brief 镜像段（一个槽一段） */
typedef struct {
    uint32_t load_addr;   /*!< 段目标地址。内部介质 = CPU 绝对地址（如 0x08010000） */
    uint32_t size;        /*!< 段字节数 */
    uint32_t crc32;       /*!< 段 CRC32（CRC-32/ISO-HDLC） */
} ota_seg_t;

/** @brief 固件包头 */
typedef struct {
    uint32_t  magic;
    uint16_t  hdr_ver;
    uint16_t  hdr_size;
    uint32_t  pkg_size;      /*!< 整包字节数 */
    uint32_t  fw_ver;        /*!< major<<24 | minor<<16 | patch<<8 | stage */
    uint32_t  build_id;      /*!< 构建时间戳 */
    uint8_t   seg_count;     /*!< 1 = 单段包（串口用），2 = 双段包（HTTP 可只取目标段） */
    uint8_t   flags;
    uint16_t  reserved16;
    ota_seg_t seg[OTA_PKG_SEG_MAX];
    uint32_t  reserved[7];
    uint32_t  hdr_crc32;     /*!< 头部前 76 字节的 CRC32 */
} ota_pkg_hdr_t;

/* ---------------- 包解析 ---------------- */

/**
 * @brief  解析包头并自检（magic / hdr_ver / hdr_size / hdr_crc32 / 段表合理性）
 * @param  buf  至少 OTA_PKG_HDR_SIZE 字节
 * @param  len  buf 实际长度
 * @param  out  输出
 * @return OTA_OK / OTA_ERR_PARAM / OTA_ERR_IMAGE
 */
int ota_image_parse(const void *buf, uint32_t len, ota_pkg_hdr_t *out);

/**
 * @brief  按目标地址找段，并算出该段数据在包内的偏移
 * @param  h         已解析的包头
 * @param  load_addr 目标段的目标地址（SWITCH = 目标 RUN 槽 CPU 地址；MOVE = RUN 区 CPU 地址）
 * @param  seg       输出段描述
 * @param  data_off  输出段数据在包内的字节偏移
 * @return OTA_OK / OTA_ERR_PARAM / OTA_ERR_SEG
 */
int ota_image_find_seg(const ota_pkg_hdr_t *h, uint32_t load_addr,
                       const ota_seg_t **seg, uint32_t *data_off);

/** @brief 版本号 -> 人读字符串（如 "1.2.3"）；buf 至少 16 字节 */
void ota_image_ver_str(uint32_t fw_ver, char *buf, uint32_t buf_len);

/* ---------------- 校验器 ---------------- */

/** @brief 校验强度（预留扩展：加 SHA256 / 签名时换一个实现即可） */
typedef enum {
    OTA_VERIFY_NONE  = 0,
    OTA_VERIFY_CRC32 = 1,
} ota_verify_kind_t;

/** @brief CRC32 校验器状态（由调用方持有，无动态分配） */
typedef struct {
    uint32_t running;   /*!< 累加中的 CRC32 */
    uint32_t expect;    /*!< 期望值（来自包头段表） */
    uint32_t total;     /*!< 期望字节数；0 = 不校验长度 */
    uint32_t done;      /*!< 已喂入字节数 */
} ota_crc32_state_t;

/** @brief 校验器接口：上层只调 init/update/finish */
typedef struct ota_verifier_s {
    ota_verify_kind_t kind;
    int  (*init)  (void *ctx, uint32_t expect, uint32_t size);
    int  (*update)(void *ctx, const void *buf, uint32_t len);
    int  (*finish)(void *ctx);
    void  *ctx;
} ota_verifier_t;

/**
 * @brief  把 verifier 绑定到 CRC32 实现
 * @param  v       输出
 * @param  st      调用方持有的状态
 * @param  expect  期望 CRC32（来自段表）
 * @note   finish() 同时校验长度与 CRC，任一不符返回 OTA_ERR_VERIFY。
 */
void ota_verifier_crc32(ota_verifier_t *v, ota_crc32_state_t *st, uint32_t expect);

#ifdef __cplusplus
}
#endif

#endif /* __OTA_IMAGE_H */
