/**
 * @file    ota_area.h
 * @brief   分区表 schema + 目标区选择（工程侧数据，SDK 只认这张表）
 * @version V1.0
 * @date    2026-09-18
 *
 * 地址是板级事实，所以**表由工程侧提供**（`User/ota_areas.c` 里一个 const 数组，
 * 在 `ota_init(&env)` 时传进来），SDK 不硬编码任何地址、也不 extern 任何全局表。
 *
 * 三种现成布局（见 ota-demo-stm32/doc/01、06）：
 *
 *   ① 内部 A/B（1 MB）
 *        BOOT 32K | CFG 32K | RUN slotA 448K | RUN slotB 512K
 *        → RUN 有 2 个 → 目标 = 另一个 RUN 槽 → OTA_ACT_SWITCH（零搬运）
 *
 *   ② 单槽 + 外挂暂存（512 KB 器件）
 *        [内部] BOOT 32K | CFG 32K | RUN 448K
 *        [外挂] STAGE 若干段
 *        → RUN 只有 1 个 → 目标 = 第一个 STAGE 段 → OTA_ACT_MOVE（BL 逐扇区搬）
 *
 *   ③ 内部 A/B + 外挂备份（1 MB + W25Q）
 *        同 ①，另加 BACKUP 段（存历史版本 / 日志），不参与生效流程
 *
 * **「A/B 切换」与「单槽搬运」不是两套代码**：差异只有「RUN 有几个」与
 * 「pending_action 取什么值」，前者是这张表、后者是状态区一个字段。
 */

#ifndef __OTA_AREA_H
#define __OTA_AREA_H

#include <stdint.h>
#include "ota_common.h"
#include "ota_flash.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 区角色 */
typedef enum {
    OTA_AREA_ROLE_BOOT   = 0,   /*!< Bootloader 自身，不可写（仅登记，供安全闸用） */
    OTA_AREA_ROLE_RUN    = 1,   /*!< 运行区，1~2 个 */
    OTA_AREA_ROLE_STAGE  = 2,   /*!< 暂存区（下载落脚点），0~N 个 */
    OTA_AREA_ROLE_BACKUP = 3,   /*!< 备份 / 历史，可选 */
    OTA_AREA_ROLE_CFG    = 4,   /*!< 状态参数区（内部会被再分成两份乒乓） */
} ota_area_role_t;

/** @brief 分区描述。**base 是该介质内的偏移，不是 CPU 地址** */
typedef struct {
    const char     *name;      /*!< 诊断名，如 "slotA" / "run" / "stage" */
    ota_area_role_t role;
    uint8_t         slot_id;   /*!< role==RUN 时 0/1；其它填 OTA_SLOT_NONE */
    uint8_t         flash_id;  /*!< 介质实例下标（ota_env_t.flashes 的下标） */
    uint32_t        base;      /*!< 介质内偏移 */
    uint32_t        size;      /*!< 字节数 */
} ota_area_t;

/** @brief 目标区选择结果 */
typedef struct {
    ota_act_t         act;          /*!< SWITCH / MOVE */
    const ota_area_t *target;       /*!< SWITCH：目标 RUN 槽；MOVE：STAGE 区（下载落脚点） */
    const ota_area_t *run_target;   /*!< 最终运行区：SWITCH 时同 target；MOVE 时被覆盖的 RUN 区 */
    uint8_t           target_slot;  /*!< SWITCH：目标槽号；MOVE：OTA_SLOT_NONE */
    uint8_t           target_stage; /*!< MOVE：暂存区在表中的下标；否则 OTA_AREA_ID_NONE */
} ota_target_t;

/* ---------------- 表注册与查询 ---------------- */

/**
 * @brief  注册分区表与介质表
 * @param  areas, area_count      工程侧的分区表（须常驻，SDK 只存指针）
 * @param  flashes, flash_count   介质实例表（内部 Flash 用 ota_flash_int_get() 取）
 * @return OTA_OK / OTA_ERR_PARAM / OTA_ERR_NO_AREA（校验不过）
 * @note   本函数会顺带做一次 ota_area_validate()。
 */
int ota_area_init(const ota_area_t *areas, uint8_t area_count,
                  const ota_flash_t *flashes, uint8_t flash_count);

/** @brief 表自检：flash_id 合法、范围落在介质内、同一介质内各区不重叠 */
int ota_area_validate(void);

/** @brief 某角色的区个数 */
uint8_t ota_area_count_role(ota_area_role_t role);

/** @brief 取某角色第 nth 个区（nth 从 0 起）；无则 NULL */
const ota_area_t *ota_area_role_at(ota_area_role_t role, uint8_t nth);

/** @brief 按表内下标取区；越界返回 NULL */
const ota_area_t *ota_area_by_id(uint8_t id);

/** @brief 取区在表内的下标；不属于本表返回 OTA_AREA_ID_NONE */
uint8_t ota_area_id(const ota_area_t *a);

/** @brief 取该区所在的介质实例；无则 NULL */
const ota_flash_t *ota_area_flash(const ota_area_t *a);

/**
 * @brief  取该区的 CPU 绝对地址（= 介质基址 + 偏移）
 * @note   **仅内部介质有意义**（外挂 SPI NOR 不在 CPU 地址空间，返回 0）。
 *         跳转 / VTOR / 段表校验都走这个函数，别在别处再拼一次地址。
 */
uint32_t ota_area_cpu_addr(const ota_area_t *a);

/** @brief 按槽号取 RUN 区（slot_id 0/1）；无则 NULL */
const ota_area_t *ota_area_slot(uint8_t slot_id);

/** @brief RUN 区个数（1 = 单槽+暂存拓扑，2 = 内部 A/B 拓扑） */
uint8_t ota_area_run_count(void);

/* ---------------- 目标区选择 ---------------- */

/**
 * @brief  按表拓扑决定「本次下载往哪写、以及怎么生效」
 * @param  running_slot  当前运行的槽号（仅 RUN 有 2 个时用得上；单槽拓扑忽略）
 * @param  out           输出
 * @return OTA_OK / OTA_ERR_PARAM / OTA_ERR_NO_AREA
 * @note   规则（doc/06 §3）：
 *           RUN 有 2 个 → act=SWITCH，target = 另一个 RUN 槽
 *           RUN 只有 1 个 → act=MOVE，target = 第一个 STAGE 段，run_target = 那个 RUN 区
 *         目标区容量不足由调用方（ota_flow）比对镜像长度后再报 OTA_ERR_NOSPACE。
 */
int ota_area_select_target(uint8_t running_slot, ota_target_t *out);

#ifdef __cplusplus
}
#endif

#endif /* __OTA_AREA_H */
