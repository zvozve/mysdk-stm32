# tools/ —— SDK host 工具集（单源，也被 sdk-pull 按配置拉进工程）

本目录存放**跑在开发 PC 上、不进固件**的脚本与工具。它们以本仓库为单源真相；
工程侧可以选择性地把其中一部分**拉取**到工程里（`sdk.toml` 的 `[tools]` 段），
拉取后随工程一起入库 —— 这样即使手上没有 SDK 检出，Flash / OTA / 行尾体检也能用。

工程侧通过 `manual/` 提供的 `sdk_run.py` 桥接，再经 `.vscode` 任务调用。
`sdk_run.py` 的工具源查找顺序：

```
1. <dest>/<tools_dir>    sdk-pull 拉进工程的那份（默认 MySDK/tools，入库、随固件版本锁定）
2. <工程根>/tools         工程自己放的工具（可选）
3. <sdk>/tools            SDK 单源（仅 sdk_run.py pull 强制走这里）
```

## 分类（manifest 的 `tools[].audience`）

### 1) SDK 维护工具（`audience: maintainer`，一般不拉进工程）
| 文件 | 作用 |
|---|---|
| `sdk-pull.py` | 按工程 `sdk.toml` 把 `library/` 子集与选中的 host 工具镜像到工程（单源真相）。**必须由 SDK 侧副本执行**——它要在 SDK 里读 manifest 与 `library/` |
| `_fw_ota_ymodem_loopback_test.py` | YMODEM 收发双引擎端到端单测（host gcc 内存通道，7 组场景） |

### 2) 工程面向工具（`audience: project`，可拉取）
| 文件 | 作用 |
|---|---|
| `fw-flash.py` | STM32 J-Link 烧录：自检 J-Link 安装目录（参数>环境变量>注册表/扫描/PATH 里取版本最高）、从 `.ioc` 的 `Mcu.CPN` 规范化出 J-Link 器件名、读 `.ld` 判断 OTA/普通模式，并把检测结果写回工程 `.vscode/settings.json` 供 cortex-debug 用。带 `--slot A\|B`（自动选工程内 `*slotA*.ld`/`*slotB*.ld`，地址从 .ld 解析，禁写死）、`--dry-run` / `--settings-only` / `--no-write-settings` / `--color auto\|always\|never`。**零 SDK 路径依赖，可单文件带走** |
| `fw-ota-ymodem.py` | OTA 主机端：经 YMODEM 把 **raw .bin** 发给设备（设备侧自动选非运行槽写入）。`--gui` 弹文件框，`--port/--baud` 指定串口，`--trigger` 指定触发字节（默认 'U'） |
| `fw-ota-pack.py` | 可选：把 bin 打成 `.otapkg` 包供 HTTP 等需「侧信道元数据」的源用；raw-bin（YMODEM）流程下一般不再需要 |
| `format-gbk2utf8.py` | GBK/GB2312 源码批量转 UTF-8 |
| `format-utf16be2utf8.py` | 与 `format-gbk2utf8.py` 互补：UTF-16(LE/BE) / UTF-8-BOM 文本转 UTF-8 无 BOM |
| `gen-cmake-paths.py` | 扫描工程目录生成 CMake 源/包含路径片段（面向手工维护 `User/`、未走 `sdk-pull` 接线的工程） |
| `check-eol.py` | 行尾体检：抓「行尾 CR 加倍 / 孤立 CR」；另报混用行尾与索引侧 `-text`。支持位置参数指定根目录 |
| `sdk-check-oop.py` | HAL 泄漏红线检查。支持 `--root` 与布局自适应（SDK 源仓 `library/<layer>` 或工程镜像 `<layer>` 直挂），无 SDK 时改审工程 `MySDK/` |

> `fw-flash.py` 的位置参数与旧 `flash.bat` 完全一致 `[JLROOT ELF DEV ITF SPEED PROJ]`（空串=自动检测），
> 所以 `.vscode/tasks.json` **不必改**。`flash.bat` 已于 2026-09-18 删除——它在 `.ioc` 里抠的是
> `Mcu.UserName`（如 `STM32F407ZGTx`），J-Link 器件表里没有这种带 `Tx` 后缀的名字，不显式传
> Device 就烧不进去。

## 调用链（关键：SDK 位置只在 `sdk.toml` 一处配置）

```
工程 .vscode/tasks.json
   └─> python <工程根>/sdk_run.py <flash|pull|audit|trans|trans16|gencmake|ymodem|pack|tools> ...
          └─> 读 User/sdk.toml（sdk / dest / tools_dir / [tools]）
                ├─> 优先用 <dest>/tools 里已拉取的副本（无 SDK 也能跑）
                └─> 缺工具时才回退到 <SDK根>/tools
```

`tasks.json` 与 `sdk_run.py` **都不硬编码 SDK 绝对路径/相对深度**；换 SDK 目录只需改
`sdk.toml` 的 `sdk = "..."` 一行。自查用 `python sdk_run.py tools`：列出 SDK 路径是否有效、
本地工具是否已拉取、每个工具当前从哪来。
