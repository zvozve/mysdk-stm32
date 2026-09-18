# ir_receiver — 红外接收驱动（TIM 输入捕获）

解调输出型一体化接收头（VS1838B / HX1838 / HS0038 类）的板无关驱动。
接收头输出的是**已解调**信号（空闲高、载波在场拉低），驱动只做一件事：
测出相邻边沿之间的时长序列 —— 与具体协议无关，协议解析留给上层。

- **层**：`library/devices/ir_receiver`
- **版本**：V2.0（2026-09-18 由「GPIO EXTI + DWT 软件计时」改为 **TIM 输入捕获**）
- **依赖**：`chip.oop_tim`（TIM 原语）、`chip.platform`

## 为什么改成输入捕获

| | V1.x（EXTI + DWT） | V2.0（输入捕获） |
|---|---|---|
| 时间戳来源 | 中断里读 DWT CYCCNT | 定时器硬件锁存到 CCR |
| 抖动 | 取决于中断响应延迟（±1~3µs） | 0（硬件在跳变时刻锁存） |
| 计时可靠度 | 依赖 DWT，**部分 F1 板实测 CYCCNT 会冻结** | 只依赖 TIM 计数，与 MCU 型号无关 |
| 引脚限制 | 任意 GPIO | 必须复用到捕获定时器的通道引脚 |
| 帧结束 | 需一个 1ms 定时器中断 | 主循环 `ir_receiver_process()` 轮询，不额外占定时器 |

用「必须落在 TIM CH 引脚」换「零抖动 + 不依赖 DWT」——这是工程侧实测后的选择。

## 接入步骤

1. **CubeMX**：一路通用定时器配成 **Input Capture direct mode**，通道接到接收头 OUT 引脚；
   计数频率建议 1MHz（`PSC = TIM_CLK/1e6 - 1`），`ARR` 取满量程（16 位定时器填 `0xffff`）；
   使能该定时器的**全局中断**。
2. **board_cfg.h**：注入句柄（语义名 → MX 名，这是全工程唯一能提 MX 名的地方）
   ```c
   #define BOARD_IR_RX_TIM          (&htim2)
   #define BOARD_IR_RX_TIM_CHANNEL  TIM_CHANNEL_2
   #define BOARD_IR_RX_TIM_CLK_HZ   1000000UL
   ```
3. **用户代码**：提供缓冲区、填 cfg、转接 HAL 回调
   ```c
   static uint16_t  s_buf[320];
   static ir_receiver_t s_rx;

   void ir_receiver_task_init(void)
   {
       ir_receiver_cfg_t cfg = {
           .htim        = BOARD_IR_RX_TIM,
           .channel     = BOARD_IR_RX_TIM_CHANNEL,
           .clk_hz      = BOARD_IR_RX_TIM_CLK_HZ,
           .buf         = s_buf,
           .cap         = 320U,
           .idle_us     = 15000U,   /* 0 = 用默认值；内部会夹到「模数的一半」 */
           .debounce_us = 0U,       /* 0 = 默认 200us */
       };
       (void)ir_receiver_init(&s_rx, &cfg);
       (void)ir_receiver_start(&s_rx);
   }

   /* 中断转接（工程侧负责，驱动不接管 HAL 回调） */
   void HAL_TIM_IC_CaptureCallback(TIM_HandleTypeDef *htim)
   {
       if (htim == BOARD_IR_RX_TIM) {
           ir_receiver_isr(&s_rx);
       }
   }

   /* 主循环 / 任务里周期调用 */
   void ir_receiver_task_poll(void)
   {
       if (ir_receiver_process(&s_rx)) {                    /* true = 收到一帧 */
           const uint16_t *raw = ir_receiver_data(&s_rx);
           uint16_t        len = ir_receiver_count(&s_rx);
           /* ... 使用 raw[0..len) ... */
           (void)ir_receiver_start(&s_rx);                  /* 取完数据再重启采集 */
       }
   }
   ```

## 数据约定

- `raw[]` 偶数下标 = **mark**（有载波），奇数下标 = **space**；
  `raw[0]` 是引导 mark。与 `ir_transmitter_send()` 的回放约定一致。
- 单位 µs，`uint16_t`（单段最长 65535µs）。
- 首个边沿只做时基基准、不产生条目 —— 所以驱动用**下降沿**起步
  （接收头空闲为高，引导 mark 是「高→低」）。

## 必须知道的三个硬约束

1. **计数器回绕**：F1 全系定时器都是 16 位，16bit@1MHz 每 65.5ms 回绕一次。
   驱动内所有时间差都按模数（`ARR+1`）做环形减法，16/32 位通吃 —— 不要自己
   在应用里裸减 `now - last`。
2. **静默上限 ≈ 模数的一半**（16bit@1MHz 约 32ms）。`idle_us` 填再大也会被夹到
   这个上限。要跨越更长的段间间隔（例如一个按键分两段、中间隔 100ms），
   应在任务层做「多段序列 + 段间 ms 间隔」记录，而不是抬 `idle_us`。
3. **去抖死区 200µs**：近距离强信号会把接收头 AGC 灌饱和，解调输出跟随 38kHz
   产生 26µs 周期的密集毛刺边沿（会把缓冲撑爆）。真实 mark/space 最短约 560µs，
   故用 200µs 死区滤掉亚载波级尖刺。间距小于死区的边沿直接丢弃。

## 状态机

```
IDLE --start()--> BUSY --process()判静默--> DONE --取数据 + start()--> BUSY
                   |
                   +-- 缓冲写满 --> OVERFLOW --start()--> BUSY
```

- `ir_receiver_process()` **只在 BUSY 下工作**，收到一帧后返回一次 `true`，
  捕获自动停止（防止半截数据继续写入）；取完数据必须再 `ir_receiver_start()`。
- 因此「一帧只上报一次」由状态机保证，调用方无需额外去重。
- 发码前若已把接收关掉，可用 `ir_receiver_stop()`，发完再 `ir_receiver_start()`，
  避免自发自收。

## 与发射侧配合

`ir_receiver`（收）+ `ir_transmitter`（发）合起来就是「学习 + 回放」：

```c
/* 发码期间关接收，避免收到自己发出的码 */
ir_receiver_stop(&s_rx);
ir_transmitter_send(&s_tx, raw, len);
(void)ir_receiver_start(&s_rx);
```
