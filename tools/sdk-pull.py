#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
sdk-pull.py —— mystm32-sdk 子集拉取工具（SDK → 工程）

用法:
    python sdk-pull.py <工程根>/User/sdk.toml [--sdk <SDK根目录>] [--dry-run]

sdk.toml 格式 (本文件随工程放在 User/ 目录，与 board_cfg.h 同处工程侧资产):
    sdk        = "<SDK 仓库绝对路径>"      # SDK 根目录；省略时 sdk-pull.py 自动以其自身所在目录定位仓库
    dest       = "../MySDK"                # 拉取目标，相对 sdk.toml 所在目录(User/)解析；
                                            #   上跳一级落到工程根；根 CMakeLists 以同名目录 add_subdirectory
    board_cfg  = "board_cfg.h"             # 绑定文件，相对 sdk.toml 所在目录 User/ 解析；
                                            #   已存在则不覆盖（工程资产）

    [modules]                              # 显式选择；depends 闭包自动补全
    "chip.oop_dwt"   = true
    "devices.ir_transmitter" = true

行为:
    1. 读 sdk_manifest.json，解析所选模块的 depends 闭包（external:* 跳过）
    2. 整目录镜像拷贝 模块 → dest/<layer>/<module>/（先清掉 dest，保证单源真相）
    3. 拷贝 SDK 载荷根 CMakeLists.txt（<SDK>/library/CMakeLists.txt，与各 layer 同级）
       工程侧 add_subdirectory(<dest>) + 链接 mystm32 目标即可，无需维护源文件清单
    4. 生成 board_cfg.h 绑定模板（仅当文件不存在；具体引脚/句柄由工程填写）
    5. 写 dest/_sdk_sync.txt 戳（SDK 版本、时间、模块清单）
"""

import argparse
import datetime
import os
import shutil
import stat
import sys
import time
from pathlib import Path

try:
    import tomllib
except ImportError:  # pragma: no cover
    sys.exit("需要 Python >= 3.11（tomllib）")


def load_manifest(sdk_root: Path) -> dict:
    mf = sdk_root / "sdk_manifest.json"
    if not mf.is_file():
        sys.exit(f"manifest 不存在: {mf}")
    import json
    with open(mf, encoding="utf-8") as f:
        return json.load(f)


def resolve_closure(manifest: dict, selected: list) -> list:
    """解析 depends 闭包（拓扑序：被依赖者在前）。external:* 跳过。"""
    mods = {m["id"]: m for m in manifest["modules"]}
    unknown = [s for s in selected if s not in mods]
    if unknown:
        sys.exit(f"sdk.toml 引用了 manifest 中不存在的模块: {unknown}")

    order, seen = [], set()

    def visit(mid: str):
        if mid in seen or mid.startswith("external:"):
            return
        seen.add(mid)
        for dep in mods[mid].get("depends", []):
            visit(dep)
        order.append(mid)

    for s in selected:
        visit(s)
    return order


def rmtree_robust(path: Path, tries: int = 6, base_delay: float = 0.3) -> bool:
    """带重试的 rmtree。

    Windows 上目录常被瞬时占用（云盘同步 / 索引器 / 杀毒扫描 / 资源管理器），
    裸 shutil.rmtree 一次 PermissionError 就会让整次拉取失败。这里：
      1) 删前清只读属性（云端盘占位文件常带 R）
      2) 失败后整体重试并退避等待
    返回 True=删除成功（或本来就不存在），False=多次重试仍失败（调用方决定降级）。
    """
    if not path.exists():
        return True

    def _unlock_and_retry(func, p, exc):
        # 清只读/系统属性后原操作重试一次；仍失败则抛出，交给外层整体重试
        try:
            os.chmod(p, stat.S_IWRITE | stat.S_IREAD)
        except OSError:
            pass
        func(p)

    last = None
    for i in range(tries):
        try:
            shutil.rmtree(path, onerror=_unlock_and_retry)
            return True
        except OSError as e:
            last = e
            time.sleep(base_delay * (i + 1))
    print(f"  [warn] 删除失败（被占用），已跳过: {path}  <- {last}")
    return False


def mirror_module(sdk_root: Path, mod: dict, dest_root: Path, dry: bool):
    src = sdk_root / "library" / mod["path"]
    dst = dest_root / mod["path"]
    if not src.is_dir():
        sys.exit(f"模块目录缺失: {src}")
    if dry:
        print(f"  [dry] {mod['id']:24s} -> {dst}")
        return
    if dst.exists():
        rmtree_robust(dst)
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(src, dst)


def prune_stale(dest: Path, keep_paths: set):
    """删掉 dest 下不属于本次闭包的模块目录（曾选过、后取消的模块）。

    与「先删整个 dest」等价，但失败只告警不中断：单个目录被占用不会毁掉整次拉取。
    """
    for layer in sorted(p for p in dest.iterdir() if p.is_dir()):
        for mod_dir in sorted(p for p in layer.iterdir() if p.is_dir()):
            rel = f"{layer.name}/{mod_dir.name}"
            if rel not in keep_paths:
                if rmtree_robust(mod_dir):
                    print(f"  [del] 移除不再选择的模块: {rel}")


BOARD_CFG_TEMPLATE = """\
#ifndef __BOARD_CFG_H
#define __BOARD_CFG_H

