# 功能说明

CH374 / CH374T SPI 接口 USB Host 控制器驱动（自研，非 ST 官方库）。

- 支持 ROOT-HUB 三端口，键盘 / 扫码枪（Boot Keyboard）与鼠标（含 12bit 高精度）识别；
- 芯片寄存器级时序全部在 `ch374.c`（SPI 命令 + 地址 + 数据三态时序）；
- 板级硬件访问（片选 / 中断 / USB 供电 / SPI 字节收发）全部在 `ch374_port.c`。

驱动分层：

| 文件 | 层 | 职责 |
| :--- | :--- | :--- |
| `ch374inc.h` | devices | 芯片寄存器、位域、USB 协议描述符定义（厂商原版） |
| `ch374_sys.h` | devices | 基础短类型名（u8/u16/u32 …） |
| `ch374.h` | devices | 驱动公共 API + 内部函数声明 |
| `ch374.c` | devices | 芯片寄存器级驱动（枚举 / HID / U盘 BulkOnly） |
| `ch374_port.h` / `ch374_port.c` | devices | 板级适配：SPI 句柄与 CS/INT/PWR 引脚注入 |

板级绑定方式（工程侧）：

```c
CH374_Bind(&hspi2,
           CH374_CS_GPIO_Port,  CH374_CS_Pin,
           CH374_INT_GPIO_Port, CH374_INT_Pin,
           USB_PWR_GPIO_Port,   USB_PWR_Pin);
```

# CubeMX修改内容

| 项目 | 值 |
| :--- | :-- |
| SPI2 | 硬件 SPI，Mode = Full-Duplex Master，CPOL=High / CPHA=2Edge（mode3），MSB First |
| CH374_CS | GPIO:Output Push Pull，上电默认高电平 |
| CH374_INT | GPIO:Input，Pull-up |
| USB_PWR | GPIO:Output Push Pull，上电默认高电平 |

# 生成后需手动修改内容

| 文件 | 修改内容 | CubeMX 重新生成后 |
| :--- | :--- | :--- |
| `Core/Src/main.c` | 在 `USER CODE BEGIN 2` 内调用 `CH374_Bind(...)` 注入 SPI 句柄与引脚 | **需手动恢复** |
| `Core/Src/main.c` | `HostMode_Init()` 的 DWT 初始化 `bsp_InitDWT()` 已由 SDK 内部改为 `oop_InitDWT()`，工程侧无需再调 `bsp_InitDWT()` | 不受影响 |
| — | 原 `GPIO_Toggle_INIT()`（配置 CH374_CS / CH374_INT / USB_PWR 三个引脚）已并入 SDK 的 `CH374_Bind()`，工程内不要再保留该函数 | **需手动删除**（否则与 SDK 重复初始化引脚） |

# 驱动版本

---
## V2.0
	2026-09-17
	按 oop 分层迁移进 mysdk-stm32：删 bsp_*/main.h 依赖，SPI 改走 oop_spi（多实例，
	与 CH9434 的 hspi3 并存）、CS/INT/USB 电源走 oop_gpio、延时走 oop_dwt；
	引脚与句柄由 CH374_Bind() 注入；原 GPIO_Toggle_INIT() 并入 CH374_Bind()；
	调试输出统一改投 SEGGER_RTT（不再依赖 libc printf 重定向）。

---
## V1.0
	CH374 主机模式驱动初版（含 HID 键盘/鼠标与 U 盘 BulkOnly）。
