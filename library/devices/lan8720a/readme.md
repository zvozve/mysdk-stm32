# 功能说明

LAN8720A 是板载以太网 PHY（物理层收发器），坐 MCU ETH 外设之上。

- **LwIP 协议栈本身不进本 SDK**：属 CubeMX Middlewares，由工程侧提供（`external:lwip`）。
  本仓库只提供 PHY 的**硬件复位 glue** 与板级绑定，定位同 `Drivers/eth_phy/`。
- 驱动分层：

| 文件 | 层 | 职责 |
| :--- | :--- | :--- |
| `lan8720a_reset.h` | devices | PHY 复位公共 API 声明 |
| `lan8720a_reset.c` | devices | 复位引脚拉低/拉高时序（走 `oop_gpio` / `oop_DelayMS`，零直调 HAL） |

- SDK 提供两个 API（引脚由工程 `board_cfg` 注入，SDK 内部零绑定）：

```c
void ETH_RST_Init(GPIO_TypeDef *port, uint16_t pin);  /* 初始化复位引脚并立即执行一次复位 */
void ETH_RST_Execute(void);                            /* 仅执行复位时序（引脚已 Init 后调用） */
```

复位时序（已在 `ETH_RST_Execute` 内实现，无需工程侧再写）：

```
拉低 (nRST) ──等待 ≥50ms──▶ 拉高 ──等待 50ms 稳定──▶ 结束
```


---

# 一、CubeMX 必须开启 / 配置项（避坑重点）

| # | 配置项 | 位置（CubeMX） | 必须值 | 不配的后果 |
| :-- | :--- | :--- | :--- | :--- |
| 1 | 启用 LWIP | `Middleware → LWIP` | Enabled | 无协议栈，无法联网 |
| 2 | **启用 DNS** | `LWIP → Key Options → General Settings → LWIP_DNS` | **Enabled** | `dns_gethostbyname()` / `netconn_gethostbyname()` 返回「not compiled in」，按域名解析（MQTT Broker 域名、NTP 等）直接失败 |
| 3 | **RTOS 任务栈 512 × 4** | `RTOS（CMSIS-V2）→ Tasks → 运行 LwIP 的任务`（tcpip_thread / 调用 `MX_LWIP_Process()` 的任务）`Stack Size` | **512 × 4（= 2048，单位 words）** | 默认栈偏小 → LwIP 内部深度调用（特别是带 TLS/DNS 时）触发 **HardFault 或任务卡死、网络无响应** |
| 4 | **使用 MicroLIB** | `Project Manager → 工程 Options（Keil/ARMCC）→ Target → Use MicroLIB` | **勾选** | 不勾选时 C 运行时不自动初始化 / `printf` 等走 semihosting 死锁，**程序不自动运行** |
| 5 | 25MHz 时钟源 | `RCC / ETH` 配置 PHY 时钟为外部 25MHz 晶振或晶振输入 | 提供 25MHz | PHY 无参考时钟，完全不工作（见第三节「晶振波形」） |

> ⚠️ 第 3 点「512 × 4」是 LwIP 处理线程的最小栈经验值（以 words 计，×4 字节）；若工程还跑了
> MQTT/TLS 等重负载，应在此基准上继续加大，而不是调小。

---

# 二、PHY 复位引脚绑定（工程侧 board_cfg）

`lan8720a` 不引用任何 CubeMX 全局符号，复位引脚由工程 `board_cfg.h` 注入（遵循 SDK「唯一绑定点」约定）：

```c
/* board_cfg.h —— 绑定区 */
#include "main.h"            /* CubeMX 生成的 mxconstants.h 提供 ETH_RST_GPIO_Port / ETH_RST_Pin */
#define BOARD_ETH_RST_PORT   ETH_RST_GPIO_Port
#define BOARD_ETH_RST_PIN    ETH_RST_Pin
```

```c
/* Tasks 的 HW 初始化阶段，务必在 MX_LWIP_Init() / netif 建立之前调用 */
#include "lan8720a_reset.h"

void Task_HW_Init(void)
{
    /* ... 其他外设初始化 ... */
    ETH_RST_Init(BOARD_ETH_RST_PORT, BOARD_ETH_RST_PIN);  /* 内部拉低 50ms → 拉高 */
    /* 之后再做 LwIP / 网络初始化 */
}
```

