/**
 * @file    ota_app.h
 * @brief   OTA 的 APP 外壳：早期 VTOR / 启动升级 / 非阻塞步进 / 自检确认
 * @version V1.0
 * @date    2026-09-18
 *
 * 与 `services.bootloader` 的分工：
 *   **BL** 决定「跳到哪个槽、凭什么跳」；**APP** 负责「下载新固件、验证、告知 BL 结果」。
 *   两者共用 `services.ota_core`（分区表 / 状态区 / 包格式 / 介质抽象 / 流程骨架）。
 *   本模块只是 `ota_flow` 外面的一层壳：把「我所在哪个槽」「下载完怎么重启」
 *   「自检通过怎么确认」这三件 APP 才知道的事补齐。
 *
 * 一次升级的完整生命周期：
 *
 *   ① BL：把 `pending_action` 写进状态区 → 跳到目标槽 → 该槽进入**试运行**
 *   ② APP 起：`ota_app_early_init()` 设 VTOR（**必须在 main 第一行**）
 *   ③ APP 自检通过 → `ota_app_confirm()` 固化；没通过就让它复位，
 *      BL 会累计 `boot_try`，超限自动回滚 —— **回滚逻辑全在 BL，APP 不用管**
 *   ④ 下次升级：`ota_app_start()` + 反复 `ota_app_process()`
 *      → 返回 `OTA_APP_DONE` → `ota_app_reboot()` 把控制权交给 BL
 *
 * ⚠ **VTOR 用编译期基址而不是状态区里的 `run_slot`**：
 *   「我在哪个槽」是链接期就确定的事实，用 `-DOTA_SELF_BASE=0x08010000` 告诉本模块
 *   即可；而状态区可能写失败（BL 在 `ota_cfg_save()` 之后才发现），
 *   拿一个可能过期的字段去设 VTOR，会把中断映到另一个槽的向量表上 ——
 *   两个槽是同一份源码的两次链接，向量表内容几乎一样，所以这种错**不会崩**，
 *   只会安静地跑错 handler。这正是最该避免的一类故障。
 *
 * 用法（APP 工程）：
 *
 *     static ota_app_t s_ota;
 *
 *     int main(void)
 *     {
 *         ota_app_early_init(&s_ota, OTA_SELF_BASE);   // ← 第一行
 *         HAL_Init(); SystemClock_Config(); ...
 *
 *         ota_env_t env = { s_areas, N, s_flash, 1u };
 *         (void)ota_flash_int_get(&s_flash[0]);
 *         if (ota_init(&env) != OTA_OK) { ... }
 *
 *         app_main();                                   // 业务起来
 *         // 业务自检通过后：
 *         ota_app_confirm(&s_ota);
 *     }
 */

#ifndef __OTA_APP_H
#define __OTA_APP_H

#include <stdint.h>
#include "ota_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief `ota_app_process()` 返回码 */
#define OTA_APP_IDLE   (2)   /*!< 没有正在进行的升级 */
#define OTA_APP_BUSY   (1)   /*!< 还在跑，继续调 */
#define OTA_APP_DONE   (0)   /*!< 完成（已提交状态区），可以 `ota_app_reboot()` */
#define OTA_APP_ERR    (-1)  /*!< 失败，原因在 `app->flow.err` */

/** @brief 进度/状态上报（可 NULL）。直接透传 `ota_flow_t`，省一层映射 */
typedef void (*ota_app_report_cb)(ota_flow_t *f, void *user);

/** @brief 自检回调：返回非 0 = 健康 */
typedef int (*ota_app_health_cb)(void *user);

/** @brief 上下文（调用方持有；含 flow 的全部状态与 1 KB 级字段，**勿放栈上**） */
typedef struct ota_app_s {
    /* ---- 对外可见（只读） ---- */
    ota_flow_t  flow;         /*!< 进度 / 错误码 / CRC 结果都在这里 */
    uint32_t    self_base;    /*!< 本固件的链接基址 */
    uint8_t     self_slot;    /*!< 由 self_base 反查出的槽号；OTA_SLOT_NONE = 查不到 */
    uint8_t     busy;         /*!< 1 = 有升级在进行 */

    /* ---- 私有 ---- */
    ota_flow_cfg_t      fcfg;
    ota_app_report_cb   report;
    void               *report_user;
    ota_app_health_cb   health;
    void               *health_user;
} ota_app_t;

