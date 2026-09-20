#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
sdk_run.py —— 工程侧接口桥（单文件，零 SDK 逻辑）

从工程 User/sdk.toml 的 [sdk] 读 SDK 根，再分发到 <SDK根>/tools/<tool>。
这样 **SDK 位置只在 sdk.toml 一处配置**；.vscode/tasks.json 永远不含 SDK 路径/相对深度。

被 .vscode/tasks.json 调用，例如:
    python sdk_run.py flash [JLinkRoot] [ELF] [Device] [Interface] [Speed] [ProjRoot]
    #   三分片工程：ELF 传 build/TP_MDC{,_A,_B}.elf 即可 —— 地址由「与 .elf 同目录的
    #   同名 .ld」解析（CMake 生成到 .bin 同目录），无需 --slot、更不写死偏移
    python sdk_run.py flash --slot A          # （向后兼容）按基版 .ld 派生槽几何再烧
    python sdk_run.py pull
    python sdk_run.py audit
    python sdk_run.py trans <file> [...]
    python sdk_run.py pack --slot-a A.bin --slot-b B.bin --ver 1.2.3 -o dist/app.otapkg
    python sdk_run.py ymodem [--port COMx] [--baud 115200] [--gui] build/ota_A/TP_MDC_A.bin

说明: 本文件是「接口」不是「工具副本」——它不含任何 SDK 逻辑，只做
「读配置 -> 调 SDK 工具」。工程从 SDK manual/ 拷贝到工程根即可。

工具文件名（2026-09-20 统一）:
    flash      -> tools/fw-flash.py
    pull       -> tools/sdk-pull.py
    audit      -> tools/sdk-check-oop.py + tools/check-eol.py
                  （审计 = ①OOP/HAL 红线 ②行尾体检，两道都要过）
    trans      -> tools/format-gbk2utf8.py
    pack       -> tools/fw-ota-pack.py   （仅脚本保留，无 VSC 任务；raw-bin 流程下一般不再需要）
    ymodem     -> tools/fw-ota-ymodem.py （直接发 raw .bin，设备侧自动选槽）
"""
import os
import sys
import subprocess
import tomllib

PROJ = os.path.dirname(os.path.abspath(__file__))


def sdk_root() -> str:
    toml = os.path.join(PROJ, "User", "sdk.toml")
    if not os.path.isfile(toml):
        sys.exit("[sdk_run] 找不到 %s" % toml)
    with open(toml, "rb") as f:
        cfg = tomllib.load(f)
    sdk = (cfg.get("sdk") or "").strip().strip('"')
    if not sdk or not os.path.isdir(sdk):
        sys.exit("[sdk_run] sdk.toml 的 sdk 路径无效: %r" % sdk)
    return sdk


def main() -> int:
    if len(sys.argv) < 2:
        sys.exit("用法: sdk_run.py <flash|pull|audit|trans|pack|ymodem> [...]")
    try:
        # 任务面板里 SDK 侧日志要先于子进程输出出现（否则被缓冲顺序打乱）
        sys.stdout.reconfigure(line_buffering=True)
    except (AttributeError, ValueError):
        pass
    task = sys.argv[1]
    rest = sys.argv[2:]
    sdk = sdk_root()
    tool_dir = os.path.join(sdk, "tools")

    if task == "flash":
        # 只调 py 版：它能自检 J-Link 安装目录与 .ioc 器件名（规范化后交 J-Link 校验），
        # 并把检测结果写回 settings.json 供 cortex-debug 用 → 烧录/调试都不写死路径。
        # 位置参数 [JLROOT ELF DEV ITF SPEED PROJ]（空串=自动检测）；
        # 地址解析顺序：--ld/--slot > 「与 .elf 同目录的同名 .ld」 > 工程根基版 .ld，
        # 全部从 .ld 的 FLASH ORIGIN 取，**禁止写死偏移/地址**。
        # 另支持 --dry-run / --settings-only / --no-write-settings，见 fw-flash.py --help。
        py = os.path.join(tool_dir, "fw-flash.py")
        if not os.path.isfile(py):
            sys.exit("[sdk_run] 找不到 %s —— SDK 检出过旧（flash.bat 已废弃并删除），"
                     "请更新 SDK 检出" % py)
        print("[sdk_run] flash ->", py)
        return subprocess.run([sys.executable, py, *rest]).returncode

    if task == "pull":
        script = os.path.join(tool_dir, "sdk-pull.py")
        toml = os.path.join(PROJ, "User", "sdk.toml")
        print("[sdk_run] pull ->", script)
        return subprocess.run([sys.executable, script, toml]).returncode

    if task == "audit":
        # 两道体检，任一不过即非零退出：
        #   ① sdk-check-oop.py —— library/ 是否直调 HAL / 引用工程句柄（板无关红线）
        #   ② check-eol.py     —— 行尾体检，防「CR 加倍」脏字节进仓库（见该文件头部说明）
        rc = 0
        for name in ("sdk-check-oop.py", "check-eol.py"):
            script = os.path.join(tool_dir, name)
            if not os.path.isfile(script):
                print("[sdk_run] 跳过（工具缺失）:", script)
                continue
            print("[sdk_run] audit ->", script)
            rc |= subprocess.run([sys.executable, script]).returncode
        return rc

    if task == "trans":
        script = os.path.join(tool_dir, "format-gbk2utf8.py")
        print("[sdk_run] trans ->", script, rest)
        return subprocess.run([sys.executable, script, *rest]).returncode

    if task == "pack":
        # OTA 固件打包（bin -> .otapkg）；产物路径由调用方用 -o 指定。
        # raw-bin 流程下一般不再需要此步（ymodem 直接发 .bin），脚本保留以备外挂 flash 暂存等场景。
        script = os.path.join(tool_dir, "fw-ota-pack.py")
        print("[sdk_run] pack ->", script)
        return subprocess.run([sys.executable, script, *rest]).returncode

    if task == "ymodem":
        # OTA 主机端：经 YMODEM 把 **raw .bin** 发给设备（配合 services.ota_src_uart）。
        # 设备侧自动选择非运行槽写入；不要发 .otapkg（自创格式，非行业通用）。
        script = os.path.join(tool_dir, "fw-ota-ymodem.py")
        print("[sdk_run] ymodem ->", script)
        return subprocess.run([sys.executable, script, *rest]).returncode

    sys.exit("[sdk_run] 未知任务: %s" % task)


if __name__ == "__main__":
    sys.exit(main())
