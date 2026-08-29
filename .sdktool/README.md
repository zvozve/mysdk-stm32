# my-stm32-sdk/.sdktool — 共享工具集（单源）

> 本目录是 SDK 的 **共享工具单源**，存放与具体工程无关、可跨工程复用的主机端工具（烧录、调试接口等）。
> **不随 `sync_lib.py` 拉取** —— `sync_lib.py` 只镜像 `chip` / `devices` / `protocols` / `middleware` 源码与根 `CMakeLists.txt`。
> 消费工程通过 `.vscode` 任务以 **相对路径** 调用这里的脚本；工程侧只保留「接口」（任务定义），不保留工具副本，避免双份漂移。

## 目录

```
.sdktool/
├── flash.bat          # F8 烧录：自动识别芯片(.ioc) / FLASH 地址(.ld) / .bin / JLink
├── README.md          # 本文件
└── .vscode/           # 规范接口模板（手动拷到工程 .vscode，勿自动覆盖）
    ├── tasks.json       # Build(F7) / Flash(F8) / Build&Flash
    ├── launch.json      # Build&Debug / Debug (cortex-debug + JLink)
    └── keybindings.json # f8 → Flash
```

## flash.bat

```
flash.bat [JLinkRoot] [ELF] [Device] [Interface] [Speed] [ProjRoot]
```

- 自动从 `ProjRoot/*.ld` 读取 FLASH ORIGIN，判断 OTA / 普通模式
- 自动从 `ProjRoot/*.ioc` 读取芯片型号（`Mcu=...`）
- 自动查找 `ProjRoot/build/{Release,Debug}/*.bin` 与 `JLink.exe`
- `ProjRoot` 缺省 = 当前工作目录（VSCode 任务默认 `cwd = ${workspaceFolder}`）

> v3.1 起不再依赖脚本自身位置（`%~dp0`），改用 `ProjRoot` 参数 + `%CD%` 兜底，
> 因此可从任意工程目录调用，是真正的「单源」工具。

## 工程侧如何对接（接口）

工程 `.vscode/tasks.json` 的 Flash 任务直接指向本脚本：

```json
{
  "label": "Flash",
  "type": "shell",
  "command": "${workspaceFolder}/../../../../Applications/Dev_STM32/my-stm32-sdk/.sdktool/flash.bat",
  "args": [
    "${config:jlink.root}",
    "${command:cmake.getLaunchTargetPath}",
    "${config:jlink.device}",
    "${config:jlink.interface}",
    "${config:jlink.speed}",
    "${workspaceFolder}"
  ],
  "group": { "kind": "build", "isDefault": true },
  "problemMatcher": []
}
```

> ⚠️ `command` 里的相对路径必须指向 **本 SDK 根** 的 `.sdktool/flash.bat`，
> 与 `sdk.toml` 的 `sdk` 字段一致。工程与 SDK 的相对层级变化时需同步调整。

## 键位映射（约定）

| 键位 | 机制 | 是否需要本工具 |
|---|---|---|
| **F7 编译** | `tasks.json` 的 `Build` 任务，`type: cmake`（CMake Tools 默认） | 否 |
| **F5 调试** | `launch.json`（cortex-debug + JLink），全 `${config:jlink.*}` 变量驱动 | 否（已通用） |
| **F8 烧录** | 键位 `f8 → 运行 "Flash" 任务` → 本 `flash.bat` | **是** |

## 设计原则

- 工具只在 SDK 单源维护，工程零副本（避免漂移）。
- 工程只持有「接口」——`.vscode` 任务定义，使其可经 VSCode 任务面板 / 快捷键选择调用。
- `settings.json` / `c_cpp_properties.json` 是工程本地资产，**不要**放进本目录或被覆盖。
