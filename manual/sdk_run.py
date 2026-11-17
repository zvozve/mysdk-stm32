#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
sdk_run.py —— 工程侧接口桥（单文件，零 SDK 逻辑）

从工程 User/sdk.toml 读配置，再分发到「工程自带的工具」或「SDK 检出里的工具」。
这样 **SDK 位置只在 sdk.toml 一处配置**；.vscode/tasks.json 永远不含 SDK 路径/相对深度。

工具源查找顺序（先本地、后 SDK）：
    1. <dest>/<tools_dir>    sdk-pull 拉进工程的那份（默认 MySDK/tools，随工程入库）
    2. <工程根>/tools        工程自己放的工具（可选）
    3. <sdk>/tools           SDK 检出单源（仅 pull 强制走这里）
为什么要先本地：工程里的副本随固件版本一起被锁定、一起提交，行为可复现；
且 SDK 目录搬走/删掉后，Flash、YMODEM、行尾体检仍然可用。

被 .vscode/tasks.json 调用，例如:
    python sdk_run.py flash [JLinkRoot] [ELF] [Device] [Interface] [Speed] [ProjRoot]
    #   三分片工程：ELF 传 build/TP_MDC{,_A,_B}.elf 即可 —— 地址由「与 .elf 同目录的
    #   同名 .ld」解析（CMake 生成到 .bin 同目录），无需 --slot、更不写死偏移
    python sdk_run.py flash --slot A          # （向后兼容）按基版 .ld 派生槽几何再烧
    python sdk_run.py pull [--only tools]     # 拉库子集 + host 工具（强依赖 SDK 检出）
    python sdk_run.py audit
    python sdk_run.py trans <file> [...]
    python sdk_run.py pack --slot-a A.bin --slot-b B.bin --ver 1.2.3 -o dist/app.otapkg
    python sdk_run.py ymodem [--port COMx] [--gui]   # 不给文件=全自动：设备自报目标槽 ->
                                                     #   在工程根自动找对应分片；--gui 仅兜底
    python sdk_run.py tools                   # 体检：工具源、逐个工具在不在、SDK 版本戳

说明: 本文件是「接口」不是「工具副本」——它不含任何 SDK 逻辑，只做
「读配置 -> 找工具 -> 调工具」。工程从 SDK manual/ 拷贝到工程根即可。

工具文件名（2026-09-20 统一，manifest tools 段为真相源）:
    flash      -> fw-flash.py
    pull       -> sdk-pull.py
    audit      -> sdk-check-oop.py + check-eol.py
                  （审计 = ①OOP/HAL 红线 ②行尾体检，两道都要过）
    trans      -> format-gbk2utf8.py
    trans16    -> format-utf16be2utf8.py  （与 trans 互补：UTF-16 LE/BE / UTF-8-BOM -> UTF-8 无 BOM）
    gencmake   -> gen-cmake-paths.py     （扫描工程目录生成 CMake 源/包含路径片段）
    pack       -> fw-ota-pack.py   （仅脚本保留，无 VSC 任务；raw-bin 流程下一般不再需要）
    ymodem     -> fw-ota-ymodem.py （直接发 raw .bin，设备侧自动选槽）