/*
 * board_cfg.h —— 工程侧硬件绑定（类 devicetree，由 sdk-pull.py 生成模板）
 *
 * 规则：
 *   - 全工程唯一允许 include CubeMX 生成头（main.h / usart.h / tim.h / gpio.h ...）
 *     的地方就是本文件；SDK（library/）不做任何绑定。
 *   - app / tasks 只引用本文件的绑定宏，不直接引用 MX 符号。
 *   - 换板只改本文件（引脚、句柄、时钟），SDK 与业务代码不动。
 */

#include "main.h"
#include "usart.h"
#include "tim.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========== 功能开关（驱动读取：决定编译哪些传输/外设；与绑定区解耦） ========== */
/* 这些宏被 SDK 头（modbus_core.h / heart_beat.h）在 MB_BOARD_CFG 下读取；
 * 也可被 CMake -D 显式覆盖（优先级更高）。无外部依赖的传输默认开，依赖外部栈的默认关。 */
#define BOARD_MODBUS_RTU_ENABLE   1      /* RTU 串行传输（仅依赖 UART） */
#define BOARD_MODBUS_TCP_ENABLE   0      /* TCP 传输：需要 LwIP 栈；无网口板保持 0 */
#define BOARD_HEART_IWDG_ENABLE   1      /* 心跳喂狗：无独立看门狗设 0 */

/* ========== 绑定区（按实际板卡填写） ========== */

/* TODO: 示例——红外接收（解调接收头 OUT 接到捕获定时器通道引脚，1MHz 计数） */
/* #define BOARD_IR_RX_TIM          (&htim2)          */
/* #define BOARD_IR_RX_TIM_CHANNEL  TIM_CHANNEL_2     */
/* #define BOARD_IR_RX_TIM_CLK_HZ   1000000UL         */

/* TODO: 示例——心跳 LED + 看门狗句柄（无 IWDG 传 NULL） */
/* #define BOARD_HEART_LED_PORT    CPU_STA_GPIO_Port     */
/* #define BOARD_HEART_LED_PIN     CPU_STA_Pin           */
/* #define BOARD_HEART_IWDG        NULL                  */

/* TODO: 示例——红外发射：载波 PWM 通道 + 一路空闲 TIM 作 µs 时基 */
/* #define BOARD_IR_TX_TIM            (&htim3)        */
/* #define BOARD_IR_TX_TIM_CHANNEL    TIM_CHANNEL_2   */
/* #define BOARD_IR_TX_TIM_CLK_HZ     72000000UL      */
/* #define BOARD_IR_TX_TICK_TIM       (&htim4)        */
/* #define BOARD_IR_TX_TICK_CLK_HZ    72000000UL      */

/* TODO: 示例——非 CubeMX 管理的外设时钟（驱动不再接管 RCC，由工程开启） */
/* static inline void board_io_init(void) */
/* { */
/*     __HAL_RCC_TIM9_CLK_ENABLE(); */
/* } */

#ifdef __cplusplus
}
#endif

