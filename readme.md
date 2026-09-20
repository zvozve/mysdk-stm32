# mystm32-sdk

本仓库用于存放自建 STM32 SDK。

仓库分四层，职责互不混淆：
- `library/` —— 固件源码（按 `sdk.toml` 自动拉取到工程 `MySDK/`），含：
  - 片上寄存器 OOP 封装（基于 HAL） —— `library/chip/`
  - 板外设备驱动 —— `library/devices/`
  - 协议封装 / 可复用算法 —— `library/protocols/`
  - 系统服务（固件生命周期、升级等） —— `library/services/`
  - 第三方中间件 —— `library/middleware/`
- `tools/` —— 跑在开发 PC 上的工具（**单源、只调用不拉取**）：`sdk-pull.py` / `sdk-check-oop.py` / `check-eol.py`（SDK 维护）、`fw-flash.py` / `format-gbk2utf8.py` / `fw-ota-ymodem.py`（工程面向）。
- `manual/` —— 手动拷贝到工程的脚手架（`sdk_run.py` 桥接 + `.vscode` 接口模板 + `User/` 配置模板），**不自动拉取**。

## 版本

- SDK 版本：**0.11.0**（2026-09-18）
- Manifest schema：**1.0**（`sdk_manifest.json`）
- 支持的 MCU 系列：STM32F1、STM32F4、STM32G4（由 `chip/platform/Inc/hal_platform.h` 按编译宏自动展开）

## 目录结构

```
mystm32-sdk/
├── library/          # 固件源码：按 sdk.toml 自动拉取 → 工程 MySDK/
│   ├── chip/         # MCU 内部外设 OOP 封装（板无关，基于 HAL）
│   │   ├── oop_dwt/ oop_gpio/ oop_uart/ oop_tim/ oop_iwdg/ oop_spi/ oop_flash/ oop_boot/ platform/
│   ├── devices/      # 板载外挂芯片驱动（坐 chip/ 总线）
│   │   ├── dht11/ heart_beat/ hlk_rm58s/ ir_receiver/ ir_transmitter/ lan8720a/ oled12864/ led_matrix/ ws1850s/ spi_nor_flash/
│   ├── protocols/    # 协议 / 算法库
│   │   ├── ac_codec/ cli/ mqtt/ wol/ ymodem/
│   ├── services/     # 系统服务（板无关，同 devices/protocols 禁止直调 HAL）
│   │   ├── ota_core/ ota_src_mem/ bootloader/ ota/ ota_src_uart/
│   └── middleware/   # 第三方调试/传输库
│       ├── cJSON/ SEGGER_RTT/
├── tools/            # 全部 host 工具（单源，只调用不拉取）
│   ├── sdk-pull.py   # 子集拉取（SDK 维护）
│   ├── sdk-check-oop.py  # HAL 泄漏检查（SDK 维护）
│   ├── check-eol.py      # 行尾体检：CR 加倍 / 孤立 CR（SDK 维护；已并入 audit）
│   ├── fw-ota-pack.py   # OTA 固件打包 bin -> .otapkg（工程面向；经 sdk_run.py pack 调用）
│   ├── fw-flash.py      # J-Link 烧录（工程面向；自检 J-Link 路径/器件名并回写 settings.json 供 debug 用）
│   └── format-gbk2utf8.py  # GBK→UTF-8（工程面向）
├── manual/           # 手动拷贝的脚手架（不自动拉取）
│   ├── sdk_run.py    # 工程侧桥接：读 User/sdk.toml → 调 tools/*
│   ├── .vscode/      # tasks/launch/keybindings 接口模板（task 调 sdk_run.py）
│   ├── User/         # sdk.toml / board_cfg.h 模板
│   └── .gitignore    # 工程默认忽略项（拷到工程根；含「必须入库」反面清单）
├── sdk_manifest.json # SDK 自描述（模块/依赖/版本/文件清单）
└── readme.md
```

## 设计约定

- **分层不按物理位置混淆**：`chip/`=MCU 内，`devices/`=板外器件，`protocols/`=协议/算法，`services/`=系统服务（固件生命周期等），`middleware/`=第三方。弃用 `Core/bsp/Drivers` 笼统或越权名。
- **`services/` 的判据**：不是协议、也不是某颗芯片，而是「跨项目的系统级流程」。放进来的是 OTA / bootloader 这类**固件生命周期**服务；塞不进原来四层、又想复用，才开这一层，别把随便什么工具函数都塞进去。
- **HAL 保留**：CubeMX 生成的 HAL 仍是 vendor 底层，OOP 封装包在它上面；明确否决「全去 HAL 自写」。
- **HAL 泄漏红线**：`tools/sdk-check-oop.py` 扫 `library/{chip,devices,protocols,services}`。**只有 `chip/` 允许直接调用 `HAL_*` 函数或定义 `HAL_*Callback`**；其余四层一律只调 `oop_*`。跑 `--strict` 必须过。
- **单源真相**：本仓库是本地共享 SDK（镜像 Zephyr 的 `zephyrproject`），bug 改一处。项目通过 `sdk-pull.py` 拷贝选中子集进 `MySDK/`，不引用本仓库路径。
- **`MySDK/` 必须进工程版本库**（不 gitignore）：虽然它由 sync 生成，但对拿到工程的人它就是源码的一部分——缺了不知道少什么、也无法直接编译。定位同 CubeMX 生成的 HAL 库与初始化代码，一律入库。
- **版本号规则**：SDK 顶层用语义化版本（如 `0.1.0`）；模块 `version` 沿用源码 `@version` 标注（Vx.y 或第三方原生版本），无标注者记为 null。
- **行尾约定（改既有文件时必须守）**：要么**字节进出**（`open(p,"rb")` 读 → `open(p,"wb")` 写），要么读写都显式 `newline=""`。**禁止「保留换行地读 + 默认模式写」**——Python 写时会把 `\n` 再翻成 `os.linesep`，于是每编辑一次行尾就多一个 CR（`\n` → `\r\r\n` → `\r\r\r\n`）：编辑器里每行看着像多一个空行，且该文件会被 git 判为 `i/-text`（不参与行尾归一化），**脏字节直接进仓库**。回归检查：`python tools/check-eol.py`（已并入 `sdk_run.py audit`）。

