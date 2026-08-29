# mystm32-sdk

本仓库用于存放自建 STM32 SDK。

仓库分三层，职责互不混淆：
- `library/` —— 固件源码（按 `sdk.toml` 自动拉取到工程 `MySDK/`），含：
  - 片上寄存器 OOP 封装（基于 HAL） —— `library/chip/`
  - 板外设备驱动 —— `library/devices/`
  - 协议封装 —— `library/protocols/`
  - 第三方中间件 —— `library/middleware/`
- `tools/` —— 跑在开发 PC 上的工具（**单源、只调用不拉取**）：`sync_lib.py` / `oop_audit.py`（SDK 维护）、`flash.bat` / `trans_gbk2utf-8.py`（工程面向）。
- `manual/` —— 手动拷贝到工程的脚手架（`sdk_run.py` 桥接 + `.vscode` 接口模板 + `User/` 配置模板），**不自动拉取**。

## 版本

- SDK 版本：**0.3.0**（2026-08-29）
- Manifest schema：**1.0**（`sdk_manifest.json`）
- 支持的 MCU 系列：STM32G4、STM32F4（由 `chip/platform/Inc/hal_platform.h` 按编译宏自动展开）

## 目录结构

```
mystm32-sdk/
├── library/          # 固件源码：按 sdk.toml 自动拉取 → 工程 MySDK/
│   ├── chip/         # MCU 内部外设 OOP 封装（板无关，基于 HAL）
│   │   ├── oop_dwt/ oop_gpio/ oop_uart/ oop_tim/ oop_iwdg/ platform/
│   ├── devices/      # 板载外挂芯片驱动（坐 chip/ 总线）
│   │   ├── dht11/ heart_beat/ hlk_rm58s/ ir_1838b/ ir_tx/ lan8720a/
│   ├── protocols/    # 协议 / 算法库
│   │   ├── ac_codec/ mqtt/ wol/
│   └── middleware/   # 第三方调试/传输库
│       ├── cJSON/ SEGGER_RTT/
├── tools/            # 全部 host 工具（单源，只调用不拉取）
│   ├── sync_lib.py   # 子集拉取（SDK 维护）
│   ├── oop_audit.py  # HAL 泄漏检查（SDK 维护）
│   ├── flash.bat     # J-Link 烧录（工程面向）
│   └── trans_gbk2utf-8.py  # GBK→UTF-8（工程面向）
├── manual/           # 手动拷贝的脚手架（不自动拉取）
│   ├── sdk_run.py    # 工程侧桥接：读 User/sdk.toml → 调 tools/*
│   ├── .vscode/      # tasks/launch/keybindings 接口模板（task 调 sdk_run.py）
│   └── User/         # sdk.toml / board_cfg.h 模板
├── sdk_manifest.json # SDK 自描述（模块/依赖/版本/文件清单）
└── readme.md
```

## 设计约定

- **分层不按物理位置混淆**：`chip/`=MCU 内，`devices/`=板外，`protocols/`=协议，`middleware/`=第三方。弃用 `Core/bsp/Drivers` 笼统或越权名。
- **HAL 保留**：CubeMX 生成的 HAL 仍是 vendor 底层，OOP 封装包在它上面；明确否决「全去 HAL 自写」。
- **单源真相**：本仓库是本地共享 SDK（镜像 Zephyr 的 `zephyrproject`），bug 改一处。项目通过 `sync_lib.py` 拷贝选中子集进 `MySDK/`，不引用本仓库路径。
- **`MySDK/` 必须进工程版本库**（不 gitignore）：虽然它由 sync 生成，但对拿到工程的人它就是源码的一部分——缺了不知道少什么、也无法直接编译。定位同 CubeMX 生成的 HAL 库与初始化代码，一律入库。
- **版本号规则**：SDK 顶层用语义化版本（如 `0.1.0`）；模块 `version` 沿用源码 `@version` 标注（Vx.y 或第三方原生版本），无标注者记为 null。

## 工程接入指南

