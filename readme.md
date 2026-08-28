# mystm32-sdk

本仓库用于存放自建 STM32 SDK。

包含以下内容：
- 片上寄存器 OOP 封装（基于 HAL） —— `chip/`
- 板外设备驱动 —— `devices/`
- 协议封装 —— `protocols/`
- 第三方中间件 —— `middleware/`

## 版本

- SDK 版本：**0.1.0**（初始版本，2026-08-28）
- Manifest schema：**1.0**（`sdk_manifest.json`）
- 支持的 MCU 系列：STM32G4、STM32F4（由 `chip/platform/Inc/hal_platform.h` 按编译宏自动展开）

## 目录结构

```
mystm32-sdk/
├── chip/            # MCU 内部外设 OOP 封装（板无关，基于 HAL）
│   ├── bsp_dwt/     # DWT 延时/计时
│   ├── bsp_gpio/    # GPIO 驱动（drv 抽象 + drv_hal 后端）
│   ├── bsp_uart/    # 串口 DMA 驱动 + 状态机
│   └── platform/    # hal_platform.h 移植层（系列统一入口）
├── devices/         # 板载外挂芯片驱动（坐 chip/ 总线）
│   ├── dht11/       # 温湿度
│   ├── heart_beat/  # 心跳 LED
│   ├── hlk_rm58s/   # WiFi 模块（AT）
│   ├── ir_1838b/    # 红外接收
│   ├── ir_tx/       # 红外发射
│   └── lan8720a/    # 以太网 PHY 复位
├── protocols/       # 协议 / 算法库
│   ├── ac_codec/    # 空调码编解码
│   ├── cJSON/       # JSON 解析（第三方）
│   ├── mqtt/        # MQTT 客户端
│   └── wol/         # Wake-on-LAN（依赖 lwIP）
├── middleware/      # 第三方调试/传输库
│   └── SEGGER_RTT/  # 实时日志
├── sdk_manifest.json  # SDK 自描述（模块/依赖/版本/文件清单）
└── readme.md
```

## 设计约定

- **分层不按物理位置混淆**：`chip/`=MCU 内，`devices/`=板外，`protocols/`=协议，`middleware/`=第三方。弃用 `Core/bsp/Drivers` 笼统或越权名。
- **HAL 保留**：CubeMX 生成的 HAL 仍是 vendor 底层，OOP 封装包在它上面；明确否决「全去 HAL 自写」。
- **单源真相**：本仓库是本地共享 SDK（镜像 Zephyr 的 `zephyrproject`），bug 改一处。项目通过 `sync_lib.py` 拷贝选中子集进 `Lib/`，不引用本仓库路径。
- **版本号规则**：SDK 顶层用语义化版本（如 `0.1.0`）；模块 `version` 沿用源码 `@version` 标注（Vx.y 或第三方原生版本），无标注者记为 null。

## 版本变更记录

### v0.1.0 (2026-08-28) — 初始版本

- 建立四层结构 `chip/ devices/ protocols/ middleware/`，确定层命名与 `hal_platform` 移植层。
- 收录模块（含各自版本）：
  - chip：bsp_dwt (V2.0)、bsp_gpio (V3.1)、bsp_uart (V3.0)、hal_platform (V1.0)
  - devices：dht11 (V3.1)、heart_beat (无标注)、hlk_rm58s (无标注)、ir_1838b (V1.2)、ir_tx (V1.0)、lan8720a (无标注)
  - protocols：ac_codec (V1.0)、cJSON (1.7.18)、mqtt (无标注)、wol (无标注)
  - middleware：SEGGER_RTT (第三方，无标注)
- 新增 `sdk_manifest.json`（schema 1.0）：描述 families / layers / modules（depends、backends、files、version），供 `sync_lib.py` 与未来 GUI 读取。
- 已知待清理：`middleware/cJSON` 为空占位目录，真实 cJSON 已落在 `protocols/cJSON`，后续删除空占位。
