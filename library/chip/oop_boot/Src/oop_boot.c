/**
 * @file    oop_boot.c
 * @brief   启动跳转实现（VTOR / MSP / NVIC / SysTick / HAL_DeInit）
 * @version V1.0
 * @date    2026-09-18
 */

#include <stddef.h>
#include "oop_boot.h"

/* SRAM 区范围（Cortex-M 的 SRAM 别名区为 0x20000000~0x3FFFFFFF）。
 * 若某板子的栈放在 CCM / DTCM 等非 0x2000_0000 区（F4 的 CCM 在 0x10000000），
 * 用 -DOOP_BOOT_SRAM_BASE=... / -DOOP_BOOT_SRAM_END=... 覆盖。 */
#ifndef OOP_BOOT_SRAM_BASE
#define OOP_BOOT_SRAM_BASE   0x20000000u
#endif
#ifndef OOP_BOOT_SRAM_END
#define OOP_BOOT_SRAM_END    0x40000000u
#endif

/* VTOR 的低 7 位保留 */
#define OOP_BOOT_VTOR_MASK   0xFFFFFF80u

/* ---------------- 向量表校验 ---------------- */

int oop_boot_vector_check(uint32_t vector_addr, uint32_t *sp_out, uint32_t *pc_out)
{
    uint32_t sp;
    uint32_t pc;

    if (vector_addr == 0u) {
        return OOP_BOOT_ERR_PARAM;
    }
    if ((vector_addr & 0x3u) != 0u) {
        return OOP_BOOT_ERR_ALIGN;
    }

    /* 首两字：初始 MSP 与复位向量。读的是目标固件（可能是刚写进去的），
     * 所以用 volatile 并原样读，不做任何缓存假设。 */
    sp = *(const volatile uint32_t *)vector_addr;
    pc = *(const volatile uint32_t *)(vector_addr + 4u);

    if (sp_out != NULL) {
        *sp_out = sp;
    }
    if (pc_out != NULL) {
        *pc_out = pc;
    }

    /* SP 必须落在 SRAM 区且 4 字节对齐（空区全是 0xFFFFFFFF，会在这里被挡下） */
    if (sp < OOP_BOOT_SRAM_BASE || sp >= OOP_BOOT_SRAM_END) {
        return OOP_BOOT_ERR_VECTOR;
    }
    if ((sp & 0x3u) != 0u) {
        return OOP_BOOT_ERR_VECTOR;
    }

    /* 复位向量：非 0、2 字节对齐、必须是 Thumb 地址（bit0 = 1） */
    if ((pc & ~1u) == 0u) {
        return OOP_BOOT_ERR_VECTOR;
    }
    if ((pc & 1u) == 0u) {
        return OOP_BOOT_ERR_VECTOR;
    }

    return OOP_BOOT_OK;
}

/* ---------------- VTOR ---------------- */

void oop_boot_set_vtor(uint32_t vector_addr)
{
    SCB->VTOR = vector_addr & OOP_BOOT_VTOR_MASK;
    __DSB();
    __ISB();
}

uint32_t oop_boot_get_vtor(void)
{
    return (uint32_t)SCB->VTOR;
}

/* ---------------- 外设复位 ---------------- */

void oop_boot_periph_deinit(void)
{
    __disable_irq();

    /* SysTick：HAL_SuspendTick() 只是关掉它的中断，计数器仍在跑；
     * 目标固件起来后如果重新 Init，残留的 VAL/COUNTFLAG 会带来一次意外中断。
     * 这里彻底停掉：CTRL 清 ENABLE/TICKINT/CLKSOURCE，LOAD/VAL 归零。 */
    SysTick->CTRL = 0u;
    SysTick->LOAD = 0u;
    SysTick->VAL  = 0u;

    /* NVIC：清全部使能与挂起。CMSIS 里没有 NVIC_ClearAllPendingIRQ() 这个函数，
     * 只能自己按寄存器数组循环。 */
    for (uint32_t i = 0u; i < (uint32_t)(sizeof(NVIC->ICER) / sizeof(NVIC->ICER[0])); i++) {
        NVIC->ICER[i] = 0xFFFFFFFFu;      /* 关使能 */
        NVIC->ICPR[i] = 0xFFFFFFFFu;      /* 清挂起 */
    }

    __DSB();
    __ISB();

    HAL_DeInit();                          /* 外设寄存器复位到已知态 */
}

/* ---------------- 跳转 ---------------- */

/**
 * @brief 真正的跳转：换 MSP 后 bx 到复位向量
 * @note  naked：编译器不生成任何序言/尾声，函数体只有汇编，中间不访问栈。
 *        参数按 AAPCS 落在 r0/r1，用 %0/%1 让编译器自己绑定（比硬编码 r0/r1 稳）。
 */
static void oop_boot_jump_asm(uint32_t sp, uint32_t pc) __attribute__((naked));

static void oop_boot_jump_asm(uint32_t sp, uint32_t pc)
{
    __asm volatile (
        "msr   msp, %0   \n\t"
        "dsb             \n\t"
        "isb             \n\t"
        "cpsie i         \n\t"
        "bx    %1        \n\t"
        :
        : "r" (sp), "r" (pc)
    );
}

int oop_boot_jump(uint32_t vector_addr)
{
    uint32_t sp = 0u;
    uint32_t pc = 0u;

    /* 必须在换栈之前做完 —— 之后连 C 函数都不能调 */
    if (oop_boot_vector_check(vector_addr, &sp, &pc) != OOP_BOOT_OK) {
        return OOP_BOOT_ERR_VECTOR;
    }

    oop_boot_periph_deinit();
    oop_boot_set_vtor(vector_addr);

    oop_boot_jump_asm(sp, pc);

    /* 不可达：jump_asm 不返回。留着只为满足函数返回值。 */
    return OOP_BOOT_ERR_PARAM;
}
