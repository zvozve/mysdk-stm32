/**
 * @file    ota_common.h
 * @brief   ota_core 公共定义：返回码、介质类型、槽常量
 * @version V1.0
 * @date    2026-09-18
 */

#ifndef __OTA_COMMON_H
#define __OTA_COMMON_H

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 统一返回码（负数为错误） */
typedef enum {
    OTA_OK              =   0,
    OTA_ERR_PARAM       =  -1,   /*!< 空指针 / 参数非法 */
    OTA_ERR_STATE       =  -2,   /*!< 状态机时序错误（未 init 就 step 等） */
    OTA_ERR_NO_AREA     =  -3,   /*!< area 表里找不到所需角色的区 */
    OTA_ERR_CFG_EMPTY   =  -4,   /*!< 状态区无有效记录（出厂态） */
    OTA_ERR_MEDIA       =  -5,   /*!< 介质读/写/擦失败 */
    OTA_ERR_IMAGE       =  -6,   /*!< 包头非法（magic / 版本 / hdr_crc 不符） */
    OTA_ERR_SEG         =  -7,   /*!< 段表里没有本机目标段 */
    OTA_ERR_VERIFY      =  -8,   /*!< 长度或 CRC 不符 */
    OTA_ERR_SOURCE      =  -9,   /*!< 取数失败 / 流被截断 */
    OTA_ERR_NOSPACE     = -10,   /*!< 镜像超过目标区容量 */
    OTA_ERR_UNSUPPORTED = -11,   /*!< 该组合尚未支持（如流式源 + 需要跳过段） */
} ota_ret_t;

/** @brief 介质类型。**偏移一律相对介质起点**，不是 CPU 地址 */
typedef enum {
    OTA_MEDIA_INT     = 0,   /*!< MCU 内部 Flash（CPU 可直接寻址） */
    OTA_MEDIA_EXT_SPI = 1,   /*!< 外挂 SPI NOR（不在 CPU 地址空间） */
} ota_media_t;

#define OTA_SLOT_A       0u
#define OTA_SLOT_B       1u
#define OTA_SLOT_COUNT   2u
#define OTA_SLOT_NONE    0xFFu

#define OTA_AREA_ID_NONE 0xFFu

/**
 * @brief 待生效动作（状态区 pending_action）
 * @note  「A/B 切换」与「单槽 + 暂存搬运」不是两套代码，差异只有这一个字段：
 *        目标是另一个 RUN 槽 → SWITCH（改 4 个字节）；目标是 STAGE 区 → MOVE（逐扇区搬）。
 */
typedef enum {
    OTA_ACT_NONE   = 0u,   /*!< 无待生效动作 */
    OTA_ACT_SWITCH = 1u,   /*!< 切换运行槽（内部 A/B） */
    OTA_ACT_MOVE   = 2u,   /*!< 从暂存区搬到运行区 */
} ota_act_t;

#ifdef __cplusplus
}
#endif

#endif /* __OTA_COMMON_H */
