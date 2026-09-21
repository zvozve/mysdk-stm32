# tools/ —— SDK 工具集（单源，只调用、不拉取）

本目录存放**跑在开发 PC 上、不进固件、只在 SDK 内单源**的脚本与工具。
工程侧通过 `manual/` 提供的 `sdk_run.py` 桥接，再经 `.vscode` 任务调用，
**工程内不保留任何工具副本**（详见 SDK 根 `readme.md` 的「工程接入指南」）。

## 分类

### 1) SDK 维护工具（仅 SDK 维护者使用）
| 文件 | 作用 |
|---|---|
| `sdk-pull.py` | 按工程 `sdk.toml` 把 `library/` 子集镜像到工程 `MySDK/`（单源真相） |
| `sdk-check-oop.py` | 检查 `library/` 是否直调 HAL / 引用工程句柄（板无关校验） |
| `check-eol.py` | 行尾体检：抓「行尾 CR 加倍 / 孤立 CR」；另报混用行尾与索引侧 `-text`。防「保留换行读 + 默认模式写」导致每编辑一次多一个 CR（脏字节会绕过 autocrlf 直接进仓库） |


### 2) 工程面向工具（开发者经 F8/F7 等任务调用）
| 文件 | 作用 |
|---|---|
| `fw-flash.py` | STM32 J-Link 烧录：自检 J-Link 安装目录（参数>环境变量>注册表/扫描/PATH 里取版本最高）、从 `.ioc` 的 `Mcu.CPN` 规范化出 J-Link 器件名、读 `.ld` 判断 OTA/普通模式，并把检测结果写回工程 `.vscode/settings.json` 供 cortex-debug 用。带 `--slot A\|B`（自动选工程内 `*slotA*.ld`/`*slotB*.ld`，地址从 .ld 解析，禁写死）、`--dry-run` / `--settings-only` / `--no-write-settings` / `--color auto\|always\|never`（沿用旧 flash.bat 的彩色提示；输出重定向时自动无色） |
| `format-gbk2utf8.py` | GBK/GB2312 源码批量转 UTF-8 |
| `format-utf16be2utf8.py` | 与 `format-gbk2utf8.py` 互补：UTF-16(LE/BE) / UTF-8-BOM 文本转 UTF-8 无 BOM（编辑器「Unicode / Unicode big endian」保存导致的乱码归一化）；`--recursive` 递归常见源码后缀 |
| `gen-cmake-paths.py` | 扫描工程目录生成 CMake 源/包含路径片段（`file(GLOB_RECURSE ...)` + `target_sources` + `target_include_directories`），面向手工维护 `User/` 源码、未走 `sdk-pull` 接线的工程；`[ROOT]` 默认 `.`，`--output FILE` 写片段，`--target NAME` 指定目标名 |
| `fw-ota-ymodem.py` | OTA 主机端：经 YMODEM 把 **raw .bin** 发给设备（设备侧自动选非运行槽写入）。`--gui` 弹文件框，`--port/--baud` 指定串口，`--trigger` 指定触发字节（默认 'U'） |
| `fw-ota-pack.py` | 可选：把 bin 打成 `.otapkg` 包供 HTTP 等需「侧信道元数据」的源用；raw-bin（YMODEM）流程下一般不再需要 |

> `fw-flash.py` 的位置参数与旧 `flash.bat` 完全一致 `[JLROOT ELF DEV ITF SPEED PROJ]`（空串=自动检测），
> 所以 `.vscode/tasks.json` **不必改**。`flash.bat` 已于 2026-09-18 删除——它在 `.ioc` 里抠的是
> `Mcu.UserName`（如 `STM32F407ZGTx`），J-Link 器件表里没有这种带 `Tx` 后缀的名字，不显式传
> Device 就烧不进去。

## 调用链（关键：SDK 位置只在 `sdk.toml` 一处配置）
```
工程 .vscode/tasks.json
   └─> python <工程根>/sdk_run.py <flash|pull|audit|trans|trans16|gencmake|ymodem|pack> ...
          └─> 读 User/sdk.toml 的 [sdk] 得到 SDK 根
                └─> 调用 <SDK根>/tools/<对应工具>
```
`tasks.json` 与 `sdk_run.py` **都不硬编码 SDK 绝对路径/相对深度**；
换 SDK 目录只需改 `sdk.toml` 的 `sdk = "..."` 一行。