- 复位引脚默认为**输出、上电高电平**（由 `oop_gpio_init_output(..., true)` 保证）。
- 若 HW 初始化阶段已调过一次 `ETH_RST_Init`，后续热复位只需 `ETH_RST_Execute()`。

---

# 三、硬件上电检查清单（板子完全没反应 / 不通先量这几项）

起不来先按这个顺序量，80% 的「LwIP 不工作」是硬件电源/时钟问题，不是软件：

| 检查项 | 引脚 | 正常表现 | 异常后果 |
| :--- | :--- | :--- | :--- |
| **9 脚电源（VDDCR）** | LAN8720A **Pin 9** | 量到 **≈1.2V**（内部 1.2V 稳压输出，必须外接 **1µF 退耦电容到地**） | 9 脚无 1.2V / 为 0 → 内部稳压没起来或退耦电容丢失 → **PHY 完全不工作**，软件怎么改都白搭 |
| 3.3V 电源（VDDIO） | Pin 1 / 13 / 14 / 22 | ≈3.3V | I/O 域无电，寄存器读不到、link 起不来 |
| **晶振波形** | XI / XO（Pin 2 / 3） | 示波器看到 **≈25MHz 正弦**；晶振两端各接负载电容（按手册 ~18–22pF） | 无振荡 → PHY 无参考时钟 → 无 link、无 50MHz REF_CLK |
| **LED 灯** | nLED1 / nLED2（Pin 20 / 21） | link 时常亮或慢闪；有流量时快闪 | 一直不亮 → PHY 未初始化 / 未建立 link / 无时钟；是判断 PHY 是否存活最直观的灯 |
| 复位引脚（nRST） | Pin 11 | 上电及复位完成后应为**高电平** | 被意外拉低 → PHY 持续保持复位态，永远不工作 |
| REF_CLK（给 MCU） | Pin 24 | REF_CLK OUT 模式下量到 **50MHz** 方波送入 MCU ETH | 无 50MHz → MCU 侧收不到 PHY 时钟，ETH 外设起不来 |

> 顺序建议：**先量 9 脚 1.2V 与 3.3V → 再看晶振 25MHz → 再看 LED / REF_CLK**。电源和时钟
> 有一项不对，软件侧（DNS、任务栈、复位时序）怎么配都救不回来。

---

# 四、常见坑速查

| 现象 | 最可能原因 | 对应处理 |
| :--- | :--- | :--- |
| 按域名连不上服务器，IP 直连 OK | **DNS 未启用**（第 1 节 #2） | CubeMX 开 `LWIP_DNS=Enabled` |
| 联网后随机死机 / 进 HardFault | LwIP 任务栈太小（第 1 节 #3） | 任务栈设 **512×4** 起步，重负载再加 |
| 烧录后程序不跑 / 卡死不进 main | **未用 MicroLIB**（第 1 节 #4，Keil/ARMCC） | 勾选 `Use MicroLIB`，或自行实现 `_sbrk/_write` 等 syscalls |
| PHY 完全无反应、LED 不亮、量不到时钟 | 9 脚 1.2V / 晶振 / 上电时序（第三节） | 按硬件清单逐项量 |
| 网口灯闪但 MCU 侧收不到包 | 复位时序太短 / REF_CLK 未输出 | 复位低电平 ≥50ms（SDK 已封装），确认 Pin24 出 50MHz |
| 编译报 `mqtt` 符号重定义 | **LwIP 自带 mqtt 与本 SDK `protocols.mqtt` 冲突** | EXCLUDE 掉 `LwIP/apps/mqtt/mqtt.c`（见仓库 readme 顶层「LwIP 自带 mqtt 冲突」） |

---

# 五、驱动版本

---
## V1.0
	LAN8720A PHY 硬件复位驱动初版（oop 分层，复位引脚由 `ETH_RST_Init()` 注入，时序低 50ms → 高）。
	配套 LwIP 接入避坑文档（CubeMX DNS / RTOS 任务栈 512×4 / MicroLIB / 9 脚电源 / 晶振 / LED 检查清单）。