## 工程接入指南

SDK 是单源真相仓库。通过 `tools/sdk-pull.py` 按 `sdk.toml` 选模块，把闭包子树镜像到
工程的 `MySDK/`，工程侧 `add_subdirectory(MySDK)` + 链接 `mystm32` 静态库即可，换板只改
`board_cfg.h`。

> **`MySDK/` 必须提交进工程版本库**——详见下方「5. 注意事项 / 坑」。

### 1. 工程侧需要准备什么

工程根放置 `sdk.toml`（可参考 SmartHome 工程）：

```toml
sdk       = "<SDK 仓库绝对路径>"   # SDK 绝对路径（无连字符）；省略时 sdk-pull.py 自动定位仓库
dest      = "MySDK"                  # 镜像目标（相对 toml 所在目录解析）
board_cfg = "User/board_cfg.h"      # 硬件绑定文件（已存在绝不覆盖）

[modules]                              # 只列叶子模块，depends 闭包自动补全
"chip.oop_dwt"   = true
"devices.ir_transmitter"  = true
# ...
```

| 字段 | 含义 | 备注 |
|---|---|---|
| `sdk` | SDK 根目录（绝对路径） | 路径必须为 `mystm32-sdk`（无连字符），写成 `my-stm32-sdk` 会报「根目录不存在」；省略时 sdk-pull.py 自动以其自身所在目录定位仓库 |
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
python <SDK根>/tools/sdk-pull.py <工程根>/User/sdk.toml

