#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
sdk_run.py —— 工程侧接口桥（单文件，零 SDK 逻辑）

从工程 User/sdk.toml 的 [sdk] 读 SDK 根，再分发到 <SDK根>/tools/<tool>。
这样 **SDK 位置只在 sdk.toml 一处配置**；.vscode/tasks.json 永远不含 SDK 路径/相对深度。

被 .vscode/tasks.json 调用，例如:
    python sdk_run.py flash [JLinkRoot] [ELF] [Device] [Interface] [Speed] [ProjRoot]
    python sdk_run.py pull
    python sdk_run.py audit
    python sdk_run.py trans <file> [...]

说明: 本文件是「接口」不是「工具副本」——它不含任何 SDK 逻辑，只做
「读配置 -> 调 SDK 工具」。工程从 SDK manual/ 拷贝到工程根即可。
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
        sys.exit("用法: sdk_run.py <flash|pull|audit|trans> [...]")
    task = sys.argv[1]
    rest = sys.argv[2:]
    sdk = sdk_root()
    tool_dir = os.path.join(sdk, "tools")

    if task == "flash":
        bat = os.path.join(tool_dir, "flash.bat")
        # flash.bat 期望 [JLROOT ELF DEV ITF SPEED PROJ]；PROJ 缺省=工程根
        args = list(rest)
        if len(args) < 6:
            args.append(PROJ)
        cmd = '"%s" %s' % (bat, " ".join('"%s"' % a for a in args))
        print("[sdk_run] flash ->", bat)
        return subprocess.run(cmd, shell=True).returncode

    if task == "pull":
        script = os.path.join(tool_dir, "sync_lib.py")
        toml = os.path.join(PROJ, "User", "sdk.toml")
        print("[sdk_run] pull ->", script)
        return subprocess.run([sys.executable, script, toml]).returncode

    if task == "audit":
        script = os.path.join(tool_dir, "oop_audit.py")
        print("[sdk_run] audit ->", script)
        return subprocess.run([sys.executable, script]).returncode

    if task == "trans":
        script = os.path.join(tool_dir, "trans_gbk2utf-8.py")
        print("[sdk_run] trans ->", script, rest)
        return subprocess.run([sys.executable, script, *rest]).returncode

    sys.exit("[sdk_run] 未知任务: %s" % task)


if __name__ == "__main__":
    sys.exit(main())
