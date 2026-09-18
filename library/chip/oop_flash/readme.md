# chip.oop_flash — 内部 Flash OOP 驱动

## 定位

`HAL_FLASH_*` 的**唯一入口**（分层铁律：只有 chip 层可以直接用 HAL）。
内部 Flash 不需要注入句柄，本层直接坐 HAL 之上；「哪一段属于谁、能不能擦」由上层决定
（`services.ota_core` + 工程侧 `User/ota_areas.c`）。

- 依赖：`chip.platform`
- 不打印日志（保持 chip 层零 SEGGER 依赖），只返回错误码
- 板无关：不引用任何引脚 / 句柄 / MX 宏

## 已支持系列

| 系列 | 几何 | 擦除单位 | 写入单位 |
|---|---|---|---|
| **F4**（单 Bank：F405/407/415/417/401/410/411/412/413/423/446） | 扇区 0~3 = 16 KB、4 = 64 KB、5~11 = 128 KB；**实际扇区数由 FLASH_SIZE 寄存器截断**（512 KB 器件 = 前 8 个扇区） | 扇区（**一次一个**，绝不跨 Bank 批量擦） | WORD（4 B） |
| **F1**（F100~F107） | 等长页：`≤128 KB` 器件 **1 KB/页**、`>128 KB` 器件 **2 KB/页** | 页（给页**地址**） | HALFWORD（2 B） |
| **G4**（G431~G484） | 等长页 **2 KB**，每 Bank 256 KB，Bank2 起址 `FLASH_BASE + 0x40000` | 页（按 Bank 分别给页号） | DOUBLEWORD（8 B） |

编译验证：三个系列各编过一次，`-Wall -Wextra -Wpedantic` **零警告**（arm-gcc 14.2）。

### ⚠ 三条限制（用前必读）

1. **G4 分支未上板验证**。SDK 目前没有 G4 的 OTA 用例，G4 代码是照 HAL 结构写的、编得过但没跑过。
   首次在 G4 上使用前：先用 `oop_flash_info()` 打印几何值，再用一个 scratch 页做「擦 → 写 → 读回」。
2. **双 Bank F4（F427/F437/F429/F439/F469/F479）编译期直接 `#error`**。
   ST 的 HAL 在 `FLASH_Erase_Sector()` 里对扇号 > `FLASH_SECTOR_11` 有一个
   「SNB 偏移 +4」的特殊规则，本驱动没有实现该映射。与其静默算错地址去擦错扇区，
   宁可拦住 —— 需要双 Bank 时请先补那段映射再放开。
3. **F1 页大小是按容量推导的**，不读 `FLASH_PAGE_SIZE` 宏（避免不同 HAL 版本宏缺失）。
   推导规则与 ST 的定义一致；若遇到非标器件，`oop_flash_sector_at()` 能立刻暴露异常。

另外：F1/F4/G4 都**没有 D-Cache**，所以「写后直接读回」天然一致，本驱动不做 cache 维护。
将来加 F7/H7 时必须补 `SCB_CleanDCache_by_Addr` / `InvalidateDCache_by_Addr`。

## 用法

### 基本读写擦

```c
#include "oop_flash_drv.h"

const oop_flash_info_t *fi = oop_flash_info();
SYS_LOG("flash: base=0x%08lX size=%luK sectors=%lu gran=%luB uniform=%d",
        (unsigned long)fi->base, (unsigned long)(fi->size / 1024u),
        (unsigned long)fi->sector_count, (unsigned long)fi->write_gran, (int)fi->uniform);

oop_flash_erase(SLOT_A_BASE, SLOT_A_SIZE);          /* 自动按扇区展开，无需自己查粒度 */
oop_flash_write(SLOT_A_BASE, buf, len);             /* 首尾非对齐由内部读-改-写补齐 */
oop_flash_verify(SLOT_A_BASE, buf, len, &bad);      /* 小段定位用；整段完整性请用 CRC32 */
```

### 安全闸：别把 Bootloader 自己擦掉

```c
/* BL 侧：只允许擦写「除自己以外」的区域，area 表算错时也伤不到自己 */
oop_flash_set_guard(BOOT_REGION_CFG_BASE, BOOT_FLASH_END);

oop_flash_erase(0x08000000u, 4096u);                /* 落在窗口外 -> OOP_FLASH_ERR_RANGE */
oop_flash_set_guard(0, 0);                          /* 解除限制 */
```

闸门校验用的是**展开后的完整扇区范围**：即使 `[addr, addr+len)` 落在窗口内，
只要它所在的扇区横跨窗口外，整个擦除也会被拒绝 —— 避免把窗口外的内容连带擦掉。

## API

| 函数 | 说明 |
|---|---|
| `oop_flash_info()` | 整片几何（惰性探测并缓存） |
| `oop_flash_sector_at(addr, &sec)` | 查地址所属扇区/页 |
| `oop_flash_sector_size(addr)` | 该地址所在扇区/页的容量 |
| `oop_flash_in_range(addr, len)` | 是否落在内部 Flash 内 |
| `oop_flash_set_guard(begin, end)` | 收紧允许擦写的窗口（`0,0` 解除） |
| `oop_flash_read / erase / write` | 读 / 擦（自动展开） / 写（自动补对齐） |
| `oop_flash_verify(addr, buf, len, &bad)` | 逐字节读回比对 |
| `oop_flash_is_erased(addr, len)` | 是否全 `0xFF` |
| `oop_flash_unlock / lock` | 显式解锁/加锁（擦写内部已自动配对，通常不用） |

返回码：`OOP_FLASH_OK` / `ERR_PARAM` / `ERR_RANGE` / `ERR_ALIGN` / `ERR_ERASE` / `ERR_WRITE` / `ERR_MISMATCH`。

## 坑位

| 现象 | 原因 |
|---|---|
| 写入后部分字节没变 | 目标未擦除。Flash 只能 1→0，**必须先擦** |
| 读-改-写把别的字节冲掉了 | `oop_flash_write` 的首尾补齐会重写整个写入单位。同一单位内其它字节必须已是最终值（OTA 场景「整区先擦、顺序写」天然满足） |
| 擦除范围比预期大 | `oop_flash_erase` 会向两侧扩展到扇区边界。要精确控制就自己把地址对齐到扇区边界 |
| F4 上按 1024 K 算出 12 个扇区，但板子是 512 KB | 容量来自 `FLASH_SIZE` 寄存器，实际会算成 8 个扇区；若读到 0（寄存器异常）会按 512 KB 兜底 |
| 擦写期间中断被延迟 | 正常现象：Flash 擦写会停住取指总线，CPU 与中断一起被挂住。界面/通信任务要容忍几十 ms 的延迟 |
| `region FLASH overflowed` | 与本模块无关，是镜像超过槽容量 |
