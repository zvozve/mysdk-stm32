/**
 * @file    ota_cfg.h
 * @brief   状态参数区：boot_cfg 记录 + 双份乒乓读写（BL 与 APP 共用的单一真相）
 * @version V1.0
 * @date    2026-09-18
 *
 * 为什么必须双份乒乓：`.otapkg` 校验通过后要「提交」一次状态（置 pending_action），
 * 而这一步一旦被掉电打断，整台设备就不知道自己是新版还是旧版。做法是把 CFG 区
 * 对半切成两份，**永远写「当前不是有效」的那一份**，写完再（隐式地）让旧份过期：
 *
 *     写入序列：读两份 → 取 seq 大的那份 → 把新记录（seq+1）写到另一份 → 完成
 *
 * 任何时刻至少有一份是完整可读的，且 seq 单调递增能判定谁更新。断电最坏结果是
 * 丢掉最近一次写入（旧值仍可用），不会出现「两份都坏」。
 *
 * ⚠ 前提：CFG 区必须能对半分，且**半区大小是擦除单位的整数倍**。否则擦第二份时
 *    会连带擦掉第一份（ota_cfg_init 会把这种情况直接判为配置错误，而不是等到写入
 *    时才毁数据）。
 */

#ifndef __OTA_CFG_H
#define __OTA_CFG_H

#include <stdint.h>
#include <stddef.h>
#include "ota_common.h"
#include "ota_area.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_CFG_MAGIC            0x544F4F42u   /*!< 'B','O','O','T'（小端） */
#define OTA_CFG_STRUCT_VER       1u
#define OTA_CFG_MIN_AREA_SIZE    128u          /*!< CFG 区下限（要能对半分） */
#define OTA_CFG_FACTORY_MAGIC    0xFFFFFFFFu   /*!< factory_flag 的「出厂态未初始化」值 */

/** @brief 状态记录（60 字节；CRC 覆盖前 56 字节） */
typedef struct {
    uint32_t magic;            /*!< OTA_CFG_MAGIC */
    uint16_t struct_ver;       /*!< OTA_CFG_STRUCT_VER */
    uint16_t size;             /*!< sizeof(ota_cfg_t)，用于结构演进时判定 */
    uint32_t seq;              /*!< 单调递增；两份都合法时取 seq 大者 */

    uint8_t  active_slot;      /*!< 已确认可运行的槽（2-RUN 拓扑；单槽拓扑恒为 0） */
    uint8_t  trial_slot;       /*!< 待试运行槽；OTA_SLOT_NONE = 无 */
    uint8_t  boot_try;         /*!< 试运行已尝试次数（BL 跳转前自增并落盘） */
    uint8_t  run_slot;         /*!< 本次要跳的槽（BL 每次跳转前写，APP 早期读它设 VTOR） */

    uint8_t  pending_action;   /*!< ota_act_t：NONE / SWITCH / MOVE */
    uint8_t  target_slot;      /*!< SWITCH：目标槽号 */
    uint8_t  stage_area;       /*!< MOVE：暂存区在 area 表里的下标 */
    uint8_t  target_area;      /*!< MOVE：运行区在 area 表里的下标 */

    uint32_t fw_ver[OTA_SLOT_COUNT];   /*!< 各槽固件版本（APP 上报缓存） */
    uint32_t image_size;       /*!< 待生效镜像字节数 */
    uint32_t image_crc32;      /*!< 待生效镜像 CRC32 */
    uint32_t move_progress;    /*!< MOVE：已搬字节数（可重入断点） */
    uint32_t factory_flag;     /*!< 出厂态标记 */
    uint32_t reserved[3];      /*!< 留签名 / 降级策略等扩展 */
    uint32_t crc32;            /*!< 前 OTA_CFG_CRC_LEN 字节的 CRC32 */
} ota_cfg_t;

/** @brief CRC 覆盖长度（= crc32 字段的偏移） */
#define OTA_CFG_CRC_LEN  ((uint32_t)offsetof(ota_cfg_t, crc32))

/* ---------------- 生命周期 ---------------- */

/**
 * @brief  定位 CFG 区并校验可对半分（需先 ota_area_init）
 * @return OTA_OK / OTA_ERR_NO_AREA（无 CFG 区 / 太小 / 半区不是擦除单位整数倍）
 */
int ota_cfg_init(void);

/** @brief CFG 区总大小（字节）；未 init 返回 0 */
uint32_t ota_cfg_area_size(void);

/* ---------------- 读写 ---------------- */

/** @brief 填出厂默认值（active/run = 槽 A、无 trial、无 pending） */
void ota_cfg_defaults(ota_cfg_t *out);

/**
 * @brief  读状态：两份都读，取 magic/size/struct_ver/crc 全过且 seq 最大者
 * @return OTA_OK / OTA_ERR_CFG_EMPTY（两份都坏）/ OTA_ERR_STATE（未 init）
 */
int ota_cfg_load(ota_cfg_t *out);

/** @brief 同 ota_cfg_load，但坏的时候回填出厂默认（不落盘） */
int ota_cfg_load_or_default(ota_cfg_t *out);

/**
 * @brief  写状态：seq 自动 +1，写到「当前无效」的那一份
 * @param  inout  输入载荷；返回时 seq / magic / struct_ver / size / crc32 已被填好
 * @return OTA_OK / OTA_ERR_PARAM / OTA_ERR_MEDIA / OTA_ERR_STATE
 */
int ota_cfg_save(ota_cfg_t *inout);

/* ---------------- 便捷判定 ---------------- */

/** @brief 是否处于「新固件试运行」状态 */
int ota_cfg_is_trial(const ota_cfg_t *c);

/** @brief 是否出厂态（从未成功提交过状态） */
int ota_cfg_is_factory(const ota_cfg_t *c);

/** @brief 当前应跳转的槽（APP 早期用它设 VTOR）；失败返回 OTA_SLOT_A */
uint8_t ota_cfg_run_slot(const ota_cfg_t *c);

#ifdef __cplusplus
}
#endif

#endif /* __OTA_CFG_H */
