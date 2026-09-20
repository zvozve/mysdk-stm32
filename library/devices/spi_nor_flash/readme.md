# devices.spi_nor_flash — 通用 SPI NOR Flash 驱动

## 定位

**板外器件**（devices 层），覆盖 W25Q / GD25Q / BY25Q / EON 等同族单线 SPI NOR。
坐 `chip.oop_spi` 之上：SPI 走 `oop_spi_*`、CS/WP 走 `chip.oop_gpio`、计时走 `chip.oop_dwt`，
**不直调任何 `HAL_SPI_*` / `HAL_GPIO_*`**（`sdk-check-oop --strict` 会拦）。

- 依赖：`chip.oop_spi`、`chip.oop_gpio`、`chip.oop_dwt`
- 不打印日志，只返回错误码
- 多实例：两片 NOR 各持一个 `spi_nor_dev_t`，挂不同 SPI 外设互不干扰
- 时钟前提：`oop_GetTickMS()` 依赖 SysTick（`HAL_Init()` 已建）；`oop_DelayUS()` 用于唤醒后的 tRES1 等待，需要工程先 `oop_InitDWT()`

## 为什么是「阻塞 + 空闲回调」而不是异步状态机

SPI NOR 单次单位操作本身很短：页写 ~0.7 ms、4 KB 扇区擦 ~45 ms、64 KB 块擦 ~150 ms。
一个完整 OTA 镜像要写上千页 —— 真正的问题不是「单次阻塞」，而是「一次调用阻塞太久」。

所以本驱动把粒度做成 **单位操作阻塞 + 空闲回调**：

```c
typedef bool (*spi_nor_idle_cb_t)(void *user);   /* 返回 false 请求中止 */
spi_nor_set_idle_cb(&s_nor, my_idle, NULL);
```

每个 WIP 等待循环里都会反复调用它 —— 喂看门狗、刷进度、响应取消都在这里做，
不必为一块几十行的器件驱动引入整套异步状态机。上层想控制一次阻塞多久，
就把 `spi_nor_write()` 的 `len` 切小（建议 ≤ 4 KB/次）。

## 用法

```c
#include "spi_nor_flash.h"

static oop_spi_dev_t  s_spi;
static gpio_dev_t     s_cs;
static spi_nor_dev_t  s_nor;

static bool ota_idle(void *user) { (void)user; heart_beat_run(); return true; }

void task_extflash_init(void)
{
    /* 1) SPI 与 CS 由工程侧配好（引脚真相在 .ioc / board_cfg.h） */
    oop_spi_dev_init(&s_spi, BOARD_SPI_FLASH);                 /* 如 &hspi3 */
    oop_gpio_init_output(&s_cs, BOARD_FLASH_CS_PORT,
                         BOARD_FLASH_CS_PIN, false);           /* CS 低有效 -> false */

    /* 2) 探测：注入总线与片选，读 JEDEC ID 定容量 */
    spi_nor_cfg_t cfg = {
        .spi        = &s_spi,
        .cs         = &s_cs,
        .wp         = NULL,
        .size       = 0,            /* 0 = 由 JEDEC 容量码推导 */
        .fast_read  = true,         /* 0x0B + 8 dummy，推荐 */
        .addr_bytes = 3,            /* <=16 MB；>16 MB 填 4 */
        .timeout_ms = 2000,
    };
    if (spi_nor_create(&s_nor, &cfg) != SPI_NOR_OK) {
        ERR_LOG("ext flash probe failed");
        return;
    }
    SYS_LOG("ext flash: mfr=0x%02X type=0x%02X cap=0x%02X size=%luK",
            s_nor.id.mfr, s_nor.id.mem_type, s_nor.id.capacity_code,
            (unsigned long)(spi_nor_size(&s_nor) / 1024u));

    spi_nor_set_idle_cb(&s_nor, ota_idle, NULL);
}

void task_extflash_write_ota(const uint8_t *buf, uint32_t len)
{
    spi_nor_erase(&s_nor, STAGE_BASE, len);        /* 自动选块擦/扇区擦 */
    for (uint32_t off = 0; off < len; off += 4096u) {   /* 分块：每块之间会返回主循环 */
        uint32_t c = (len - off > 4096u) ? 4096u : (len - off);
        if (spi_nor_write(&s_nor, STAGE_BASE + off, buf + off, c) != SPI_NOR_OK) {
            ERR_LOG("stage write fail @%lu", (unsigned long)off);
            return;
        }
    }
}
```

## API