/**
 * @brief  早期初始化：设 VTOR + 开中断。**必须在 main 第一行**
 * @param  self_base  本固件的链接基址（如 0x08010000），由工程 `-D` 传入
 * @return OTA_OK / OTA_ERR_PARAM
 * @note   只做两件事，不碰外设、不碰时钟、不读 Flash —— 所以它能在
 *         `HAL_Init()` 之前安全执行。
 *         开中断是**兜底**：BL 跳转时已经 `cpsie i`，但如果是别的方式进来的
 *         （调试器直接改 PC、BOOT0 启动），这里补一次更省事。
 */
int ota_app_early_init(ota_app_t *a, uint32_t self_base);

/** @brief 本固件所在槽（查不到返回 OTA_SLOT_NONE），需先 `ota_init()` */
uint8_t ota_app_self_slot(const ota_app_t *a);

/* ---------------- 升级流程 ---------------- */

/**
 * @brief  启动一次升级
 * @param  src, buf, buf_len  取数后端、搬运缓冲（由调用方提供，本模块不分配）
 * @return OTA_OK / OTA_ERR_PARAM / OTA_ERR_STATE（已在升级中）
 * @note   需先 `ota_init()`（注册分区表）。真正的工作从第一次 process 开始。
 */
int ota_app_start(ota_app_t *a, ota_source_t *src, uint8_t *buf, uint32_t buf_len);

/** @brief 推进一步。返回 OTA_APP_IDLE / BUSY / DONE / ERR */
int ota_app_process(ota_app_t *a);

/** @brief 中止本次升级（关源、置 FAILED） */
int ota_app_abort(ota_app_t *a);

/** @brief 进度 0~100（未在升级时返回 0） */
uint8_t ota_app_progress(const ota_app_t *a);

/** @brief 流程状态（日志用） */
ota_flow_state_t ota_app_state(const ota_app_t *a);

/* ---------------- 试运行确认 ---------------- */

/** @brief 本固件是否处于「试运行」（还没被确认） */
int ota_app_is_trial(const ota_app_t *a);

/**
 * @brief  确认本固件可用（自检通过后调）
 * @return OTA_OK / OTA_ERR_MEDIA（状态区写失败）
 * @note   这一步会 `active_slot = 本槽`、清 `trial_slot`、`boot_try = 0`。
 *         之后 BL 每次上电直接跳本槽，**零擦写**。
 *         没调它就一直算试运行，BL 累计若干次后会回滚 —— 这是期望行为。
 */
int ota_app_confirm(ota_app_t *a);

/**
 * @brief  注册自检回调，并提供一个便利入口
 * @note   `ota_app_trial_check()` 的逻辑：在试运行 + 已注册回调 + 回调返回非 0
 *         → 自动 `confirm()`。APP 在 bring-up 结束后调一次即可。
 */
void ota_app_set_health_cb(ota_app_t *a, ota_app_health_cb cb, void *user);

/** @brief 见上。返回 OTA_OK（含「还不到时候」）/ 其它 = confirm 失败 */
int ota_app_trial_check(ota_app_t *a);

/* ---------------- 杂项 ---------------- */

/** @brief 进度/状态上报（透传给 ota_flow） */
void ota_app_set_report_cb(ota_app_t *a, ota_app_report_cb cb, void *user);

/**
 * @brief  重启（交给 BL 生效）。**不返回**
 * @note   只建议在 `ota_app_process()` 返回 `OTA_APP_DONE` 之后调用。
 *         本函数是 `oop_boot_system_reset()` 的转发，services 层因此不需要
 *         CMSIS 符号。
 */
void ota_app_reboot(void) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif /* __OTA_APP_H */
