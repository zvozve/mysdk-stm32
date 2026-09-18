/**
 * @file    bootloader.h
 * @brief   Bootloader 外壳：读状态 → 生效(SWITCH/MOVE) → 试运行/回滚 → 校验 → 跳转
 * @version V1.0
 * @date    2026-09-18
 *
 * BL 的职责边界（这条边界是整个设计的地基）：
 *
 *   BL **能**判：分区表是否自洽、目标槽向量表是否合法（SP/PC）、
 *                刚下载完的镜像 CRC 是否与包头一致。
 *   BL **不能**判：「业务是否正常」—— 那只有 APP 自己知道。
 *
 * 所以「这次升级成功了吗」的判据是：**APP 自检通过后调 ota_confirm()**。
 * 在那之前 BL 把新槽当「试运行」，每上电一次计数 +1，超过 max_try 就撤回旧槽。
 *
 * 用法（BL 工程 main）：
 *
 *     static ota_flash_t      s_flash[1];
 *     static const ota_area_t s_areas[] = { ... };      // User/ota_areas.c
 *     static uint8_t          s_buf[4096];
 *     static boot_ctx_t       s_boot;                   // 勿放栈上
 *
 *     int main(void)
 *     {
 *         HAL_Init(); SystemClock_Config(); MX_GPIO_Init(); ...
 *
 *         ota_env_t env = { s_areas, N, s_flash, 1u };
 *         (void)ota_flash_int_get(&s_flash[0]);
 *         if (ota_init(&env) != OTA_OK) { rescue(); }
 *
 *         boot_cfg_t bc = { s_buf, sizeof(s_buf), 3u, on_boot_event, NULL };
 *         if (boot_init(&bc) != OTA_OK) { rescue(); }
 *
 *         boot_start(&s_boot);
 *         for (;;) {
 *             boot_step_ret_t r = boot_step(&s_boot);
 *             if (r == BOOT_STEP_BUSY)   { continue; }
 *             if (r == BOOT_STEP_READY)  { boot_jump(&s_boot); }   // 成功不返回
 *             rescue();                                            // 没得跳
 *         }
 *     }
 *
 * 跳转本身不在本模块：它是芯片内核操作（VTOR/MSP/NVIC/SysTick + HAL_DeInit），
 * 按 HAL 红线落在 `chip.oop_boot`。本模块只负责「决定跳到哪、凭什么跳」。
 */

#ifndef __BOOTLOADER_H
#define __BOOTLOADER_H

#include <stdint.h>
#include "ota_core.h"
#include "boot_activate.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 启动决策阶段 */
typedef enum {
    BOOT_ST_IDLE     = 0,
    BOOT_ST_CFG      = 1,   /*!< 读状态区（两份都坏 → 出厂默认，不落盘） */
    BOOT_ST_ACTIVATE = 2,   /*!< 执行 pending_action（SWITCH / MOVE） */
    BOOT_ST_HEALTH   = 3,   /*!< 试运行计数 / 回滚判定 */
    BOOT_ST_VERIFY   = 4,   /*!< 校验目标槽向量表 */
    BOOT_ST_READY    = 5,   /*!< 就绪，等 boot_jump() */
    BOOT_ST_RESCUE   = 6,   /*!< 无可用镜像，停在 BL */
    BOOT_ST_FAILED   = 7,
} boot_state_t;

/** @brief 单步返回 */
typedef enum {
    BOOT_STEP_BUSY   =  0,   /*!< 继续调 boot_step */
    BOOT_STEP_READY  =  1,   /*!< 可以跳了（调 boot_jump） */
    BOOT_STEP_RESCUE =  2,   /*!< 需救援，**不要跳** */
    BOOT_STEP_ERR    = -1,   /*!< 出错，看 ctx->err */
} boot_step_ret_t;

/**
 * @brief 事件（给 report 回调：打日志 / 点灯 / 上报）
 * @note  这是 BL 里唯一能观察内部状态的窗口。BL 通常没有 RTOS、也没有 RTT，
 *        所以回调里最实用的是「点灯」和「往串口打一行」。
 */