SDK 是单源真相仓库。通过 `tools/sync_lib.py` 按 `sdk.toml` 选模块，把闭包子树镜像到
工程的 `MySDK/`，工程侧 `add_subdirectory(MySDK)` + 链接 `mystm32` 静态库即可，换板只改
`board_cfg.h`。

**`MySDK/` 要提交进工程的版本库**（不要加进 `.gitignore`）。它是「生成」的，但对 clone
工程的人而言就是源码本身——类比 CubeMX 生成的 HAL 库与初始化代码，一律入库，保证工程
自包含、开箱可编译。因此每次 sync 之后，`git status` 里出现的增删改都应随工程一起提交。

### 1. 工程侧需要准备什么

工程根放置 `sdk.toml`（可参考 SmartHome 工程）：

```toml
sdk       = "<SDK 仓库绝对路径>"   # SDK 绝对路径（无连字符）；省略时 sync_lib.py 自动定位仓库
dest      = "MySDK"                  # 镜像目标（相对 toml 所在目录解析）
board_cfg = "User/board_cfg.h"      # 硬件绑定文件（已存在绝不覆盖）

[modules]                              # 只列叶子模块，depends 闭包自动补全
"chip.oop_dwt"   = true
"devices.ir_tx"  = true
# ...
```

| 字段 | 含义 | 备注 |
|---|---|---|
| `sdk` | SDK 根目录（绝对路径） | 路径必须为 `mystm32-sdk`（无连字符），写成 `my-stm32-sdk` 会报「根目录不存在」；省略时 sync_lib.py 自动以其自身所在目录定位仓库 |
| `dest` | 镜像目标目录 | 相对 toml 所在目录；根 CMakeLists 的 `add_subdirectory` 名必须与之一致 |
| `board_cfg` | 绑定文件路径 | 仅首次生成模板；之后保留工程资产，不被覆盖 |
| `[modules]` | 模块选择 | 写 `= true` 的模块，其 `depends` 由脚本递归补全；`external:*` 依赖跳过、由工程侧提供 |

`board_cfg.h` 是唯一绑定点：全工程唯一允许 `#include "main.h"/"tim.h"/"usart.h"`
并引用 CubeMX 全局句柄（`&htim6`、`&hiwdg`、引脚宏）的地方。SDK 内部零绑定、板无关。

> 布局约定：把 `sdk.toml` 与 `board_cfg.h` 放在 `MySDK/` **之外**（如 `User/`），
> 因为 `MySDK/` 每次拉取会被整体清空重建。推荐结构见第 4 节。

### 2. 如何拉取

```bash
# 直接跑 SDK 提供的脚本（需要 python >= 3.11，tomllib 必需）
python <SDK根>/tools/sync_lib.py <工程根>/User/sdk.toml

# 演习不写盘
python <SDK根>/tools/sync_lib.py <工程根>/User/sdk.toml --dry-run
# 覆盖 toml 里的 sdk 路径
python <SDK根>/tools/sync_lib.py <工程根>/User/sdk.toml --sdk <其他 SDK 根>
```

拉取行为：
1. 解析所选模块 `depends` 闭包（拓扑序）；引用了 manifest 不存在的模块直接报错退出。
2. **先 `rmtree(dest)` 再整目录镜像** `library/<layer>/<module>/` → `dest/<layer>/<module>/`
   （手动改 `MySDK/` 会被清空，要改只能改 SDK 源仓后重新拉取）。
3. 拷贝 SDK 根 `CMakeLists.txt` → `dest/CMakeLists.txt`（组件构建脚本）。
4. `board_cfg.h` 仅在「读取 1 字节确认不存在」时生成模板（避免云端盘在线占位误覆盖）。
5. 写 `dest/_sdk_sync.txt` 戳（SDK 版本 / 时间 / 模块清单）。

### 3. 接入编译

工程根 `CMakeLists.txt` 三处：