| 函数 | 说明 |
|---|---|
| `spi_nor_create(dev, cfg)` | 注入总线/片选 + 读 JEDEC ID + 定容量（会先发 0xAB 唤醒） |
| `spi_nor_read_id(dev, &id)` | 重读 ID（自检用）；`create` 之前也可调用 |
| `spi_nor_set_idle_cb(dev, cb, user)` | 注册空闲回调（喂狗/进度/取消） |
| `spi_nor_size` / `spi_nor_read_status` / `spi_nor_is_busy` | 容量 / 状态寄存器 1 / WIP 忙 |
| `spi_nor_read(dev, addr, buf, len)` | 顺序读，长度无上限（内部 4 KB 分块） |
| `spi_nor_write(dev, addr, buf, len)` | 页编程，内部自动按页切分（不跨页），每页等 WIP |
| `spi_nor_erase_sector` / `_block` / `_chip` | 4 KB / 64 KB / 整片（整片超时放宽到 5 min） |
| `spi_nor_erase(dev, addr, len)` | 向两侧对齐到单位边界，能整块擦就整块擦 |
| `spi_nor_is_erased` / `spi_nor_verify` | 读回判断「全 0xFF」 / 读回逐字节比对 |
| `spi_nor_write_enable` / `spi_nor_wait_ready` | 写使能（含 WEL 校验）/ 等 WIP |
| `spi_nor_deep_power_down` / `_release_power_down` | 0xB9 / 0xAB |

返回码：`SPI_NOR_OK` / `ERR_PARAM` / `ERR_RANGE` / `ERR_IO` / `ERR_TIMEOUT` / `ERR_ABORT` / `ERR_WEL` / `ERR_MISMATCH`。

## 工程侧需要准备什么

1. **CubeMX**：SPI 配成全双工主机（单线，模式 0 或 3 均可，NOR 都支持）；再配 CS 与（可选）WP 两个 GPIO 输出。
   - 注意：`mcu/app` 与 `mcu/bootloader` 目前**都没有启用 SPI 外设**，真要用外挂 Flash 得先在 `.ioc` 里加
     （`.ioc` + `Core/Src/*.c` + `Core/Inc/*.h` + `main.c` 的 `MX_SPIx_Init()` 四处同步改）。
2. **board_cfg.h**：把 SPI 句柄与 CS/WP 引脚映射成语义名（`BOARD_SPI_FLASH` / `BOARD_FLASH_CS_PORT` / `BOARD_FLASH_CS_PIN`）。
3. **`oop_gpio_init_output(cs, port, pin, false)`**：CS 是低有效，`active_high` 必须传 `false`，
   之后 `oop_gpio_write(cs, true)` 即「选中」。本驱动不会替你配引脚。
4. **`oop_InitDWT()`**：`spi_nor_create()` 里唤醒后要等 tRES1（用 `oop_DelayUS`）。

## 坑位

| 现象 | 原因 / 处理 |
|---|---|
| `spi_nor_create` 返回 `ERR_IO` | 接线/CS 极性/SPI 模式不对。先确认 CS 空闲时是高电平，再确认 MISO/MOSI 没接反 |
| 返回 `ERR_WEL` | 回读 WEL 位没置起。常见于 WP 脚被拉低、或状态寄存器保护位被设过。用 `spi_nor_read_status()` 看 SR1 的保护位 |
| 返回 `ERR_TIMEOUT` | 器件没响应（掉电/虚焊），或 `timeout_ms` 对小容量老器件太短 |
| 写入后读回不对 | 目标未擦除（NOR 只能 1→0）；或写跨页了 —— 本驱动内部按页切分，若你绕过它自己拼时序才会踩到 |
| 读回数据整体偏移 1 字节 | `fast_read` 与器件不匹配：`fast_read=true` 要多发 **1 个 dummy 字节**，本驱动已处理；若器件不支持 0x0B（很老的型号）改 `fast_read=false` |
| 容量探测为 0 导致 `create` 失败 | 非标容量码（超出 `0x10~0x1E`），或 JEDEC ID 读出全 0。在 `cfg.size` 里直接给容量 |
| 整片擦几十秒 | 正常（`tCE` 与容量成正比）。靠 `idle_cb` 喂狗；返回 `false` 可中止 |
| 4 字节地址器件连不上 | `cfg.addr_bytes = 4` 只改「发几个地址字节」，**不会**自动下发 EN4B(0xB7)。器件必须已经处于 4 字节模式（出厂 / 上次设过） |

## 未实现（需要时先补 SDK，别在工程侧另开一份）

- SFDP 探测（容量按 JEDEC 容量码推导 + `cfg.size` 兜底）
- QSPI / 双线四线模式（只用单线 SPI）
- EN4B/EX4B 自动下达、状态寄存器写保护位配置、OTP / 安全寄存器

## 定位：它给谁用

`services.ota_flash_ext` 会把它适配成 `ota_flash_t`，供 `services.ota_core` 把外挂 Flash
当作 STAGE（暂存区）或 BACKUP 用 —— 这是 512 KB 器件「单运行槽 + 外挂暂存 + BL 搬运」
方案的介质基础（见 `ota-demo-stm32/doc/06-架构设计-兼容与可选.md`）。
