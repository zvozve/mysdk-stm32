/**
 * @file    ota_source.h
 * @brief   取数后端抽象：任何下载通道都只实现这一个接口
 * @version V1.0
 * @date    2026-09-18
 *
 * ota_core 只认识 `ota_source_t`，不认识串口 / 网络 / FATFS。
 * 每个通道一个独立模块（sync 的选取粒度是目录，这样才能做到「选了 uart 就
 * 完全不引入 lwip/fatfs 的依赖声明」）：
 *
 *   services.ota_src_uart   串口 YMODEM   （依赖 protocols.ymodem + chip.oop_uart）
 *   services.ota_src_http   lwIP HTTP     （依赖 external:lwip）
 *   services.ota_src_tfcard TF 卡 FATFS   （依赖 external:fatfs）
 *   services.ota_src_mem    内存/RAM 测试 （零依赖，单测与自检用）
 *
 * **seek 是否可用决定策略**（ota_flow 据此分流）：
 *   - 能 seek → 双段包可用 `seek` 直接跳到目标段，省一半流量（HTTP 用 Range）
 *   - 不能 seek → 顺序读完，跳过的段直接丢弃（串口场景建议改用单段包）
 */

#ifndef __OTA_SOURCE_H
#define __OTA_SOURCE_H

#include <stdint.h>
#include "ota_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 源信息（open 之后可读） */
typedef struct {
    uint32_t total_size;   /*!< 总字节数；0 = 未知（流式通道常见） */
    uint32_t fw_ver;       /*!< 版本；0 = 未知 */
    uint8_t  seekable;     /*!< 1 = seek 可用 */
    /**
     * @brief 外部期望 CRC32（0 = 未知）
     * @note  通道在 open 之前先拿到元数据（如 UART 的「元数据优先」小文件）时填入。
     *        ota_flow 有它就用它校验整段镜像（能发现 PC 端源文件本身损坏），
     *        没有就退回裸 bin 的「内存 CRC == 闪存 CRC」自校。
     */
    uint32_t expect_crc32;
} ota_src_info_t;

/** @brief 取数后端实例（调用方持有） */
typedef struct ota_source_s {
    const char *name;      /*!< 诊断名："mem" / "uart-ymodem" / "eth-http" … */

    /**
     * @brief 打开（发起传输）
     * @return OTA_OK / OTA_ERR_SOURCE
     */
    int  (*open) (void *ctx, ota_src_info_t *info);

    /**
     * @brief 顺序读
     * @param got  实际读到的字节数；返回 OTA_OK 且 *got==0 表示流已结束
     * @return OTA_OK / OTA_ERR_SOURCE
     */
    int  (*read) (void *ctx, void *buf, uint32_t len, uint32_t *got);

    /**
     * @brief 定位（可为 NULL）
     * @return OTA_OK / OTA_ERR_UNSUPPORTED
     */
    int  (*seek) (void *ctx, uint32_t off);

    /** @brief 关闭（可为 NULL） */
    void (*close)(void *ctx);

    void *ctx;
} ota_source_t;

/** @brief 源是否可用（open/read 必须齐全） */
int ota_source_check(const ota_source_t *s);

#ifdef __cplusplus
}
#endif

#endif /* __OTA_SOURCE_H */