```cmake
add_subdirectory(cmake/stm32cubemx)   # 先 CubeMX（提供 HAL/RTOS 头与 stm32cubemx 接口目标）
add_subdirectory(MySDK)               # 必须在其后（SDK 会自动 link stm32cubemx）
# ...
target_link_libraries(<主目标> PRIVATE mystm32)  # 链接 SDK 静态库
```

SDK 自带 `CMakeLists.txt` 用 `GLOB_RECURSE ... CONFIGURE_DEPENDS` 收集所有 `*/Src/*.c`
并 PUBLIC 导出各 `*/Inc`——**新增模块无需改清单**，下次构建自动重扫。
非 CubeMX 工程需自行给 `mystm32` 注入 HAL 头路径与 `STM32Fxxx/USE_HAL_DRIVER` 宏。

### 4. 增删模块

- **加**：在 `[modules]` 加 `"<layer>.<name>" = true`，闭包自动补全（如选
  `devices.ir_tx` 会自动带入 `chip.oop_gpio/chip.oop_tim/chip.platform/middleware.SEGGER_RTT`）。重跑 sync。
- **删**：把对应行改成 `= false`（或删行），重跑 sync 会按新闭包重新镜像。
- **查可用模块 id**：看 `sdk_manifest.json` 的 `modules[].id`。

### 5. 注意事项 / 坑

- **别手改 `MySDK/` 内任何文件**：每次拉取整体覆盖（要改就改 SDK 源仓再重新拉取）。
- **拉取后记得提交 `MySDK/`**：它已入库，sync 完 `git status` 会列出增删改，这些变更属于工程的一部分，需一并提交；否则别人 clone 到的仍是旧镜像。
- **`board_cfg.h` 会被保留**，但前提是它是「真实非空文件」。云端盘（Google Drive 在线-only）
  占位文件会让 `os.path.exists` 误判，已用「读 1 字节」加固；若仍被覆盖，从 SDK 外备份恢复。
- **`external:lwip` 不进 SDK**：`protocols.wol` 依赖 lwIP，属 CubeMX Middlewares，工程侧提供。
- **LwIP 自带 mqtt 冲突**：需 EXCLUDE 掉 `LwIP/apps/mqtt/mqtt.c`，避免与 `protocols.mqtt` 同名符号冲突。
- **Python 版本**：`sync_lib.py` 用 `tomllib`，需 `>= 3.11`。
- **审计局限**：`tools/oop_audit.py` 只查 CubeMX 头/全局句柄引用，**查不出直调 HAL 函数**；
  devices 层零直调 HAL 需靠 `grep -E "HAL_(TIM|IWDG|GPIO|Delay|GetTick)"` 兜底。
- **路径风格**：sync 命令用 Windows 风格 `C:/...`，避免 Git Bash 把 `/c/...` 解析成 `c:\c\...`。
- **工程侧接口（SDK 位置只在 `sdk.toml` 一处配置）**：把 `manual/` 的内容拷到工程——
  `manual/sdk_run.py` → 工程根、`manual/.vscode/*` → 工程 `.vscode/`、`manual/User/*` → 工程 `User/`。
  之后 `.vscode` 任务（Build/Pull/Audit/Flash/Debug）经 `sdk_run.py` 桥接调用 `tools/*`，
  **task.json 与 sdk_run.py 都不硬编码 SDK 路径**；换 SDK 目录只改 `sdk.toml` 的 `sdk = "..."` 一行。

## 版本变更记录

### 仓库结构三层化（2026-08-29，SDK 组织重构，模块 API 不变）

