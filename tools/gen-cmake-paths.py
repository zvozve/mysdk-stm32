#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen-cmake-paths.py —— 扫描工程目录，生成 CMake 源文件 / 包含路径片段（SDK tools/，单源）

用途:
    面向「手工维护 User/ 源码 + 主 CMakeLists」的工程（未走 sdk-pull 自动接线的项目）：
    扫描目录下所有 .c / .h，输出可直接粘进 CMakeLists 的片段 ——
      - file(GLOB_RECURSE USER_SOURCES "<dir>/*.c")   每含 .c 的目录一条
      - target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${USER_SOURCES})
      - target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE <含 .h 的目录> ...)

用法:
    python gen-cmake-paths.py [ROOT] [--output FILE] [--target NAME]
    ROOT      扫描根目录（默认 "."，即当前目录）；输出路径相对 ROOT，可直接用于位于 ROOT 的 CMakeLists。
    --output  同时把片段写入该文件（默认只打印到 stdout）。
    --target  target_sources / target_include_directories 的目标名（默认 ${CMAKE_PROJECT_NAME}）。

注意:
    - 仅收集路径，不读写文件内容；可重复执行、不会改坏任何源码。
    - 想只处理 User/ 时把 ROOT 指向 User/ 即可（如 `gen-cmake-paths.py User`）。
"""
import argparse
import os
import sys
from pathlib import Path

C_SUFFIX = (".c",)
H_SUFFIX = (".h",)


def scan(root: str):
    """返回 (c_dirs, h_dirs)：相对 root、含 .c / .h 的目录（去重、排序）。"""
    root_abs = os.path.abspath(root)
    if not os.path.isdir(root_abs):
        sys.exit("[gen-cmake-paths] 目录不存在: %s" % root_abs)

    c_dirs, h_dirs = set(), set()
    for cur, _dirs, files in os.walk(root_abs):
        has_c = any(f.endswith(C_SUFFIX) for f in files)
        has_h = any(f.endswith(H_SUFFIX) for f in files)
        rel = os.path.relpath(cur, start=root_abs).replace("\\", "/")
        if rel == ".":
            rel = ""  # 根目录本身
        if has_c:
            c_dirs.add(rel)
        if has_h:
            h_dirs.add(rel)
    return sorted(c_dirs), sorted(h_dirs)


def render(c_dirs, h_dirs, target: str) -> str:
    lines = []
    if c_dirs:
        lines.append("file(GLOB_RECURSE USER_SOURCES")
        for d in c_dirs:
            pat = f"{d}/*.c" if d else "*.c"
            lines.append(f'    "{pat}"')
        lines.append(")")
    else:
        lines.append("# 未找到含 .c 的目录")
        lines.append('file(GLOB_RECURSE USER_SOURCES "*.c"  # 改为实际路径)')
    lines.append("")
    lines.append("# Add sources to executable")
    lines.append(f"target_sources({target} PRIVATE")
    lines.append("    ${USER_SOURCES}")
    lines.append(")")
    lines.append("")
    lines.append("# Add include paths")
    if h_dirs:
        lines.append(f"target_include_directories({target} PRIVATE")
        for d in h_dirs:
            lines.append(f"    {d}" if d else "    .")
        lines.append(")")
    else:
        lines.append(f"target_include_directories({target} PRIVATE")
        lines.append("    # 未找到含 .h 的目录，改为实际路径")
        lines.append(")")
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(description="扫描目录生成 CMake 源/包含路径片段")
    ap.add_argument("root", nargs="?", default=".",
                    help="扫描根目录（默认当前目录）")
    ap.add_argument("--output", "-o", help="同时把片段写入该文件")
    ap.add_argument("--target", default="${CMAKE_PROJECT_NAME}",
                    help="target_sources/include_directories 的目标名（默认 ${CMAKE_PROJECT_NAME}）")
    args = ap.parse_args()

    c_dirs, h_dirs = scan(args.root)
    print("[gen-cmake-paths] 扫描: %s" % os.path.abspath(args.root))
    print("  含 .c 目录 %d 个，含 .h 目录 %d 个" % (len(c_dirs), len(h_dirs)))
    content = render(c_dirs, h_dirs, args.target)
    print("")
    print(content)

    if args.output:
        Path(args.output).write_text(content + "\n", encoding="utf-8")
        print("\n[gen-cmake-paths] 已写入: %s" % args.output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
