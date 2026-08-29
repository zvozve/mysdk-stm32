# tools/ —— SDK 工具集（单源，只调用、不拉取）

本目录存放**跑在开发 PC 上、不进固件、只在 SDK 内单源**的脚本与工具。
工程侧通过 `manual/` 提供的 `sdk_run.py` 桥接，再经 `.vscode` 任务调用，
**工程内不保留任何工具副本**（详见 SDK 根 `readme.md` 的「工程接入指南」）。

## 分类

### 1) SDK 维护工具（仅 SDK 维护者使用）
| 文件 | 作用 |
|---|---|
| `sync_lib.py` | 按工程 `sdk.toml` 把 `library/` 子集镜像到工程 `MySDK/`（单源真相） |
| `oop_audit.py` | 检查 `library/` 是否直调 HAL / 引用工程句柄（板无关校验） |

### 2) 工程面向工具（开发者经 F8/F7 等任务调用）
| 文件 | 作用 |
|---|---|
| `flash.bat` | STM32 J-Link 烧录（自动读 `.ld` 判断 OTA / 普通模式、读 `.ioc` 识别芯片） |
| `trans_gbk2utf-8.py` | GBK/GB2312 源码批量转 UTF-8 |

## 调用链（关键：SDK 位置只在 `sdk.toml` 一处配置）
```
工程 .vscode/tasks.json
   └─> python <工程根>/sdk_run.py <flash|pull|audit|trans> ...
          └─> 读 User/sdk.toml 的 [sdk] 得到 SDK 根
                └─> 调用 <SDK根>/tools/<对应工具>
```
`tasks.json` 与 `sdk_run.py` **都不硬编码 SDK 绝对路径/相对深度**；
换 SDK 目录只需改 `sdk.toml` 的 `sdk = "..."` 一行。
