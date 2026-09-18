/**
 * @file    oop_boot.h
 * @brief   启动跳转：向量表校验 / 外设复位 / VTOR / MSP / 跳转
 * @version V1.0
 * @date    2026-09-18
 *
 * 为什么「跳转」在 chip 层而不是 services 层：
 *   跳转是一组**芯片内核操作**（VTOR / MSP / NVIC / SysTick）外加 `HAL_DeInit()`。
 *   HAL 红线规定只有 chip 层允许出现 HAL 符号，所以 bootloader 外壳（services 层）
 *   只负责「决策 + 校验 + 搬运」，最后把控制权交给本模块。
 *
 * 跳转前必须做的四件事（doc/04）：
 *   ① 关全局中断            ② 停 SysTick（**只关它的中断不够，计数器还在跑**）
 *   ③ 清 NVIC 使能与挂起     ④ HAL_DeInit（把外设复位到已知态）
 * 之后才是：校验向量表 → 设 VTOR → 设 MSP → 开中断 → 跳。
 *
 * ⚠ 有一处顺序不能颠倒：**SP 合法性检查必须在 __set_MSP() 之前做完**。
 *   MSP 一换，栈上所有局部变量（含返回地址）都失效，此后连普通 C 函数都不能调。
 *   所以真正的跳转由 naked 汇编函数完成，中间不碰栈。
 */

#ifndef __OOP_BOOT_H
#define __OOP_BOOT_H

#include <stdint.h>
#include <stdbool.h>
#include "hal_platform.h"

#if !(HAL_PLATFORM_F1 || HAL_PLATFORM_F4 || HAL_PLATFORM_G4)
#error "oop_boot: 只验证过 F1 / F4 / G4，补该系列后请删掉本行"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 返回码 */
typedef enum {
    OOP_BOOT_OK         =  0,   /*!< 校验通过（oop_boot_jump 成功时不会返回） */
    OOP_BOOT_ERR_PARAM  = -1,   /*!< 空地址 / 参数非法 */
    OOP_BOOT_ERR_ALIGN  = -2,   /*!< 向量表地址未 4 字节对齐 */
    OOP_BOOT_ERR_VECTOR = -3,   /*!< 向量表首两字（SP / PC）不合法 */
} oop_boot_ret_t;

/**
 * @brief  校验向量表：取首两字并做合法性判断
 * @param  vector_addr  向量表基址（CPU 地址）
 * @param  sp_out,pc_out  输出（可为 NULL）
 * @return OOP_BOOT_OK / OOP_BOOT_ERR_*
 * @note   判据（保守但零成本，读 8 字节）：
 *           - vector_addr 4 字节对齐
 *           - SP 落在 SRAM 区 [OOP_BOOT_SRAM_BASE, OOP_BOOT_SRAM_END) 且 4 字节对齐
 *           - PC 非 0、2 字节对齐、Thumb 位（bit0）为 1
 *         这能把「空区（0xFF...）」「未下载」「写坏的槽」挡在跳转之前。
 *         想做更严格的「PC 必须落在本槽范围内」检查，由调用方拿 area 表自己比对。
 */
int oop_boot_vector_check(uint32_t vector_addr, uint32_t *sp_out, uint32_t *pc_out);

/**
 * @brief  设 VTOR 并同步指令流（APP 早期设自己的向量表 / BL 跳转前设目标向量表）
 */
void oop_boot_set_vtor(uint32_t vector_addr);

/** @brief 当前 VTOR 值（诊断用） */
uint32_t oop_boot_get_vtor(void);

/**
 * @brief  复位外设到「跳转前」状态：关中断 + 停 SysTick + 清 NVIC + HAL_DeInit
 * @note   幂等，可单独调用。调用后本工程的外设（UART 日志、定时器…）即失效，
 *         所以要先打日志 / 点灯，再调它。
 *         **故意不在这里恢复中断**：开中断的时机由 oop_boot_jump() 掌握。
 */
void oop_boot_periph_deinit(void);

/**
 * @brief  终局：校验 → 复位外设 → 设 VTOR → 设 MSP → 开中断 → 跳转
 * @param  vector_addr  目标固件向量表基址（= 目标区 CPU 地址）
 * @return **成功不返回**；向量表非法时返回错误码（此时未做任何破坏性操作，
 *         调用方可以转去救援模式）。
 */
int oop_boot_jump(uint32_t vector_addr);

#ifdef __cplusplus
}
#endif

#endif /* __OOP_BOOT_H */
