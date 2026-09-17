/**
 * @file    ch374_sys.h
 * @brief   CH374 驱动层基础数据类型
 * @version V2.0
 * @date    2026-09-17
 *
 * @note    原文件（正点原子 sys.h 派生）含位带操作、GPIOx_ODR/IDR 地址映射、
 *          寄存器位定义与 Stm32_Clock_Init/WFI_SET 等汇编函数声明。
 *          CH374 驱动本身只用到 u8/u16/u32 等短类型名，其余内容均未被引用，
 *          且位带/寄存器宏属于「MCU 寄存器直访问」，按 oop 分层不允许出现在
 *          devices 层，故本文件精简为纯数据类型定义。
 */

#ifndef _SYS_H
#define _SYS_H

#include <stdint.h>

//////////////////////////////////////////////////////////////////////////////////
// 定义一些常用的数据类型短关键字
//////////////////////////////////////////////////////////////////////////////////
typedef int32_t  s32;
typedef int16_t  s16;
typedef int8_t   s8;

typedef const int32_t sc32;
typedef const int16_t sc16;
typedef const int8_t  sc8;

typedef __IO int32_t vs32;
typedef __IO int16_t vs16;
typedef __IO int8_t  vs8;

typedef __I int32_t vsc32;
typedef __I int16_t vsc16;
typedef __I int8_t  vsc8;

typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t  u8;

typedef const uint32_t uc32;
typedef const uint16_t uc16;
typedef const uint8_t  uc8;

typedef __IO uint32_t vu32;
typedef __IO uint16_t vu16;
typedef __IO uint8_t  vu8;

typedef __I uint32_t vuc32;
typedef __I uint16_t vuc16;
typedef __I uint8_t  vuc8;

#endif