- **`library/`**：原 `chip/ devices/ protocols/ middleware/` 整体移入 `library/`，作为**按 `sdk.toml` 自动拉取**的固件源码根。`sync_lib.py` 源根改为 `library/`、目标仍落 `dest/<layer>/<module>/`（剥离前缀），`sdk_manifest.json` 的 `path` 不变；`oop_audit.py` 扫描目录同步加 `library/` 前缀。
- **`tools/`**：合并原 `.sdktool/` 与既有维护脚本，成为**全部 host 工具单源**（只调用不拉取）：`sync_lib.py`/`oop_audit.py`（SDK 维护）、`flash.bat`/`trans_gbk2utf-8.py`（工程面向）。新增 `tools/README.md` 标注分类。
- **`manual/`**：新增**手动拷贝脚手架**（不自动拉取）：`sdk_run.py`（工程侧桥接，读 `User/sdk.toml` 的 `[sdk]` → 调 `tools/*`，使 SDK 位置只在 `sdk.toml` 一处配置）、`.vscode/`（tasks/launch/keybindings 接口模板，任务经 `sdk_run.py` 调工具）、`User/`（sdk.toml / board_cfg.h 模板）。
- 工程侧 `.vscode` 任务的 `flash/pull/audit` 全部经 `sdk_run.py` 分发，**task.json 不出现 SDK 路径或相对深度**；换 SDK 目录只需改 `sdk.toml` 的 `sdk = "..."` 一行。

### v0.3.0 (2026-08-29) — devices 层零直调 HAL + 任务接口统一

- **新增 chip OOP 封装（补齐 TIM / IWDG 边界）**：
  - `chip/oop_tim`（`oop_tim_drv.h/.c`）：封装 `HAL_TIM_PWM_Init/ConfigChannel/Start/Stop/DeInit` 与 `HAL_TIM_Base_Start_IT/Stop_IT`，device 层统一走本封装。
  - `chip/oop_iwdg`（`oop_iwdg_drv.h/.c`）：封装 `HAL_IWDG_Refresh`，内部按 `HAL_IWDG_MODULE_ENABLED` 守卫，未启用时为空操作，device 层无需自行 `#ifdef`。
  - `chip/oop_dwt` 新增 `oop_GetTickMS()`（封装 `HAL_GetTick`），device 层时间基准统一走 OOP。
- **devices 层去直调 HAL（审计结论更新）**：
  - `ir_tx`：`HAL_GPIO_WritePin/Init` → `oop_gpio_init_af` + `OOP_GPIO_WRITE_RAW`；`HAL_TIM_PWM_*` → `oop_tim_pwm_*`。
  - `ir_1838b`：`HAL_TIM_Base_Start_IT/Stop_IT` → `oop_tim_base_start_it/stop_it`；`HAL_GetTick` → `oop_GetTickMS`。
  - `heart_beat`：`HAL_IWDG_Refresh` → `oop_iwdg_refresh`。
  - `lan8720a`：`HAL_Delay` → `oop_DelayMS`。
  - `hlk_rm58s`：`HLK_GET_TICK_MS` 宏改走 `oop_GetTickMS`。
  - 现 6 个 device 全部经 OOP 操作；`dht11` 仍经 `OOP_GPIO_READ_RAW/WRITE_RAW`（OOP 层原始原语）与 `__disable_irq/__enable_irq`（CMSIS 内核内联，非 HAL）。
- **任务调用接口收口（工程侧 SmartHome）**：device init 从各任务体提到 `Task_X_Init()`，`app_main_create_tasks()` 统一调用（详见项目改造）。
- 注意：`oop_audit.py` 仅查「工程头 / 具体句柄引用」，**不查 HAL 函数调用**；本版额外以 `grep` 确认 devices 无 `HAL_TIM_*`/`HAL_IWDG_*`/`HAL_GPIO_*`/`HAL_Delay`/`HAL_GetTick` 直调残留。

### v0.2.0 (2026-08-28) — bsp_* 全面更名 oop_* + 依赖规范化

- **破坏性更名**：chip 层 `bsp_*` → `oop_*`（目录、文件、内部符号、宏、头文件守卫全部同步）：
  - `chip/bsp_dwt` → `chip/oop_dwt`（`oop_dwt.h/.c`，`bsp_DelayUS` 等 → `oop_DelayUS`）
  - `chip/bsp_gpio` → `chip/oop_gpio`（`oop_gpio_drv.h/.c/.c(_hal)`，`bsp_gpio_*` → `oop_gpio_*`，`BSP_GPIO_*` → `OOP_GPIO_*`）
  - `chip/bsp_uart` → `chip/oop_uart`（同上规则）
  - `tools/bsp_audit.py` → `tools/oop_audit.py`
  - devices / protocols 中的所有调用点同步更新，全库 `bsp_` / `BSP_` token 清零。