"""
import os
import sys
import subprocess

try:
    import tomllib
except ImportError:  # pragma: no cover
    sys.exit("需要 Python >= 3.11（tomllib）")

PROJ = os.path.dirname(os.path.abspath(__file__))
TOML = os.path.join(PROJ, "User", "sdk.toml")

# 子命令 -> 工具文件名（单工具类）
SIMPLE_TASKS = {
    "flash": "fw-flash.py",
    "trans": "format-gbk2utf8.py",
    "trans16": "format-utf16be2utf8.py",
    "gencmake": "gen-cmake-paths.py",
    "pack": "fw-ota-pack.py",
    "ymodem": "fw-ota-ymodem.py",
}
AUDIT_TOOLS = ("sdk-check-oop.py", "check-eol.py")


def load_cfg() -> dict:
    if not os.path.isfile(TOML):
        sys.exit("[sdk_run] 找不到 %s" % TOML)
    with open(TOML, "rb") as f:
        return tomllib.load(f)


def dest_dir(cfg: dict) -> str:
    """拉取目标目录（相对 User/ 解析，与 sdk-pull.py 一致）。"""
    return os.path.normpath(os.path.join(PROJ, "User", cfg.get("dest") or "MySDK"))


def local_tools_dir(cfg: dict) -> str:
    """sdk-pull 把工具拉到哪里（tools_dir 相对 dest，默认 <dest>/tools）。"""
    dest = dest_dir(cfg)
    td = str(cfg.get("tools_dir") or "tools").strip()
    return td if os.path.isabs(td) else os.path.normpath(os.path.join(dest, td))


def sdk_root(cfg: dict):
    """SDK 检出目录；无效返回 None（不再全局致命——只有 pull 必须要有 SDK）。"""
    sdk = str(cfg.get("sdk") or "").strip().strip('"')
    if sdk and os.path.isdir(sdk) and os.path.isfile(os.path.join(sdk, "sdk_manifest.json")):
        return sdk
    return None


def _has_py(path: str) -> bool:
    return os.path.isdir(path) and any(f.endswith(".py") for f in os.listdir(path))


def resolve_tools(cfg: dict, force_sdk: bool = False):
    """返回 (工具目录, 来源标签) 或 (None, None)。"""
    if not force_sdk:
        for cand, tag in ((local_tools_dir(cfg), "工程内已拉取"),
                          (os.path.join(PROJ, "tools"), "工程自备")):
            if _has_py(cand):
                return cand, tag
    sdk = sdk_root(cfg)
    if sdk and _has_py(os.path.join(sdk, "tools")):
        return os.path.join(sdk, "tools"), "SDK 单源"
    return None, None


def _missing(tool: str, cfg: dict, tool_dir) -> int:
    sdk = sdk_root(cfg)
    sys.exit(
        "[sdk_run] 缺少工具: %s\n"
        "  已查找: %s\n"
        "          %s\n"
        "          %s\n"
        "  处理: ①把 User/sdk.toml 的 sdk 指到有效检出后执行「SDK PULL」把工具拉进工程；\n"
        "        ②或从别的工程拷一份 %s 过来（它自带工具，无需 SDK）。"
        % (
            tool,
            os.path.join(local_tools_dir(cfg), tool),
            os.path.join(PROJ, "tools", tool),
            os.path.join(sdk, "tools", tool) if sdk else "(sdk 路径无效，无法回退)",
            os.path.relpath(local_tools_dir(cfg), PROJ).replace("\\", "/"),
        )
    )


def run(script: str, args: list, label: str, tag: str) -> int:
    print("[sdk_run] %s -> %s （工具源：%s）" % (label, script, tag))
    try:
        sys.stdout.flush()
    except (AttributeError, ValueError):
        pass
    return subprocess.run([sys.executable, script, *args]).returncode


def cmd_tools(cfg: dict) -> int:
    """体检：工具源在哪、缺哪些工具——SDK 搬走后第一时间能看出问题。"""
    local = local_tools_dir(cfg)
    sdk = sdk_root(cfg)
    tool_dir, tag = resolve_tools(cfg)
    print("[tools] 配置     : %s" % TOML)
    print("[tools] SDK 检出 : %s" % (sdk or "无效/未配置（只能用工程内工具）"))
    print("[tools] 拉取目标 : %s" % dest_dir(cfg))
    print("[tools] 本地工具 : %s%s" % (local, "" if _has_py(local) else "  (未拉取)"))
    print("[tools] 实际使用 : %s（%s）" % (tool_dir or "无", tag or "-"))

    stamp = os.path.join(local, "_tools_sync.txt")
    if os.path.isfile(stamp):
        for ln in open(stamp, encoding="utf-8", errors="replace").read().splitlines():
            if ln.startswith(("SDK 版本", "拉取时间", "工具清单")):
                print("[tools] %s" % ln)

    names = sorted(set(SIMPLE_TASKS.values()) | set(AUDIT_TOOLS) | {"sdk-pull.py"})
    print("[tools] 工具状态（* = 当前可用）:")
    for n in names:
        marks = []
        if _has_py(local) and os.path.isfile(os.path.join(local, n)):
            marks.append("本地")
        if sdk and os.path.isfile(os.path.join(sdk, "tools", n)):
            marks.append("SDK")
        print("  %s%-24s %s" % ("* " if marks else "  ", n, "/".join(marks) or "缺失"))

    if tag is None:
        print("[tools] 结论: 没有任何可用工具源。Flash/YMODEM/Audit 都跑不了，"
              "请让 sdk.toml 的 sdk 指到有效检出后执行 SDK PULL。")
        return 1
    if not _has_py(local):
        print("[tools] 结论: 正在吃 SDK 单源。建议执行一次 SDK PULL 把工具拉进工程，"
              "之后 SDK 搬走也不影响烧录。")
    return 0


def main() -> int:
    if len(sys.argv) < 2:
        sys.exit("用法: sdk_run.py <flash|pull|audit|trans|trans16|gencmake|pack|ymodem|tools> [...]")
    try:
        # 任务面板里 SDK 侧日志要先于子进程输出出现（否则被缓冲顺序打乱）
        sys.stdout.reconfigure(line_buffering=True)
    except (AttributeError, ValueError):
        pass
    task = sys.argv[1]
    rest = sys.argv[2:]
    cfg = load_cfg()

    if task == "tools":
        return cmd_tools(cfg)

    if task == "pull":
        # 拉取必须从 SDK 侧执行（要读 manifest 与 library/）；本地工具只是副本，不能当源
        sdk = sdk_root(cfg)
        if not sdk:
            sys.exit(
                "[sdk_run] pull 需要有效的 SDK 检出，但 sdk.toml 的 sdk 路径无效: %r\n"
                "          其余任务（flash / ymodem / audit / trans）在工具已拉进工程时不需要 SDK。"
                % (cfg.get("sdk"),)
            )
        script = os.path.join(sdk, "tools", "sdk-pull.py")
        if not os.path.isfile(script):
            sys.exit("[sdk_run] 找不到 %s —— SDK 检出过旧，请更新 SDK" % script)
        return run(script, [TOML, *rest], "pull", "SDK 单源")

    if task == "audit":
        # 两道体检，任一不过即非零退出：
        #   ① sdk-check-oop.py —— library/ 是否直调 HAL / 引用工程句柄（板无关红线）
        #   ② check-eol.py     —— 行尾体检，防「CR 加倍」脏字节进仓库
        # 审计对象：有 SDK 就审 SDK 源仓；没有就审工程里的镜像 MySDK/（布局自适配）。
        tool_dir, tag = resolve_tools(cfg)
        sdk = sdk_root(cfg)
        target = sdk or dest_dir(cfg)
        rc = 0
        ran = 0
        for name in AUDIT_TOOLS:
            if tool_dir and os.path.isfile(os.path.join(tool_dir, name)):
                script = os.path.join(tool_dir, name)
            elif sdk and os.path.isfile(os.path.join(sdk, "tools", name)):
                script = os.path.join(sdk, "tools", name)
                tag = "SDK 单源"
            else:
                print("[sdk_run] 跳过（工具缺失）:", name)
                continue
            ran += 1
            args = ["--root", target] if name == "sdk-check-oop.py" else [target]
            rc |= run(script, args, "audit", tag)
        if ran == 0:
            print("[sdk_run] audit 未执行：工程内与 SDK 都找不到审计工具。"
                  "先让 sdk.toml 的 sdk 有效并执行 SDK PULL（选上 check-eol / sdk-check-oop）。")
            return 2
        return rc

    tool = SIMPLE_TASKS.get(task)
    if not tool:
        sys.exit("[sdk_run] 未知任务: %s" % task)

    tool_dir, tag = resolve_tools(cfg)
    if not tool_dir:
        return _missing(tool, cfg, tool_dir)
    script = os.path.join(tool_dir, tool)
    if not os.path.isfile(script):
        # 工具源里没有这一个（例如 [tools] 少选了某项）→ 再退一步找 SDK
        sdk = sdk_root(cfg)
        alt = os.path.join(sdk, "tools", tool) if sdk else None
        if alt and os.path.isfile(alt):
            script, tag = alt, "SDK 单源（本地未选该工具）"
        else:
            return _missing(tool, cfg, tool_dir)
    return run(script, rest, task, tag)


if __name__ == "__main__":
    sys.exit(main())
