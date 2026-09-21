#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
format-utf16be2utf8.py —— UTF-16(LE/BE) / UTF-8-BOM 文本转 UTF-8 无 BOM（SDK tools/，单源）

与 format-gbk2utf8.py 互补：
    format-gbk2utf8.py     处理 GBK/GB2312（中文 Windows 记事本「ANSI」保存）
    format-utf16be2utf8.py 处理「带 BOM 的 UTF-16 LE/BE / UTF-8-BOM」——某些编辑器
                           （如以「Unicode / Unicode big endian」保存）会把 .c/.h/.md
                           写成 UTF-16，arm-none-eabi-gcc / Clang 编译直接报乱码，本工具归一化。

用法:
    python format-utf16be2utf8.py <file>
    python format-utf16be2utf8.py <dir> --recursive
    python format-utf16be2utf8.py a.c b.h dir/

行为:
    - 单文件: 就地转换为 UTF-8（覆盖写，无 BOM）。
    - 目录 + --recursive: 递归转换常见源码/文本后缀。
    - 已是 UTF-8 无 BOM / 解码失败（二进制或非 UTF-16）自动跳过，不会破坏文件。
"""
import os
import sys

TEXT_SUFFIX = (".c", ".h", ".cpp", ".hpp", ".cc", ".txt", ".md", ".py", ".s", ".ld")


def decode_to_utf8(raw: bytes):
    """返回 (text, label)：text 为解码后字符串（需写回），label 为结果标记。

    text 为 None 表示无需转换（已是 UTF-8 无 BOM / 二进制跳过）。
    """
    # 1) BOM 直判优先级最高（用 utf-16 自动判定字节序的编解码器，会自动剥掉 BOM 字符）
    if raw[:2] in (b"\xff\xfe", b"\xfe\xff"):
        candidates = [("utf-16", "utf-16")]
    elif raw[:3] == b"\xef\xbb\xbf":
        candidates = [("utf-8-sig", "utf-8-sig")]
    else:
        # 2) 无 BOM：UTF-8 优先（已是目标则跳过），其次 UTF-16（自动序），仍失败=二进制跳过
        candidates = [("utf-8", "skip(utf8-nobom)"),
                      ("utf-16", "utf-16")]
    for enc, label in candidates:
        try:
            text = raw.decode(enc)
        except UnicodeDecodeError:
            continue
        if label.startswith("skip"):
            return None, label
        return text, label
    return None, "skip(binary/unknown)"


def convert_file(path: str) -> str:
    try:
        with open(path, "rb") as f:
            raw = f.read()
    except OSError as e:
        return "err:%s" % e
    text, label = decode_to_utf8(raw)
    if text is None:
        return label
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)
    return "ok(%s->utf8)" % label


def main() -> int:
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 0
    recursive = False
    targets = []
    for a in args:
        if a in ("--recursive", "-r"):
            recursive = True
        else:
            targets.append(a)

    count = 0
    for t in targets:
        if os.path.isdir(t):
            if not recursive:
                print("[skip dir] %s (加 --recursive)" % t)
                continue
            for root, _dirs, files in os.walk(t):
                for fn in files:
                    if fn.endswith(TEXT_SUFFIX):
                        full = os.path.join(root, fn)
                        r = convert_file(full)
                        if r.startswith("ok"):
                            count += 1
                            print("  [%s] %s" % (r, full))
                        elif r.startswith("skip"):
                            pass
                        else:
                            print("  [%s] %s" % (r, full))
        elif os.path.isfile(t):
            r = convert_file(t)
            if r.startswith("ok"):
                count += 1
                print("[%s] %s" % (r, t))
            else:
                print("[%s] %s" % (r, t))
        else:
            print("[not found] %s" % t)
    print("完成: 转换 %d 个文件" % count)
    return 0


if __name__ == "__main__":
    sys.exit(main())
