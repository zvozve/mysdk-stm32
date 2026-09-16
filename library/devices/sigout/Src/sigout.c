/* ============================================================
 * sigout.c - 外部状态输出（SEL1/2 + OUT1..4 脉冲）
 *
 * SDK 版：由工程 Hardware/sigout 迁入 devices 层。去掉 CubeMX main 头
 * 硬编码引脚，改 SIGOUT_Init(map) 注入 6 路引脚映射；bsp_gpio → oop_gpio，
 * bsp_dwt → oop_dwt。脉冲状态机（到位 1ms / 反弹 200us）保持原语义。
 * ============================================================ */

#include "sigout.h"
#include "oop_gpio_drv.h"
#include "oop_dwt.h"

#define SIGOUT_CH_MAX           2
#define SIGOUT_REACH_PULSE_US   1000   /* 到位脉冲宽度 1ms（外部/PLC 可靠捕获） */
#define SIGOUT_BOUNCE_PULSE_US  200    /* 反弹报警脉冲宽度 200us */

static gpio_dev_t s_sel_io[SIGOUT_CH_MAX]    = {0};   /* [0]=SEL1(A) [1]=SEL2(B) */
static gpio_dev_t s_reach_io[SIGOUT_CH_MAX]  = {0};   /* [0]=OUT1(A) [1]=OUT3(B) */
static gpio_dev_t s_bounce_io[SIGOUT_CH_MAX] = {0};   /* [0]=OUT2(A) [1]=OUT4(B) */

/* 脉冲状态：kind 0=到位 1=反弹 */
static bool     s_pulse_active[2][SIGOUT_CH_MAX] = {{0}};
static uint32_t s_pulse_t0[2][SIGOUT_CH_MAX]     = {{0}};

void SIGOUT_Init(const sigout_pin_map_t *map) {
    if (!map) return;
    oop_gpio_init_output(&s_sel_io[0],    map->ch[0].sel_port,    map->ch[0].sel_pin,    true);
    oop_gpio_init_output(&s_sel_io[1],    map->ch[1].sel_port,    map->ch[1].sel_pin,    true);
    oop_gpio_init_output(&s_reach_io[0],  map->ch[0].reach_port,  map->ch[0].reach_pin,  true);
    oop_gpio_init_output(&s_reach_io[1],  map->ch[1].reach_port,  map->ch[1].reach_pin,  true);
    oop_gpio_init_output(&s_bounce_io[0], map->ch[0].bounce_port, map->ch[0].bounce_pin, true);
    oop_gpio_init_output(&s_bounce_io[1], map->ch[1].bounce_port, map->ch[1].bounce_pin, true);
}

void SIGOUT_SetSel(uint8_t ch, bool sel) {
    if (ch >= SIGOUT_CH_MAX) return;
    if (oop_gpio_is_initialized(&s_sel_io[ch])) {
        oop_gpio_write(&s_sel_io[ch], sel);
    }
}

/* 触发一个脉冲：置高并记录起始时间（同通道再次触发会重起计时） */
static void sigout_pulse_trigger(gpio_dev_t *dev, bool active[2][SIGOUT_CH_MAX],
                                 uint32_t t0[2][SIGOUT_CH_MAX], uint8_t kind, uint8_t ch) {
    if (!oop_gpio_is_initialized(dev)) return;
    oop_gpio_set_high(dev);
    active[kind][ch] = true;
    t0[kind][ch]     = oop_GetCycleCount();
}

void SIGOUT_ReachPulse(uint8_t ch) {
    if (ch >= SIGOUT_CH_MAX) return;
    sigout_pulse_trigger(&s_reach_io[ch], s_pulse_active, s_pulse_t0, 0, ch);
}

void SIGOUT_BouncePulse(uint8_t ch) {
    if (ch >= SIGOUT_CH_MAX) return;
    sigout_pulse_trigger(&s_bounce_io[ch], s_pulse_active, s_pulse_t0, 1, ch);
}

/* 每轮检查脉冲到点自动复位（到位 1ms / 反弹 200us） */
void SIGOUT_Tick(void) {
    for (int c = 0; c < SIGOUT_CH_MAX; c++) {
        if (s_pulse_active[0][c] &&
            oop_IsTimeout(s_pulse_t0[0][c], SIGOUT_REACH_PULSE_US)) {
            oop_gpio_set_low(&s_reach_io[c]);
            s_pulse_active[0][c] = false;
        }
        if (s_pulse_active[1][c] &&
            oop_IsTimeout(s_pulse_t0[1][c], SIGOUT_BOUNCE_PULSE_US)) {
            oop_gpio_set_low(&s_bounce_io[c]);
            s_pulse_active[1][c] = false;
        }
    }
}
