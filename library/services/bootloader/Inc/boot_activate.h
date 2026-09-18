/**
 * @file    boot_activate.h
 * @brief   待生效动作执行：SWITCH（改槽号）与 MOVE（逐扇区搬运）
 * @version V1.0
 * @date    2026-09-18
 *
 * 两条路径由 `ota_cfg_t.pending_action` 分流，但**共用同一段代码**：
 *
 *   OTA_ACT_SWITCH —— 内部 A/B：APP 已把新镜像写进另一个运行槽，
 *                     BL 只需把 active_slot 改成它（几毫秒，改 4 个字节）。
 *
 *   OTA_ACT_MOVE   —— 单运行槽 + 暂存：APP 把新镜像写进 STAGE 区（内部或外挂），
 *                     BL 负责把它搬进运行区（几百 KB，按扇区步进）。
 *
 * MOVE 的断电安全（为什么可以「按扇区搬 + 先写数据后写进度」就够）：
 *   暂存区在搬完之前是**只读源**，全程只写运行区、不做原地交换，所以
 *   「重启后从进度所在扇区起点重搬」是幂等的 —— 源没被破坏，重搬多少次结果一样。
 *   代价只是多搬一次，而不是数据不一致。
 *
 *   反过来，「运行区 ↔ 暂存区就地交换」那种省空间的花招被明确否决：它的幂等性要靠
 *   复杂协议才成立，而省下的是便宜的外挂容量，不值。
 *
 * 擦除是幂等的（重擦若干扇区结果仍是全 0xFF），所以擦除阶段不记断点，每次从头擦。
 */

#ifndef __BOOT_ACTIVATE_H
#define __BOOT_ACTIVATE_H

#include <stdint.h>
#include "ota_common.h"
#include "ota_area.h"
#include "ota_cfg.h"
#include "ota_flash.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 生效动作阶段 */
typedef enum {
    BACT_IDLE = 0,
    BACT_VERIFY,          /*!< 校验下载到位的镜像（完整 CRC32；分块读） */
    BACT_SWITCH_COMMIT,   /*!< SWITCH：改状态区 */
    BACT_MOVE_ERASE,      /*!< MOVE：擦运行区（每次一步一个擦除单位） */
    BACT_MOVE_COPY,       /*!< MOVE：逐块搬运（按擦除单位记断点） */
    BACT_MOVE_VERIFY,     /*!< MOVE：读回运行区重算 CRC32 */
    BACT_MOVE_COMMIT,     /*!< MOVE：改状态区 */
    BACT_DONE,
    BACT_FAILED,
} boot_act_st_t;

/** @brief 进度回调：done/total 由上层翻译成日志 / 点灯 / 上报 */
typedef void (*boot_act_prog_cb)(void *user, boot_act_st_t st, uint32_t done, uint32_t total);

/** @brief 生效动作上下文（调用方持有；含缓冲指针与游标，勿放栈上临时变量） */
typedef struct {
    boot_act_st_t      st;
    ota_ret_t          err;
    ota_act_t          act;          /*!< SWITCH / MOVE */
    const ota_area_t  *chk;          /*!< 待校验区：SWITCH = 目标槽；MOVE = STAGE 区 */
    const ota_area_t  *src;          /*!< MOVE：STAGE 区；SWITCH：NULL */
    const ota_area_t  *dst;          /*!< 最终运行区 */
    const ota_flash_t *f_chk;
    const ota_flash_t *f_src;
    const ota_flash_t *f_dst;
    uint8_t            slot;         /*!< 生效后应运行的槽号 */
    uint32_t           total;        /*!< 字节数 */
    uint32_t           cur;          /*!< 进度游标（擦除 / 搬运 / 回读共用） */
    uint32_t           unit_end;     /*!< 当前擦除单位终点（搬运分段用；0 = 需重算） */
    uint32_t           crc;          /*!< 累计 CRC32 */
    uint8_t           *buf;
    uint32_t           buf_len;
    ota_cfg_t         *cfg;          /*!< 工作副本；本模块负责 ota_cfg_save() */
    boot_act_prog_cb   prog;
    void              *user;
} boot_act_t;

/** @brief MOVE 搬运缓冲下限（小于它拒绝启动，避免把 448 KB 变成长阻塞） */
#define BOOT_ACT_MIN_BUF  512u

/**
 * @brief  按 cfg->pending_action 准备生效动作
 * @param  a        上下文（内部先清零）
 * @param  cfg      状态记录工作副本，**须常驻**（本模块会就地改并落盘）
 * @param  buf,buf_len  搬运缓冲；MOVE 必填，SWITCH 可为 NULL
 * @param  prog     进度回调，可 NULL
 * @return OTA_OK / OTA_ERR_PARAM / OTA_ERR_NO_AREA / OTA_ERR_IMAGE / OTA_ERR_NOSPACE
 * @note   失败时 a->st 置 BACT_FAILED，便于调用方区分「未启动」与「启动失败」。
 */
int boot_act_start(boot_act_t *a, ota_cfg_t *cfg, uint8_t *buf, uint32_t buf_len,
                   boot_act_prog_cb prog, void *user);

/**
 * @brief  推进一步
 * @return 0 = BUSY / 1 = DONE / -1 = ERR（原因见 a->err）
 * @note   单步耗时上限 ≈ 一个擦除单位（擦除）或一次 buf_len 的读+写（搬运），
 *         所以调用方在两步之间有机会喂狗、刷进度。
 */
int boot_act_step(boot_act_t *a);

/** @brief 阶段名（日志用） */
const char *boot_act_st_name(boot_act_st_t s);

#ifdef __cplusplus
}
#endif

#endif /* __BOOT_ACTIVATE_H */
