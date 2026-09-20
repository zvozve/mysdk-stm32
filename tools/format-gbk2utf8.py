#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
format-gbk2utf8.py —— GBK/GB2312 文本转 UTF-8（SDK tools/，单源）

用法:
    python format-gbk2utf8.py <file>
    python format-gbk2utf8.py <dir> --recursive
    python format-gbk2utf8.py a.c b.h dir/

行为:
    - 单文件: 就地转换为 UTF-8（覆盖写）。
    - 目录 + --recursive: 递归转换常见源码/文本后缀。
    - 已是 UTF-8 或非文本（解码失败）自动跳过并提示，不会破坏文件。
"""
import os
import sys

TEXT_SUFFIX = (".c", ".h", ".cpp", ".hpp", ".cc", ".txt", ".md", ".py", ".s", ".ld")


def convert_file(path: str) -> str:
    try:
        with open(path, "rb") as f:
            raw = f.read()
        text = raw.decode("gbk")
    except UnicodeDecodeError:
        return "skip(utf8/binary)"
    except Exception as e:  # pragma: no cover
        return "err:%s" % e
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)
    return "ok"


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
                        if r == "ok":
                            count += 1
                            print("  [ok] %s" % full)
                        elif r.startswith("skip"):
                            pass
                        else:
                            print("  [%s] %s" % (r, full))
        elif os.path.isfile(t):
            r = convert_file(t)
            if r == "ok":
                count += 1
                print("[ok] %s" % t)
            else:
                print("[%s] %s" % (r, t))
        else:
            print("[not found] %s" % t)
    print("完成: 转换 %d 个文件" % count)
    return 0


if __name__ == "__main__":
    sys.exit(main())