typedef enum {
    BOOT_EV_CFG_FACTORY = 0,   /*!< 状态区两份都坏，按出厂态处理 */
    BOOT_EV_CFG_FAIL,          /*!< 状态区写失败（介质故障） */
    BOOT_EV_ACTIVATE_SWITCH,   /*!< arg = 目标槽号 */
    BOOT_EV_ACTIVATE_MOVE,     /*!< arg = 待搬字节数 */
    BOOT_EV_MOVE_PROGRESS,     /*!< arg = 已处理字节数（擦除 / 搬运 / 回读共用） */
    BOOT_EV_ACT_FAIL,          /*!< arg = 取正的错误码 */
    BOOT_EV_SLOT_BAD,          /*!< arg = 槽号（向量表不合法） */
    BOOT_EV_TRIAL,             /*!< arg = 已尝试次数 */
    BOOT_EV_ROLLBACK,          /*!< arg = 回滚到的槽号 */
    BOOT_EV_RESCUE,            /*!< 无可用镜像 */
    BOOT_EV_JUMP,              /*!< arg = 槽号；即将跳转（最后一次能打日志的机会） */
} boot_event_t;

typedef void (*boot_report_cb)(boot_event_t ev, uint32_t arg, void *user);

/** @brief BL 配置（boot_init 传入；须常驻） */
typedef struct {
    uint8_t        *buf;       /*!< 搬运缓冲；MOVE 拓扑必填，SWITCH 拓扑可 NULL */
    uint32_t        buf_len;   /*!< 字节数；MOVE 建议 >= 4096 */
    uint8_t         max_try;   /*!< 试运行最大次数；0 = BOOT_DEFAULT_MAX_TRY */
    boot_report_cb  report;    /*!< 可 NULL */
    void           *user;
} boot_cfg_t;

#define BOOT_DEFAULT_MAX_TRY  3u
#define BOOT_MIN_MOVE_BUF     512u

/** @brief 启动上下文（调用方持有；含 ota_cfg_t 与完整上下文，**勿放栈上**） */
typedef struct boot_ctx_s {
    /* ---- 对外可见（只读） ---- */
    boot_state_t state;
    ota_ret_t    err;           /*!< 失败原因 */
    uint8_t      target_slot;   /*!< 最终要跳的槽 */
    uint8_t      rolled_back;   /*!< 1 = 本次启动发生了回滚 */
    ota_cfg_t    cfg;           /*!< 决策后的状态记录 */

    /* ---- 私有 ---- */
    boot_act_t   act;
} boot_ctx_t;

/**
 * @brief  BL 初始化：挂安全闸 + 记录配置
 * @return OTA_OK / OTA_ERR_PARAM / OTA_ERR_NO_AREA / OTA_ERR_MEDIA
 * @note   需先 ota_init()。本函数会把 `chip.oop_flash` 的安全闸收窄成
 *         **[BOOT 区末尾, 内部 Flash 末尾)** —— 于是即使后面分区表算错、
 *         或 MOVE 的地址算错，也擦不到 BL 自己。这是 BL 的最后一道保险。
 *         分区表里必须登记 BOOT 区（role = OTA_AREA_ROLE_BOOT），否则拒绝启动。
 */
int boot_init(const boot_cfg_t *cfg);

/**
 * @brief  开始一轮启动决策
 * @note   ctx 里除 act 外全部清零；真正的工作从第一次 boot_step 开始。
 */
void boot_start(boot_ctx_t *ctx);

/**
 * @brief  推进一步
 * @return BOOT_STEP_BUSY / READY / RESCUE / ERR
 * @note   单步耗时上限 = 一个擦除单位（MOVE 擦除）或一次 buf_len 的读+写（搬运），
 *         调用方在两步之间可以喂狗、刷进度、闪灯。
 */
boot_step_ret_t boot_step(boot_ctx_t *ctx);

/**
 * @brief  终局：落盘 run_slot（仅变化时）+ 跳转
 * @return **成功不返回**；返回错误码表示没跳成（向量表非法 / 区表有问题）
 * @note   run_slot 写失败也会照跳 —— 跳过去至少可能活，不跳必砖；失败经
 *         BOOT_EV_CFG_FAIL 上报。APP 侧若以 CFG 的 run_slot 决定 VTOR，
 *         应同时保留「按自己的链接地址兜底」的能力。
 */
int boot_jump(boot_ctx_t *ctx);

/** @brief 阶段名（日志用） */
const char *boot_state_name(boot_state_t s);

#ifdef __cplusplus
}
#endif

#endif /* __BOOTLOADER_H */