# 演习不写盘
python <SDK根>/tools/sdk-pull.py <工程根>/User/sdk.toml --dry-run
# 覆盖 toml 里的 sdk 路径
python <SDK根>/tools/sdk-pull.py <工程根>/User/sdk.toml --sdk <其他 SDK 根>
```

拉取行为：
1. 解析所选模块 `depends` 闭包（拓扑序）；引用了 manifest 不存在的模块直接报错退出。
2. **先 `rmtree(dest)` 再整目录镜像** `library/<layer>/<module>/` → `dest/<layer>/<module>/`
   （手动改 `MySDK/` 会被清空，要改只能改 SDK 源仓后重新拉取）。
3. 拷贝 SDK 载荷根 `library/CMakeLists.txt` → `dest/CMakeLists.txt`（组件构建脚本）；它就在各 layer 目录同级，故源仓内也能直接配置。
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

SDK 的 `library/CMakeLists.txt`（拉取后即 `MySDK/CMakeLists.txt`）用 `GLOB_RECURSE ... CONFIGURE_DEPENDS` 收集所有 `*/Src/*.c`
并 PUBLIC 导出各 `*/Inc`——**新增模块无需改清单**，下次构建自动重扫。
非 CubeMX 工程需自行给 `mystm32` 注入 HAL 头路径与 `STM32Fxxx/USE_HAL_DRIVER` 宏。

### 3b. 多槽 OTA 工程：一次 build 出三份（base / A / B）

A/B 双槽 OTA 的 APP **必须在链接期**就链接到槽地址——Flash 只是把它放到那里。
只把地址写进烧录脚本是不够的：`.bin` 内部不含地址，链接地址由 `-T` 的 `.ld` 决定，
两者不一致的后果是 **BL 跳到槽、APP 起不来**（向量表的复位向量指回 BL 区）。

典型现场：APP 被链接在 `0x08000000`（基版 `.ld`），却被烧到 `0x08010000`。
判据：读 `.bin` 首 8 字节，第二个字是复位向量。

```
SP=0x20020000  Reset=0x0800C04D    链接在 0x08000000（错，与槽不符）
SP=0x20020000  Reset=0x0801xxxx    链接在 0x08010000（对）
```

Cortex-M 是绝对寻址，「一个位置」就对应「一份二进制」，所以三份产物是必然的：

| 产物 | 链接地址 | 用途 |
|---|---|---|
| `TP_MDC.bin` | `0x08000000` / 1M | **不带 BL**，可独立烧到默认位置直接运行 |
| `TP_MDC_A.bin` | `0x08010000` / 448K | 进 A 槽 |
| `TP_MDC_B.bin` | `0x08080000` / 512K | 进 B 槽 |

做法：逻辑全在 **SDK 侧** —— `library/services/ota_core/cmake/ota_slots.cmake`
（随模块拉取到 `MySDK/services/ota_core/cmake/`），由 `<MySDK>/CMakeLists.txt` 末尾自动
挂载。**工程根 `CMakeLists.txt` 不需要任何 OTA 槽相关内容**。

`ota_slots.cmake` 内部：

1. 由基版 `.ld` 派生三份 `.ld` 写到 `${CMAKE_BINARY_DIR}`（= 与 `.bin` 同目录）：
   `<name>.ld`（基版原样副本）+ `<name>_A.ld` + `<name>_B.ld`；
2. **先从 `CMAKE_EXE_LINKER_FLAGS` 里摘掉 toolchain 写死的 `-T <基版.ld>`** —— 否则
   「全局 `-T` + 目标级 `-T`」会让 ld 看到两个同名 `MEMORY`（redeclaration 警告 + 段重复）。
   实测这个修改**不受时机限制**：晚改对已创建的目标同样生效，所以能藏在子目录里做；
3. 用 `cmake_language(DEFER DIRECTORY <工程根>)` 把「建 A/B 目标」推迟到**工程根目录处理
   结束时**执行 —— 那时 base 目标的源/链接库才齐全（CubeMX 的源挂在
   `cmake/stm32cubemx/` 子目录、用户源挂在工程根，太早复制会漏源）；随后用
   `get_target_property(... SOURCES/LINK_LIBRARIES)` 复制给 `<name>_A` / `<name>_B`
   （自动跟随 CubeMX，不必手抄源列表）；
4. 每个目标 `target_link_options(-T <自己的.ld> -Wl,-Map=<自己>.map)` +
   `target_compile_definitions(OTA_SELF_BASE=<自己的基址>)`。

- 地址**全部从 `.ld` 解析**，不写死任何地址或偏移（槽几何只在 CMake 里给一次）。
- 根目录**不需要**预置 `slotA/slotB.ld`，也**不改动**根目录那份基版 `.ld`。
- `OTA_SELF_BASE` 只被应用层（`app_ota.c`）使用，SDK 不用它 → 三个目标能共享同一批
  SDK/HAL 编译产物（它们都是 OBJECT/INTERFACE 库），只有应用层编三遍。
- 基版 `.ld` 的 `MEMORY` 行格式变了要 `FATAL_ERROR`，绝不静默错链。
- 改了 `CMakeLists.txt` 必须**重新 configure**（不是只 rebuild），并在
  `--print-memory-usage` 里确认三个目标的 FLASH 区分别是 1M / 448K / 512K。

烧录侧：`fw-flash.py` 会优先用「与 `.elf` 同目录的同名 `.ld`」解析地址 —— 所以 VSC 的
`Flash` / `Flash Slot-A` / `Flash Slot-B` 三个任务只要换 `.elf`，**任务里不出现任何地址**。
OTA（`ymodem`）由设备自报目标槽，主机自动挑对应后缀的那份镜像。

### 3c. 多槽开关（都在 include `ota_slots.cmake` 之前 `set`）

| 变量 | 作用 |
|---|---|
| `OTA_MULTISLOT` | `OFF` = 关掉整个多槽逻辑，只留 base 一份产物。**BL 工程必须设 OFF**（它是引导程序，多槽三分片是 APP 的事） |
| `OTA_BASE_LD` | 基版链接脚本路径；默认在工程根自动找唯一的 `*.ld`（多于一份则 fail-fast） |
| `OTA_SLOT_A_ORIGIN` / `OTA_SLOT_A_LENGTH` | 显式覆盖槽 A 几何（B 同理） |
| `OTA_SELF_BASE_MACRO` | 注入应用层的「我在哪」宏名，默认 `OTA_SELF_BASE` |

槽几何默认按**基版容量派生**（1MB → A = `+0x10000`/448K、B = `+0x80000`/512K，与工程分区表
`User/Src/ota_areas.c` 对齐）；未知容量会 fail-fast 要求显式给出，绝不猜。

### 3d. 产物生成（.hex / .bin）也在 SDK 侧

同样由 SDK 载荷根统一挂载（`<MySDK>/CMakeLists.txt` 末段）—— 工程根 `CMakeLists.txt`
里不再需要那段 objcopy 样板（CubeMX 生成的 CMakeLists 本来也不含它，历来靠手加）。

- 经 `cmake_language(DEFER DIRECTORY <工程根>)` 执行。原因：`add_custom_command(TARGET ...)`
  要求目标与调用方**在同一目录**，而 SDK 载荷根跑在 `<MySDK>/` 子目录、宿主主目标建在工程根
  —— 直接调用会报 `TARGET ... was not created in this directory`（实测踩到）。
- 用 `if(TARGET ${CMAKE_PROJECT_NAME})` 保护：宿主只把本组件当静态库用时不报错、静默跳过。
- 关掉：`set(MYSDK_ARTIFACTS OFF)`。
- 多槽工程的 `<name>_A` / `<name>_B` 由 `ota_slots.cmake` 各自生成（同样是工程根 DEFER）。

### 4. 增删模块

- **加**：在 `[modules]` 加 `"<layer>.<name>" = true`，闭包自动补全（如选
  `devices.ir_transmitter` 会自动带入 `chip.oop_tim/chip.oop_dma/chip.platform`）。重跑 sync。
- **删**：把对应行改成 `= false`（或删行），重跑 sync 会按新闭包重新镜像。
- **查可用模块 id**：看 `sdk_manifest.json` 的 `modules[].id`。

### 5. 注意事项 / 坑

- **别手改 `MySDK/` 内任何文件**：每次拉取整体覆盖（要改就改 SDK 源仓再重新拉取）。
- **拉取后记得提交 `MySDK/`**：它已入库，sync 完 `git status` 会列出增删改，这些变更属于工程的一部分，需一并提交；否则别人 clone 到的仍是旧镜像。
- **`board_cfg.h` 会被保留**，但前提是它是「真实非空文件」。云端盘（Google Drive 在线-only）
  占位文件会让 `os.path.exists` 误判，已用「读 1 字节」加固；若仍被覆盖，从 SDK 外备份恢复。
- **`external:lwip` 不进 SDK**：`protocols.wol` 依赖 lwIP，属 CubeMX Middlewares，工程侧提供。
- **LwIP 自带 mqtt 冲突**：需 EXCLUDE 掉 `LwIP/apps/mqtt/mqtt.c`，避免与 `protocols.mqtt` 同名符号冲突。`protocols.mqtt` 的线缆编解码已改用 vendored 的 Eclipse Paho `MQTTPacket`（零 HAL/OS 依赖、纯 TCP 1883、传输由用户注入），详见 `library/protocols/mqtt/readme.md`。
- **LwIP 接入完整避坑清单（CubeMX DNS / RTOS 任务栈 512×4 / MicroLIB / PHY 9 脚电源 / 晶振 / LED 检查）**：见 `library/devices/lan8720a/readme.md`。
- **OLED 驱动（oled12864）文本/字模用法**：字模由用户按 `oled_font_t` 注入 + `OLED_RegisterFont` 注册，再 `OLED_DrawString` 显示；驱动不内置字库，详见 `library/devices/oled12864/readme.md`。
- **RFID 读卡（ws1850s）异步用法**：`uart_drv_t*` 注入 + 周期调用 `ws1850s_process()`（拉取收包/状态机），卡片/结果经 `card_cb`/`result_cb` 回调上抛，全程不阻塞；详见 `library/devices/ws1850s/readme.md`。
- **IR 收发（`ir_receiver` / `ir_transmitter`）的 TIM 配置依赖**：`ir_receiver` = **一路通用定时器配成 Input Capture**（通道须接到解调接收头 OUT；1MHz 计数、ARR 取满量程、使能 NVIC），在 `HAL_TIM_IC_CaptureCallback()` 里转调 `ir_receiver_isr()`；`ir_transmitter` = **一路 TIM PWM 通道**（载波，ARR/CCR 由 cfg 覆写）+ **另一路空闲 TIM 作 µs 时基**（自由运行 1MHz，无引脚、无中断）。**不用 DWT**——工程侧实测部分 F1 板 CYCCNT 不可靠（自检偶发通过、发码时却冻结）。两个模块都是**多实例**（实例 + cfg 注入），详见各自 `readme.md`。
- **串口命令行（`protocols.cli`）用法**：传输由工程注入 `cli_transport_t`（`getc` / `write` / `flush`，三者都带 `ctx`），命令用 `cli_register()` 因表注册、业务模块自带 `xxx_cli_cmds[]`，app 侧只做聚合；内置防回显自激四道闸（一次只执行一条 / 执行后 `flush` / 命令级 `guard_ms` / 未知行静默+限速摘要），handler **不得阻塞**（长动作只置标志，交任务执行）；详见 `library/protocols/cli/readme.md`。
- **Python 版本**：`sdk-pull.py` 用 `tomllib`，需 `>= 3.11`。
- **审计局限**：`tools/sdk-check-oop.py` 只查 CubeMX 头/全局句柄引用，**查不出直调 HAL 函数**；
  devices 层零直调 HAL 需靠 `grep -E "HAL_(TIM|IWDG|GPIO|Delay|GetTick)"` 兜底。
- **路径风格**：sync 命令用 Windows 风格 `C:/...`，避免 Git Bash 把 `/c/...` 解析成 `c:\c\...`。
- **行尾「CR 加倍」事故（已设防呆）**：临时改写脚本若「保留换行地读 + 默认文本模式写」，每改一次就给行尾多加一个 `\r`（`\n` → `\r\r\n` → `\r\r\r\n`）。症状是编辑器里每行后像多了个空行；更麻烦的是 `git ls-files --eol` 会把该文件标成 `i/-text` —— **它不参与行尾归一化，脏字节会被提交进仓库**（`library/CMakeLists.txt` 曾中招：4108 B 纯 LF → `\r\r\n` → `\r\r\r\n`）。检查：`python tools/check-eol.py`（`-v` 列 warn 明细），已并入 `python sdk_run.py audit` 与 VSC「SDK Check OOP」任务。修法：按 LF 切分、逐行 `rstrip(b"\r")`、以单 LF 拼回，并断言「抹掉所有 CR 后字节完全相同」。
- **工程侧接口（SDK 位置只在 `sdk.toml` 一处配置）**：把 `manual/` 的内容拷到工程——
  `manual/sdk_run.py` → 工程根、`manual/.gitignore` → 工程根、`manual/.vscode/*` → 工程 `.vscode/`、
  `manual/User/*` → 工程 `User/`。
  之后 `.vscode` 任务（Build/Pull/Audit/Flash/Debug）经 `sdk_run.py` 桥接调用 `tools/*`，
  **task.json 与 sdk_run.py 都不硬编码 SDK 路径**；换 SDK 目录只改 `sdk.toml` 的 `sdk = "..."` 一行。
- **`.gitignore` 两个，目的不同**：仓库根那份管 SDK 自身；`manual/.gitignore` 是给工程的默认模板（随脚手架拷走）。
  模板只忽略「可再生成的产物」与「本机/个人工具态」，并把 `MySDK/`、`User/`、`.vscode/`、`*.ioc`/`.mxproject`、
  `sdk_run.py` 列成**禁止忽略反面清单**——这几项看着像产物，缺了别人 clone 后编译不过或无法复现。
  写规则时**注释必须独立成行**：`.cache/  # 缓存` 这种行尾注释会被 gitignore 当成模式的一部分，规则静默失效
  （本文件初版踩过：`.cache/`、`.vscode/*.log`、`*.ioc.broken` 三条全部没生效）。

## 版本变更记录
### 新增 protocols.ymodem + services.ota + services.ota_src_uart（2026-09-18，v0.11.0）

APP 侧升级链路打通（P3）。方案文档：`ota-demo-stm32/doc/01~06`。

- **新增 `protocols.ymodem`（V1.0）**：YMODEM 收发引擎。参考工程（`refe/Ymodem-master` =
  STM32F0xx_IAP、`refe/F407ZG/.../YMODEM`）都是**阻塞**实现 —— 在一个函数里死等包头、死等 ACK、
  收完整个文件才返回。那套写法在独立 BL 里勉强能用，装进带协议栈的 APP 就不行：一次传输几秒到
  几十秒，期间什么都不干，看门狗会先咬人。这里整体重写为**非阻塞状态机**，且**零硬件依赖**
  （收字节 / 发字节 / 时基全部注入）。
  - **因此它能上 PC**：单测把收发两个引擎通过内存通道对接，注入真机上很难复现的场景 ——
    ACK 整包丢失 → 发送端重发 → **接收端去重**、线路噪声 → NAK → 重传、接收端头包处拒收 → CAN 中止、
    空文件，共 7 组端到端用例 + 9 项帧工具断言，全过。
  - 协议易错点都写进了模块 readme：**SOH 恒配 128 / STX 恒配 1024**（拿 SOH 发 1024 会让整包错位，
    而 CRC 仍能过 —— 异常得很安静）；序号 0 是头包、255→0 回绕；**重复包必须重新 ACK 但绝不重复写数据**；
    `EOT → ACK → 空第 0 包 → ACK` 才算结束；超时重发的是「上一次的响应」而不是无脑 NAK。
  - 两处刻意宽容（为兼容不标准的 PC 端）：EOT 一律 ACK；结束阶段没收到空第 0 包就再收到 EOT 也认为结束。

- **新增 `services.ota`（V1.0）**：APP 外壳。`ota_app_early_init()` 设 VTOR + 开中断（只做这两件事，
  所以能当 `main()` 第一行）、`start/process` 非阻塞步进、`confirm()` 固化、
  `trial_check()+health_cb` 自动确认、`reboot()` 交回 BL。
  - ★ **VTOR 用编译期基址（`-DOTA_SELF_BASE`）而不是状态区的 `run_slot`**：「我在哪个槽」是链接期
    就确定的事实，而状态区**可能写失败**；两个槽是同一份源码的两次链接、向量表内容几乎一样，
    拿过期的 `run_slot` 设 VTOR **不会崩**，只会安静地跑错 handler —— 这正是最该避免的一类故障。
  - **回滚逻辑全在 BL**，APP 不需要知道「我跑了几次」「什么时候会被撤回」。

- **新增 `services.ota_src_uart`（V1.0）**：`uart_drv` + `ymodem` → `ota_source_t`。
  - **工程侧两个前提**（都写进了模块 readme）：
    ① `UART_DRV_BUF_SIZE` 必须 ≥ 一个 YMODEM 1K 帧（1029 字节），否则 DMA 覆盖还没被取走的字节，
       现象是「随机某包 CRC 错、重传几次又过」，极难查；该宏已加 `#ifndef` 守卫，
       用 `-DUART_DRV_BUF_SIZE=1088` 覆盖（F407 上每实例多占 ~1.7 KB RAM）。
    ② 单次 `read()` 阻塞上限 = `poll_timeout_ms`（默认 2000 ms）—— 流式源的固有属性，
       **看门狗超时必须大于它**。
  - 头包处用 `on_header`（在回 ACK **之前**）以 `ota_core_image_max()` 拦一次「明显放不下」。

- **`chip.oop_boot` 补 `oop_boot_system_reset()`**：让 services 层不必出现 CMSIS 符号。
- **验证**：3 个源 × F1/F4/G4 共 9 个组合 `-Wall -Wextra -Wpedantic` 零警告；
  YMODEM 端到端单测（7 组场景）在 PC 上用 host gcc 全过；`tools/sdk-check-oop.py --strict` 通过。
- 待建：`services.ota_flash_ext`（P5）、`services.ota_src_http`（P6）。
### 新增 chip.oop_boot + services.bootloader（2026-09-18，v0.10.0）

BL 外壳落地（P2）。方案文档：`ota-demo-stm32/doc/01~06`。

- **新增 `chip.oop_boot`（V1.0）**：启动跳转 —— 向量表校验 / 外设复位 / VTOR / MSP / 跳转。
  放 chip 层的原因是 HAL 红线：跳转 = 一组芯片内核操作（VTOR/MSP/NVIC/SysTick）+ `HAL_DeInit()`，
  只有 `chip/` 允许出现 HAL 符号；`services.bootloader` 只负责「决定跳到哪、凭什么跳」。
  两处顺序不能颠倒：① SP 合法性检查必须在 `__set_MSP()` **之前**做完（换栈后连普通 C 函数
  都不能调，故跳转实现为 naked 汇编函数，函数体不碰栈）；② 开中断放在换栈之后、`bx` 之前。
  向量表判据保守但零成本（读 8 字节）：SP 落 SRAM 区且 4 字节对齐、PC 的 Thumb 位为 1 ——
  能挡下空区（`0xFF...`）/ 未下载 / 写坏的槽。另注意 CMSIS 里**没有**
  `NVIC_ClearAllPendingIRQ()`，且 `HAL_SuspendTick()` 只关中断不停计数器，两者都要自己处理。

- **新增 `services.bootloader`（V1.0）**：BL 决策状态机
  `CFG → ACTIVATE → HEALTH → VERIFY → READY`（或 `RESCUE`），全程非阻塞步进
  （单步上限 = 一个擦除单位，或一次 `buf_len` 的读+写），BL 里仍能喂狗、刷进度、闪灯。
  - **职责边界**：BL 只判分区表自洽、目标槽向量表合法、刚下载完的镜像 CRC；
    「业务是否正常」只有 APP 知道，由 APP 自检后调 `ota_confirm()` 表达。在那之前新槽算
    「试运行」，`boot_try` 每上电 +1（**必须落盘**，否则每次复位都从 0 开始、回滚等于不存在），
    超过 `max_try` 清 `trial_slot` 跳回 `active_slot`。**回滚瞬时且零搬运** —— 旧槽从未被动过。
  - **已确认状态下正常开机零擦写**：`boot_health_apply()` 返回 0 表示「无需落盘」，
    `boot_jump()` 也只在 `run_slot` 变化时写。频繁复位不会把 CFG 扇区写死。
  - **SWITCH 与 MOVE 共用同一段代码**：差异只有 `pending_action` 与分区表拓扑。
    MOVE 的断电安全靠两条 ——「暂存区在搬完之前是只读源」+「按擦除单位搬、先写数据后写进度、
    重启后从进度所在单位起点重搬」，因此**天然幂等**；明确否决「运行区↔暂存区就地交换」。
  - **安全闸**：`boot_init()` 把 `chip.oop_flash` 的安全闸收窄成
    `[BOOT 区末尾, 内部 Flash 末尾)`，即使地址算错也擦不到 BL 自己；分区表未登记 BOOT 区则拒绝启动。

- **验证**：4 个源文件 × F1/F4/G4 共 12 个组合 `-Wall -Wextra -Wpedantic` 零警告；
  `boot_health` 是**纯逻辑**（只依赖 `ota_cfg.h` 的数据结构、无 HAL/chip 依赖），
  在 PC 上用 host gcc 真跑通 22 项断言（含 `max_try` 边界、回滚后稳态零写、`active_slot` 越界兜底）。
- `tools/sdk-check-oop.py --strict` 通过。
- 待建：`protocols.ymodem` + `services.ota` + `services.ota_src_uart`（P3）。
### 新增 services.ota_core + services.ota_src_mem + tools/fw-ota-pack.py（2026-09-18，v0.9.0）

OTA 共用底座落地（P1）。方案文档：`ota-demo-stm32/doc/01~06`。

- **新增 `services.ota_core`（V1.0）**：BL 与 APP 的**共同底座** —— 分区表 schema 与目标区选择、
  状态区双份乒乓读写、`.otapkg` 80 字节包头解析与校验器接口、介质抽象、非阻塞步进式流程骨架。
  三个关键设计：
  1. **「A/B 切换」与「单槽 + 暂存搬运」不是两套代码** —— 差异只在「RUN 区有几条」（分区表数据）
     与「`pending_action` 取什么值」（状态区一个字段）；于是 APP 侧两种拓扑完全共用，
     BL 侧只在 `boot_activate` 里分两个分支。这就是「兼容 + 可选」的落地点。
  2. **第 2 关（内存累计 CRC）与第 3 关（读回 Flash 重算 CRC）分开做**。
     增量 CRC 只证明「收到的报文对」，读回 CRC 才证明「落到 Flash 的字节对」——最易漏的设计点。
  3. **非阻塞步进式流程**：每个 `ota_flow_step()` 只读一块 / 擦一个擦除单位 / 校验一块，
     448 KB 的长擦除被切成多步，步与步之间应用能喂狗、刷进度、响应取消；
     比「一次长阻塞 + 异步状态机」两条路都简单。
  另外：搬运缓冲由调用方提供（不动态分配）；介质偏移而非 CPU 地址（外挂 Flash 不在地址空间）；
  `ota_cfg_init()` 会校验「CFG 半区是擦除单位整数倍」，否则擦第二份会连带擦掉第一份。
- **新增 `services.ota_src_mem`（V1.0）**：内存取数后端。让 OTA 逻辑在没有任何传输协议栈时
  就能跑通「写 → 双重校验 → 提交」，避免后面把 YMODEM 的问题和 OTA 逻辑的问题混在一起查。
  `seekable=1/0` 分别覆盖 HTTP/TF 卡（可跳段）与串口流式（不可回退）两条分支。
- **新增 `tools/fw-ota-pack.py`**：bin → `.otapkg`（双段包 / 单段包、`zlib.crc32`、版本注入），
  **产物写完会自己重新打开复算 CRC**；`--list` 可查看并校验已有包。
  `manual/sdk_run.py` 加 `pack` 子命令桥接，工程侧 `python sdk_run.py pack ...` 即可用。
- 验证：8 个源文件在 F4 下 `-Wall -Wextra -Wpedantic` **零警告**；
  `fw-ota-pack.py` 打包往返 **22 项头部字节布局核对全过**（含段数据 4 字节对齐填充）。
- 待建：`services.bootloader`（P2）、`protocols.ymodem` + `services.ota` + `services.ota_src_uart`（P3）。

### 新增 services 层 + 两个 flash 模块（2026-09-18，v0.8.0）

为 ota-demo-stm32 的 OTA 方案铺路（方案文档：`ota-demo-stm32/doc/01~06`）。

- **新增 `chip.oop_flash`（V1.0）**：内部 Flash 驱动，`HAL_FLASH_*` 唯一入口。
  扇区几何按系列内置 —— F4 变长扇区（16K/64K/128K，实际扇区数由 `FLASH_SIZE` 截断）、
  F1 等长页（≤128 KB 器件 1 KB / >128 KB 器件 2 KB）、G4 等长页 2 KB（含双 Bank 页号）。
  `oop_flash_erase()` 自动按扇区展开，上层不必知道粒度；写入单位 F1=2B / F4=4B / G4=8B，
  首尾非对齐由内部读-改-写补齐。`oop_flash_set_guard()` 可收紧允许擦写的窗口，
  防止上层地址算错把 Bootloader 自己擦掉。**三系列各编过、零警告**；
  双 Bank F4（F427/F437/F429/F439/F469/F479）编译期 `#error`（HAL 对扇号 > 11 的 SNB 偏移规则未实现），
  G4 分支未上板验证。
- **新增 `devices.spi_nor_flash`（V1.0）**：通用 SPI NOR（W25Q/GD25Q/BY25Q/EON 同族）。
  坐 `chip.oop_spi`/`oop_gpio`/`oop_dwt` 之上，零 HAL 直调。0x9F 读 JEDEC ID 定容量，
  0x0B FAST_READ；页编程自动按页切分 + WREN + 回读 WEL 校验；擦除自动选 64 KB 块擦 / 4 KB 扇区擦。
  设计取舍：**不做异步状态机**，改为「单位操作阻塞 + `spi_nor_set_idle_cb()` 空闲回调」——
  等 WIP 期间反复回调（喂狗 / 进度 / 返回 false 可中止），由上层决定一次喂多少字节。
- **`tools/sdk-check-oop.py`**：扫描范围加入 `library/services`（新层同样禁止直调 HAL）；
  `allow_hal` 判定改为按目录名 `endswith("/chip")`，避免以后加层时漏改映射表。
- 待建：`services.ota_core`（BL/APP 共用底座）、`services.bootloader`、`services.ota`、
  `protocols.ymodem`、`tools/fw-ota-pack.py` —— 清单见 `todo.md`。

### 忽略项默认模板（2026-09-18）— `.gitignore` 两份 + manual 脚手架补充

- 新增 `manual/.gitignore`：随脚手架拷到工程根即生效的**默认忽略模板**。屏蔽 `.claude/` `.codegraph/` `.workbuddy/` `.cache/`、构建产物（`build/` `Build/` `Debug/` `Release/` + `*.o/*.d/*.elf/*.bin/*.hex/*.map/*.lst`）、Keil MDK-ARM（`MDK-ARM/` `RTE/` `*.uvprojx` `*.uvoptx` `*.axf` `*.crf` `*.dep` `*.htm` `JLinkLog.txt`…）、其他 IDE（`.settings/` `.metadata/`）、CMake 误 in-source 构建兜底、备份/临时副本（含 `*.ioc.broken`）。
- 仓库根 `.gitignore` 同步补齐为同一套规则（原先只有 `.inbox` / `.workbuddy` / `todo.md`）。
- 模板内含**「禁止忽略」反面清单**：`MySDK/`、`User/`、`*.ioc`/`.mxproject`、`sdk_run.py`、`.vscode/`、`Core/`、`*.md` —— 看着像产物但缺失会导致别人 clone 后编译不过或无法复现，防止后来人「顺手清理」。
- **踩坑并已修**：gitignore **不支持行尾注释**，`.cache/  # 缓存` 会被当成模式本身。初版三条带行尾注释的规则（`.cache/`、`.vscode/*.log`、`*.ioc.broken`）静默失效，已全部改为注释独立成行；两个文件都写明了这一坑。
- 校验方式：临时仓 `git check-ignore --no-index` 全量跑，42 项应忽略零漏、25 项必入库零误伤。
- 未动 `sdk_manifest.json` 与 SDK 版本号：这两份文件不属于 `MySDK/` 拉取内容，接入工程不会因此产生 diff。

### 新增 protocols.cli（2026-09-18）— 串口命令行核心（命令注册 + 行解析）

- 新增 `library/protocols/cli/`（`Inc/cli_core.h` + `Src/cli_core.c` + `readme.md`，V1.0）：依赖仅 `chip.oop_dwt`（去抖窗口与限速上报的时基），不碰 HAL/OS，也不绑定任何具体串口。
- 来源：ir-demo-stm32 工程的 `User/Src/uart_cli.c`。那里「6 条命令的 if-else 链」只是数据，而「防回显自激四道闸」是通用知识，不该每个工程重新踩一遍。
- 有意改掉旧实现的粗糙处（非兼容性问题）：参数改用空白分隔（`txcar 38000`，旧实现是删光空白的 `TXCAR38000`，且 `T X` 会被规范化成 `TX` 命中命令）；仅命令名大小写不敏感、参数原样保留；数字解析加溢出检查。
- 接口：`cli_init` / `cli_register`（可多张表累加）/ `cli_process`（逐字节拉）/ `cli_feed`（整包推，配 `chip.oop_uart` 用）/ `cli_printf` / `cli_print_help` / `cli_parse_u32_range`；handler 返回 `CLI_RET_BAD_ARG` 时核心自动代打 usage。
- 首批用户回灌验证：ir-demo-stm32 的 6 条命令（status / txpol / txcar / txn / txmod / tx）改为注册式。
- SDK 版本 0.6.0 → 0.7.0。

### IR 收发升 V2.0（2026-09-18）— 取工程侧稳定方案：输入捕获 + CCR 门控

- `ir_receiver` V1.2 → **V2.0**：时间戳由「GPIO EXTI + DWT 软件计时」改为 **TIM 输入捕获**（硬件在跳变时刻锁存 CCR，抖动为 0）；F1 通用定时器无硬件双沿，故在 ISR 内翻转 `CCxP` 用软件模拟双沿；时间差按 `ARR+1` 做环形减法补偿 16 位回绕；200µs 去抖滤掉 AGC 饱和产生的亚载波毛刺；帧结束改由 `ir_receiver_process()` 轮询判定（不再需要 1ms TIM 中断收尾）。顺带删掉不可达的 `TIMEOUT` 状态与 `mute` 路径（死代码）。
- `ir_transmitter` V1.0 → **V2.0**：载波由「每段 Start/Stop PWM」改为 **常开 + CCR 门控**（`OCxPE=0` 下单次寄存器写即生效，避免最坏一个载波周期 26µs 的抖动）；µs 时基由 DWT 改为**注入的自由运行 TIM**，并加「init 探测时基是否真在计数」+「等待循环时基冻结保护」（判废并置 `ready=false`，绝不挂死主循环）。
- 两模块公开 API 从**全局单例**改为**多实例**：`ir_receiver_init/start/stop/process/isr`、`ir_transmitter_init/send/mark/space/set_*`，句柄/通道/时钟/缓冲区全部注入；依赖收敛为 `chip.platform + chip.oop_tim`（不再需要 `oop_gpio`/`oop_dwt`/`SEGGER_RTT`）。
- 配套：`chip.oop_tim` V1.1 增补输入捕获（`ic_init/start_it/stop_it/read_capture/toggle_polarity`）、计数器/周期读写与更新事件、CCR 直写/极性/关预装载原语；`chip.platform` V1.1 补上 **STM32F1 分支**（此前只有 F4/G4，F103 工程编译不过）。SDK 版本 0.5.1 → 0.6.0。

### IR 模块改名（2026-09-18）— ir_1838b → ir_receiver、ir_tx → ir_transmitter

- `devices/ir_1838b` → `devices/ir_receiver`：1838B 只是「解调输出型接收头」的一个型号，模块本身不绑定芯片，故改用功能名；符号 `IR1838B_*` → `IR_Receiver_*`，头文件名与守卫同步。
- `devices/ir_tx` → `devices/ir_transmitter`：`tx` 过短且与 `rx` 不成对，改为与 `ir_receiver` 对称的全名；符号 `IR_TX_*` → `IR_Transmitter_*`，类型 `ir_tx_cfg_t` → `ir_transmitter_cfg_t`。
- 同步更新：manifest id/path/files、`manual/User/sdk.toml`、`manual/User/board_cfg.h`（`BOARD_IR_TX_CFG` → `BOARD_IR_TRANSMITTER_CFG`）、`tools/sdk-pull.py` 模板、`chip.oop_tim` 与 `protocols.ac_codec` 的注释引用。
- 顺带修正 manifest 里 `ir_receiver` 的描述：原写「基于定时器输入捕获」与实现不符，实为 **GPIO 双沿中断 + DWT 打时间戳**（1ms TIM 仅用于静默收尾）。

### 新增 devices.ws1850s（2026-09-18）— RFID 读卡器异步驱动

- 新增 `devices/ws1850s`（V1.0）：WS1850S RFID 读卡器（MFRC522/RC522 国产兼容，UART 二进制帧协议）。按原工程 `rfid-driver` 的**异步版**逻辑迁入（另一版为阻塞忙等，未采用）：`uart_drv_t*` 注入 + `ws1850s_start()` 初始化状态机 + `ws1850s_process()` 拉取收包，结果/卡片/状态经回调上抛，全程不阻塞，与 `devices.hlk_rm58s` 同构。

### 仓库结构三层化（2026-08-29）

- `library/` 收编四层固件源码（按 `sdk.toml` 自动拉取）；`tools/` 合并为 host 工具单源（只调用不拉取）；`manual/` 新增手动脚手架（`sdk_run.py` + `.vscode` + `User` 模板）。SDK 位置只在 `sdk.toml` 一处配置。

### v0.3.0 (2026-08-29) — devices 层零直调 HAL + 任务接口统一

- 新增 `oop_tim` / `oop_iwdg` / `oop_dwt(oop_GetTickMS)`；devices 全部经 OOP 操作（`ir_transmitter`/`ir_receiver`/`heart_beat`/`lan8720a`/`hlk_rm58s`），`dht11` 走原始原语。device init 收口到 `Task_X_Init()`。

### v0.2.0 (2026-08-28) — bsp_* 全面更名 oop_* + 依赖规范化

- chip 层 `bsp_*`→`oop_*`（目录/文件/符号/宏/守卫全同步），`bsp_audit`→`sdk-check-oop`。cJSON 归位 `middleware`，`mqtt` 补 cJSON 依赖。

### v0.1.1 (2026-08-28) — 修复 OOP 封装泄漏 HAL

- 全库剔除工程头/具体句柄/MX 引脚宏，改由各模块 `init` 注入（`uart`/`ir_receiver`/`ir_transmitter`/`heart_beat`/`lan8720a`/`dht11`/`wol`/`oop_dwt`）。新增 `sdk-check-oop.py` 回归防护。

### v0.1.0 (2026-08-28) — 初始版本

- 四层结构 + `hal_platform` 移植层；收录模块见「版本」章节清单；新增 `sdk_manifest.json` (schema 1.0)。