- **platform 重新处理**：`hal_platform.h` 注释中旧文件名引用同步为 `oop_uart_drv.c`；该头仍是全库唯一 HAL 入口（系列宏 `HAL_PLATFORM_G4/F4`）。
- **依赖规范化（manifest 与实际 include 对齐）**：
  - cJSON 归位 `middleware/cJSON`（第三方库入中间件层），manifest 条目 `protocols.cJSON` → `middleware.cJSON`，文件路径校验通过。
  - `mqtt` 模块补齐依赖声明：`middleware.cJSON`（`mqtt_codec.c` include `cJSON.h`）。
- 审计结论：devices/protocols 只 include ①自身头 ②chip 公共头（`oop_*.h` / `hal_platform.h`）③middleware 日志头 ④libc；无内部 `_hal` 后端泄漏、无 CubeMX 工程头、无跨层违规。

### v0.1.1 (2026-08-28) — 修复 OOP 封装泄漏 HAL

- **核心约束**：SDK 不再包含任何工程生成头（`main.h` / `tim.h` / `gpio.h` / `usart.h` / `config_network.h` 等），也不再引用具体全局句柄（`&huart6` / `&htim6` / `hiwdg` / `gnetif`）或 MX 引脚宏（`XXX_GPIO_Port` / `XXX_Pin`）。
- **具体句柄 / IO / 定时器 / 网口全部改为由调用方注入**（工程 `board_cfg` 完成绑定），SDK 仅做板无关 OOP 封装：
  - `oop_uart`：删除硬编码 `&huart6` 描述表，`huart` 由 `uart_drv_init()` 注入。
  - `ir_1838b`：`IR1838B_Init(port, pin, htim)` 增加 TIM 句柄注入。
  - `ir_tx`：新增 `ir_tx_cfg_t`，`IR_TX_Init(cfg)` 注入 GPIO/TIM/载波参数（原 PE6/TIM9/PSC/ARR 全部外提）。
  - `heart_beat`：`heart_beat_init(led_port, led_pin, hiwdg)` 注入 LED 与看门狗句柄，删除 `MX_IWDG_Init` / `CPU_STA_*`。
  - `lan8720a`：`ETH_RST_Init(port, pin)` 注入复位引脚。
  - `dht11`：`DHT11_Init(port, pin)` 注入 DAT 引脚。
  - `wol`：`send_wol(mac, netif)` / `wol_print_mac(mac)` / `wol_check_network_ready(netif)` 注入目标 MAC 与网口。
  - `oop_dwt`：头文件 `main.h` → `hal_platform.h`。
- 新增 `tools/oop_audit.py` 做 HAL 泄漏静态检查（`python tools/oop_audit.py --strict`），回归防护。

### v0.1.0 (2026-08-28) — 初始版本

- 建立四层结构 `chip/ devices/ protocols/ middleware/`，确定层命名与 `hal_platform` 移植层。
- 收录模块（含各自版本）：
  - chip：oop_dwt (V2.0)、oop_gpio (V3.1)、oop_uart (V3.0)、hal_platform (V1.0)
  - devices：dht11 (V3.1)、heart_beat (无标注)、hlk_rm58s (无标注)、ir_1838b (V1.2)、ir_tx (V1.0)、lan8720a (无标注)
  - protocols：ac_codec (V1.0)、cJSON (1.7.18)、mqtt (无标注)、wol (无标注)
  - middleware：SEGGER_RTT (第三方，无标注)
- 新增 `sdk_manifest.json`（schema 1.0）：描述 families / layers / modules（depends、backends、files、version），供 `sync_lib.py` 与未来 GUI 读取。
- 已知待清理：`middleware/cJSON` 为空占位目录，真实 cJSON 已落在 `protocols/cJSON`，后续删除空占位。
