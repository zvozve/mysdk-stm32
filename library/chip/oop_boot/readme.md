# chip.oop_boot —— 启动跳转（V1.0）

> 把控制权从 Bootloader 交给 APP 的那一步：**向量表校验 → 外设复位 → VTOR → MSP → 跳**。

## 为什么在 chip 层

跳转是一组**芯片内核操作**（VTOR / MSP / NVIC / SysTick）外加 `HAL_DeInit()`。
HAL 红线规定只有 `chip/` 允许出现 HAL 符号，所以 `services.bootloader`（BL 外壳）
只负责「决定跳到哪、凭什么跳」，最后把控制权交给本模块。

## API

| 函数 | 作用 |
|---|---|
| `oop_boot_vector_check(addr, &sp, &pc)` | 读向量表首两字并判合法性（不跳，供上层先判断） |
| `oop_boot_set_vtor(addr)` | 设 VTOR + `__DSB/__ISB`（APP 早期设自己的向量表也用它） |
| `oop_boot_get_vtor()` | 读回 VTOR（诊断） |
| `oop_boot_periph_deinit()` | 关中断 + 停 SysTick + 清 NVIC + `HAL_DeInit()`；幂等 |
| `oop_boot_jump(addr)` | 终局。**成功不返回**；向量表非法时返回错误码 |

## 跳转前必须做的四件事（顺序不能变）

1. **关全局中断** —— `__disable_irq()`
2. **停 SysTick** —— 注意 `HAL_SuspendTick()` **只关它的中断，计数器还在跑**。要彻底停：
   `CTRL = LOAD = VAL = 0`
3. **清 NVIC 使能与挂起** —— CMSIS 里**没有** `NVIC_ClearAllPendingIRQ()` 这个函数
   （它在某些厂商封装里才存在），只能按寄存器数组循环写 `ICER[]` / `ICPR[]`
4. **`HAL_DeInit()`** —— 外设寄存器复位到已知态

之后才是：校验向量表 → 设 VTOR → 设 MSP → 开中断 → `bx`。

## 两个不能颠倒的顺序

**① SP 合法性检查必须在 `__set_MSP()` 之前做完。**
MSP 一换，栈上所有局部变量（含返回地址）全部失效，此后连普通 C 函数都不能调 ——
在换栈之后再调用「检查函数」，检查函数自己的栈帧就已经是垃圾了。
本模块把跳转实现为 **naked 汇编函数**（`oop_boot_jump_asm`），函数体只有
`msr msp` / `dsb` / `isb` / `cpsie i` / `bx`，中间不碰栈。

**② 开中断要放在换栈之后、`bx` 之前。**
之前开的话，NVIC 虽然清了，但 SysTick 之类的内核异常仍可能在换栈中途打进原固件的
处理函数；之后开则目标固件已经拥有自己的向量表。中间窗口是安全的，因为 NVIC 已全清。

## `oop_boot_vector_check` 的判据

保守但零成本（只读 8 字节）：

- `vector_addr` 4 字节对齐
- `SP` 落在 SRAM 区 `[0x20000000, 0x40000000)` 且 4 字节对齐
- `PC` 非 0、Thumb 位（bit0）为 1

这能把三类情况挡在跳转之前：**空区**（`0xFF...` → SP 越界）、**未下载**、**写坏的槽**。

> 栈放在 CCM / DTCM 等非 `0x20000000` 区的板子，用
> `-DOOP_BOOT_SRAM_BASE=...` / `-DOOP_BOOT_SRAM_END=...` 覆盖范围。

想要更严格的「PC 必须落在本槽范围内」，由调用方拿分区表自己比对 ——
本模块不知道分区表，也不需要知道。

## 限制

- 只验证过 **F1 / F4 / G4**，其它系列在编译期 `#error`。
- 不处理 D-Cache（F1/F4/G4 无 D-Cache）；将来加 F7/H7 时，
  跳转前必须补 `SCB_CleanDCache` / 相应的 MPU 配置。

## 依赖

`chip.platform`（唯一）。
