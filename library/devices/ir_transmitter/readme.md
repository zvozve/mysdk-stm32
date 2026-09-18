# ir_transmitter — 红外发射驱动（TIM 载波 + CCR 门控）

把学习到的 mark/space 时长序列原样重放出去。载波由定时器硬件产生（频率/占空比精确），
段时长用一路**注入的自由运行定时器**计时。

- **层**：`library/devices/ir_transmitter`
- **版本**：V2.0（2026-09-18 由「DWT 微秒延时 + Start/Stop PWM 门控」改为
  **注入 TIM 时基 + 载波常开 CCR 门控**）
- **依赖**：`chip.oop_tim`（TIM 原语）、`chip.platform`

## 两处关键改动（都来自工程侧实测）

### 1. 载波常开，用 CCR 做门控

```
mark  ->  CCR = ccr_on   （PWM mode1，CNT<CCR 输出有效电平）
space ->  CCR = 0        （恒无效电平）
```

相比「每段 Start/Stop PWM」，这是**单次寄存器写**，几乎无抖动。

前提：**必须关掉输出比较预装载（OCxPE=0）**，否则 CCR 要等下一个更新事件才生效，
最坏引入一个载波周期（38kHz 时 26µs）的抖动。驱动在 `init` 里显式关掉，
不依赖 CubeMX 勾选项（`.ioc` 里 `OCxPreload=DISABLE` 只是双保险）。

发送前驱动会把载波计数器 `CNT` 清零以对齐相位，让首段 mark 干净起步。

### 2. µs 时基用注入的 TIM，不用 DWT

工程侧实测：**DWT CYCCNT 在这类 F1 板上不可靠**（自检偶发通过、发码时却冻结）。
改用一路空闲定时器配成 **1MHz 自由运行计数器**（`PSC = TIM_CLK/1e6 - 1`、`ARR = 0xffff`），
与接收侧捕获定时器同源方案 —— 可靠，且不占 CPU 周期。

不用 DWT 带来的两个副作用，都由驱动内部处理掉：
- **回绕**：16bit@1MHz 每 65.5ms 回绕一次。驱动用环形减法（模数 = `ARR+1`）比较，
  不是裸减。单段上限因此是 ~65ms（IR 帧单段远小于它）。
- **假死**：时基若冻结，等待循环会挂死主循环。驱动带死机保护 ——
  连续读到同一计数值超阈值就判定冻结，置 `ready=false` 并让本次发送返回失败，
  后续发送立即失败（不会静默发出时序全错的波形）。
  另外 `init` 里会**探测时基是否真的在计数**，不计数就直接 init 失败。

## 接入步骤

1. **CubeMX**：
   - 一路定时器配 **PWM Generation CHx**，通道接到发射引脚 AF；`PSC` 填 0
     （`ARR/Pulse` 只是为了让 CubeMX 生成通道，实际值由驱动按 `carrier_hz/duty` 覆写）。
   - **另一路空闲定时器**（Basic 或通用定时器皆可，不要引脚、不要中断）作为 µs 时基。
     `TIMx` 的时钟由 CubeMX 的 MspInit 打开即可，参数由驱动设置。
2. **board_cfg.h**：
   ```c
   /* 载波 PWM */
   #define BOARD_IR_TX_TIM            (&htim3)
   #define BOARD_IR_TX_TIM_CHANNEL    TIM_CHANNEL_2
   #define BOARD_IR_TX_TIM_CLK_HZ     72000000UL     /* 载波定时器计数时钟 */

   /* µs 时基（自由运行计数器） */
   #define BOARD_IR_TX_TICK_TIM        (&htim4)
   #define BOARD_IR_TX_TICK_CLK_HZ     72000000UL
   ```
3. **用户代码**：
   ```c
   static ir_transmitter_t s_tx;

   void ir_transmitter_task_init(void)
   {
       ir_transmitter_cfg_t cfg = {
           .htim             = BOARD_IR_TX_TIM,
           .channel          = BOARD_IR_TX_TIM_CHANNEL,
           .tim_clk_hz       = BOARD_IR_TX_TIM_CLK_HZ,
           .carrier_hz       = 38000U,
           .duty_percent     = 33U,
           .inverted         = false,   /* 待机常亮就改 true：模块是低有效 */
           .use_carrier      = true,    /* 模块自带 38kHz 振荡时改 false */
           .tick_htim        = BOARD_IR_TX_TICK_TIM,
           .tick_tim_clk_hz  = BOARD_IR_TX_TICK_CLK_HZ,
       };
       (void)ir_transmitter_init(&s_tx, &cfg);
   }

   /* 回放：raw[] 来自 ir_receiver，偶数下标=mark、奇数下标=space */
   (void)ir_transmitter_send(&s_tx, raw, len);
   ```

## `use_carrier` 与 `inverted` 怎么选

| 现象 | 结论 |
|---|---|
| 待机时 IR 管/状态灯常亮 | 模块是**低有效**，`inverted = true` |
| mark 有波形但空调不响应、接收头只闪一下 | 本端发了无调制直流红外 → 模块自带 38kHz，改 `use_carrier = false`（mark 只给有效电平，交模块内部调制） |
| 状态灯闪但 IR 管不发光 | 多半是驱动电流不足（限流电阻过大），不是代码问题 |

`use_carrier = false` 时 mark 输出的是**有效电平**（由 `inverted` 决定高/低），
不是固定高 —— 与外部模块的接法保持一致即可。

## 单段时长的取值

- `uint16_t`，单位 µs；`timings[i]` 中 **i 为偶数 = mark**，奇数 = space。
- 帧间间隔（例如重复码之间保持空号 40ms）用 `ir_transmitter_space(&s_tx, 40000U)`
  或由任务层控制重复次数，驱动本身**只负责一帧**（单一职责）。
