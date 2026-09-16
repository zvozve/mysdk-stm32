#ifndef __SIGOUT_H__
#define __SIGOUT_H__

#include <stdint.h>
#include <stdbool.h>
#include "hal_platform.h"   /* GPIO_TypeDef / uint16_t 等类型 */

/* ============================================================
 * 外部状态输出：SEL1/SEL2 触发电平选择 + OUT1..4 到位/反弹脉冲
 *  - SEL1/SEL2 ：A/B 触发电平选择（线圈 2/3 控制），电平跟随
 *  - OUT1/OUT3 ：A/B 动作到位脉冲（GH2SE_EVT_REACH 触发，1ms）
 *  - OUT2/OUT4 ：A/B 反弹报警脉冲（GH2SE_EVT_BOUNCE_ALARM 触发，200us）
 * 脉冲语义：事件时置高，由 SIGOUT_Tick 计时到点自动复位（与旧驱动同款）。
 *
 * SDK 版：由工程 Hardware/sigout 迁入 devices 层。去掉 CubeMX main 头
 * 硬编码引脚，改为 SIGOUT_Init(map) 注入 6 路引脚映射（板级绑定）；
 * GPIO 走 oop_gpio，计时走 oop_dwt。引脚映射 / 脉冲状态机独立成模块。
 * ============================================================ */

/* 单通道 3 路输出引脚（板级注入） */
typedef struct {
    GPIO_TypeDef *sel_port;    uint16_t sel_pin;    /* SEL1(A) / SEL2(B) */
    GPIO_TypeDef *reach_port;  uint16_t reach_pin;  /* OUT1(A) / OUT3(B) */
    GPIO_TypeDef *bounce_port; uint16_t bounce_pin; /* OUT2(A) / OUT4(B) */
} sigout_pin_t;

/* 双通道引脚映射（[0]=A [1]=B） */
typedef struct {
    sigout_pin_t ch[2];
} sigout_pin_map_t;

/** 初始化 6 路输出引脚（SEL1/2 + OUT1..4，默认低电平） */
void SIGOUT_Init(const sigout_pin_map_t *map);

/** SEL1/SEL2 电平跟随（ch: 0=A 1=B, sel: 选择位） */
void SIGOUT_SetSel(uint8_t ch, bool sel);

/** 到位脉冲（OUT1=A / OUT3=B，1ms）——同通道重复触发会重起计时 */
void SIGOUT_ReachPulse(uint8_t ch);

/** 反弹报警脉冲（OUT2=A / OUT4=B，200us） */
void SIGOUT_BouncePulse(uint8_t ch);

/** 每轮主循环调用：脉冲到点自动复位 */
void SIGOUT_Tick(void);

#endif /* __SIGOUT_H__ */