#endif /* __BOARD_CFG_H */
"""


def main():
    ap = argparse.ArgumentParser(description="mystm32-sdk 子集拉取")
    ap.add_argument("toml", help="工程 sdk.toml 路径")
    ap.add_argument("--sdk", help="覆盖 toml 中的 SDK 根目录")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    toml_path = Path(args.toml).resolve()
    if not toml_path.is_file():
        sys.exit(f"sdk.toml 不存在: {toml_path}")

    with open(toml_path, "rb") as f:
        cfg = tomllib.load(f)

    # SDK 根目录解析优先级: --sdk > sdk.toml 的 sdk= > 脚本自身所在目录(仓库内 tools/ 的上一级)
    # 仓库不记录自身位置，随 clone 到任意路径都能自定位。
    explicit = (args.sdk or cfg.get("sdk") or "").strip()
    if explicit and explicit != "<SDK_ROOT>":
        sdk_root = Path(explicit).resolve()
    else:
        sdk_root = Path(__file__).resolve().parent.parent
    if not sdk_root.is_dir() or not (sdk_root / "sdk_manifest.json").is_file():
        sys.exit(f"SDK 根目录不存在或无效（未找到 sdk_manifest.json）: {sdk_root}")

    project_root = toml_path.parent
    dest = project_root / cfg.get("dest", "MySDK")
    # project_root 就是 sdk.toml 所在目录（User/），故默认值不带 User/ 前缀
    board_cfg = project_root / cfg.get("board_cfg", "board_cfg.h")

    selected = [k for k, v in cfg.get("modules", {}).items() if v]
    if not selected:
        sys.exit("sdk.toml 未选择任何模块（[modules] 段为空）")

    manifest = load_manifest(sdk_root)
    closure = resolve_closure(manifest, selected)

    print(f"SDK      : {sdk_root}  (v{manifest['sdk']['version']})")
    print(f"工程     : {project_root}")
    print(f"目标     : {dest}")
    print(f"选中 {len(selected)} 个模块，闭包 {len(closure)} 个：")
    for mid in closure:
        print(f"  - {mid}")

    if args.dry_run:
        print("[dry-run] 不做任何写操作")
        for m in manifest["modules"]:
            if m["id"] in closure:
                mirror_module(sdk_root, m, dest, dry=True)
        return

    # 镜像同步：覆盖式拷贝闭包，再清理不再选择的模块（保证 SDK 单源真相）
    # 注：不再「先删整个 dest」——云端盘/索引器瞬时占用会让整次拉取失败；
    #     改为逐模块覆盖 + 收尾清理孤儿，局部被占用只告警不中断。
    dest.mkdir(parents=True, exist_ok=True)

    keep_paths = set()
    for m in manifest["modules"]:
        if m["id"] in closure:
            keep_paths.add(m["path"])
            mirror_module(sdk_root, m, dest, dry=False)
            print(f"  [ok] {m['id']}")

    prune_stale(dest, keep_paths)

    # SDK 组件构建脚本（随拉取更新，工程侧不维护）
    # 载荷根的组件构建脚本：与各 layer 目录同级，故在 <SDK>/library/ 下
    sdk_cmakelists = sdk_root / "library" / "CMakeLists.txt"
    if sdk_cmakelists.is_file():
        shutil.copy2(sdk_cmakelists, dest / "CMakeLists.txt")
        print(f"  [ok] library/CMakeLists.txt -> {dest / 'CMakeLists.txt'}")
    else:
        print("  [warn] SDK 缺少 library/CMakeLists.txt，工程需自行接入源文件")

    # board_cfg 模板（不覆盖已有——绑定是工程资产）
    # 注意：云端盘（如 Google Drive 在线-only 占位）下 os.path.exists 可能误报
    # 不存在，导致已填好的绑定被模板覆盖。故先用 try 打开并读取 1 字节确认，
    # 触发云端下载；非空即视为已存在，保留工程资产。
    _board_cfg_present = False
    try:
        with open(board_cfg, "rb") as _f:
            if _f.read(1):
                _board_cfg_present = True
    except OSError:
        _board_cfg_present = False
    if _board_cfg_present:
        print(f"  [keep] board_cfg 已存在，不覆盖: {board_cfg}")
    else:
        board_cfg.parent.mkdir(parents=True, exist_ok=True)
        board_cfg.write_text(BOARD_CFG_TEMPLATE, encoding="utf-8")
        print(f"  [new] board_cfg 模板已生成: {board_cfg}（需填写绑定后使用）")

    # 同步戳
    stamp = dest / "_sdk_sync.txt"
    stamp.write_text(
        "此目录由 mystm32-sdk/tools/sdk-pull.py 生成，请勿手工修改（会被下次拉取覆盖）。\n"
        "本目录随工程一并提交进版本库（不要 gitignore）：对拿到工程的人它就是源码的一部分。\n"
        f"SDK 版本 : {manifest['sdk']['version']}\n"
        f"拉取时间 : {datetime.datetime.now():%Y-%m-%d %H:%M:%S}\n"
        f"模块清单 : {', '.join(closure)}\n",
        encoding="utf-8",
    )
    print(f"  [ok] 戳: {stamp}")
    print("完成。")
    print(f"提示: {dest.name}/ 需随工程提交（不 gitignore），请检查 git status 后一并 commit。")


if __name__ == "__main__":
    main()
