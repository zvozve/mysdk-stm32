#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
sync_lib.py —— mystm32-sdk 子集拉取工具（SDK → 工程）

用法:
    python sync_lib.py <工程根>/sdk.toml [--sdk <SDK根目录>] [--dry-run]

sdk.toml 格式:
    sdk        = "C:/Container/Applications/Dev_STM32/mystm32-sdk"  # SDK 根目录
    dest       = "MySDK"                   # 拉取目标（相对工程根；根 CMakeLists 以同名目录 add_subdirectory）
    board_cfg  = "User/board_cfg.h"        # 绑定文件路径（已存在则不覆盖）

    [modules]                              # 显式选择；depends 闭包自动补全
    "chip.oop_dwt"   = true
    "devices.ir_tx"  = true

行为:
    1. 读 sdk_manifest.json，解析所选模块的 depends 闭包（external:* 跳过）
    2. 整目录镜像拷贝 模块 → dest/<layer>/<module>/（先清掉 dest，保证单源真相）
    3. 拷贝 SDK 根 CMakeLists.txt（组件构建脚本）→ dest/CMakeLists.txt，
       工程侧 add_subdirectory(<dest>) + 链接 mystm32 目标即可，无需维护源文件清单
    4. 生成 board_cfg.h 绑定模板（仅当文件不存在；具体引脚/句柄由工程填写）
    5. 写 dest/_sdk_sync.txt 戳（SDK 版本、时间、模块清单）
"""

import argparse
import datetime
import shutil
import sys
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


def mirror_module(sdk_root: Path, mod: dict, dest_root: Path, dry: bool):
    src = sdk_root / mod["path"]
    dst = dest_root / mod["path"]
    if not src.is_dir():
        sys.exit(f"模块目录缺失: {src}")
    if dry:
        print(f"  [dry] {mod['id']:24s} -> {dst}")
        return
    if dst.exists():
        shutil.rmtree(dst)
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(src, dst)


BOARD_CFG_TEMPLATE = """\
#ifndef __BOARD_CFG_H
#define __BOARD_CFG_H

/*
 * board_cfg.h —— 工程侧硬件绑定（类 devicetree，由 sync_lib.py 生成模板）
 *
 * 规则：
 *   - 全工程唯一允许 include CubeMX 生成头（main.h / usart.h / tim.h / gpio.h ...）
 *     的地方就是本文件；SDK（User/mystm32-sdk/）不做任何绑定。
 *   - app / tasks 只引用本文件的绑定宏，不直接引用 MX 符号。
 *   - 换板只改本文件（引脚、句柄、时钟），SDK 与业务代码不动。
 */

#include "main.h"
#include "usart.h"
#include "tim.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========== 绑定区（按实际板卡填写） ========== */

/* TODO: 示例——红外接收 1838B：引脚 + 1ms 节拍 TIM 句柄 */
/* #define BOARD_IR_RX_PORT        VS1838B_DAT_GPIO_Port  */
/* #define BOARD_IR_RX_PIN         VS1838B_DAT_Pin       */
/* #define BOARD_IR_RX_TIM         (&htim6)              */

/* TODO: 示例——心跳 LED + 看门狗句柄（无 IWDG 传 NULL） */
/* #define BOARD_HEART_LED_PORT    CPU_STA_GPIO_Port     */
/* #define BOARD_HEART_LED_PIN     CPU_STA_Pin           */
/* #define BOARD_HEART_IWDG        NULL                  */

/* TODO: 示例——红外发射：复合配置（引脚/定时器/载波参数） */
/* #define BOARD_IR_TX_CFG         { GPIOE, GPIO_PIN_6, GPIO_AF3_TIM9, \\
                                    TIM9, TIM_CHANNEL_2, 167, 25, 9 } */

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

    sdk_root = Path(args.sdk or cfg.get("sdk", "")).resolve()
    if not sdk_root.is_dir():
        sys.exit(f"SDK 根目录不存在: {sdk_root}")

    project_root = toml_path.parent
    dest = project_root / cfg.get("dest", "User/mystm32-sdk")
    board_cfg = project_root / cfg.get("board_cfg", "User/board_cfg.h")

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

    # 镜像同步：先清掉整个 dest，再拷贝闭包（保证 SDK 单源真相）
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)

    for m in manifest["modules"]:
        if m["id"] in closure:
            mirror_module(sdk_root, m, dest, dry=False)
            print(f"  [ok] {m['id']}")

    # SDK 组件构建脚本（随拉取更新，工程侧不维护）
    sdk_cmakelists = sdk_root / "CMakeLists.txt"
    if sdk_cmakelists.is_file():
        shutil.copy2(sdk_cmakelists, dest / "CMakeLists.txt")
        print(f"  [ok] CMakeLists.txt -> {dest / 'CMakeLists.txt'}")
    else:
        print("  [warn] SDK 根缺少 CMakeLists.txt，工程需自行接入源文件")

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
        "此目录由 mystm32-sdk/tools/sync_lib.py 生成，请勿手工修改（会被下次拉取覆盖）。\n"
        f"SDK 版本 : {manifest['sdk']['version']}\n"
        f"拉取时间 : {datetime.datetime.now():%Y-%m-%d %H:%M:%S}\n"
        f"模块清单 : {', '.join(closure)}\n",
        encoding="utf-8",
    )
    print(f"  [ok] 戳: {stamp}")
    print("完成。")


if __name__ == "__main__":
    main()
