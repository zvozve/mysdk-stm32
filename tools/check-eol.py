#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check-eol.py —— 行尾体检（专治「行尾 CR 加倍」事故）

## 这个检查防的是什么

改写既有文件的脚本，如果**读的时候保留换行**、**写的时候用默认文本模式**，
Python 会在写时把每个 \\n 再翻成 os.linesep，于是每改一次行尾就多一个 CR：

    'a\\n'        --改一次-->  'a\\r\\r\\n'   --再改一次-->  'a\\r\\r\\r\\n'
    （LF）                      （VS Code 里看起来像多了一个空行）

同一段脚本如果自己还做过 `text.replace('\\n', '\\r\\n')`，第一次就翻倍。

后果有两层，第二层更麻烦：
  1. 编辑器里每行后面都像多一个空行；
  2. 该文件会被 git 判为「不参与行尾归一化」（`git ls-files --eol` 显示 `i/-text`），
     于是**脏字节直接被提交进仓库**——其它文件有 core.autocrlf 兜着，它没有。

## 判据

error（必须为零，非零即退出码 1）：
  · 行尾 CR 超过 1 个，或行内出现孤立 CR（即任何不是「单个 CR + LF」的 CR）

warn（只报告，不影响退出码）：
  · 同一文件里 CRLF 与 LF 混用
  · 按仓库约定应为 CRLF 的 C 源文件（.c/.h/.S）却是纯 LF
  · git 把文本文件判成 `-text`（索引里带脏 CR / 未参与归一化）

## 改既有文件时的正确写法（二选一）

    A) 字节进出：d = open(p, 'rb').read(); ...; open(p, 'wb').write(d)
    B) 保留换行：t = open(p, encoding='utf-8', newline='').read()
                open(p, 'w', encoding='utf-8', newline='').write(t)

  ✗ 禁止：读时保留换行（newline='' 或 二进制+decode），却用**默认**模式写。

用法:
  python tools/check-eol.py            # 默认检查本文件所在仓库（<SDK>/）
  python tools/check-eol.py <root>     # 检查指定目录
  python tools/check-eol.py -v         # 同时列出 warn 明细

退出码: 0 = 无 error；1 = 有 error
"""
import os
import subprocess
import sys

# 遍历时跳过的目录（构建产物 / 版本库内部 / 缓存）
SKIP_DIRS = {".git", "build", "Build", "Debug", "Release", "__pycache__",
             "node_modules", ".venv", "venv", ".vs", ".idea", ".workbuddy"}

# 纳入检查的扩展名（故意用白名单：避免去读真正的二进制）
EXTS = {".c", ".h", ".cpp", ".hpp", ".cc", ".s", ".py", ".md", ".json", ".toml",
        ".txt", ".cmake", ".ld", ".ioc", ".yaml", ".yml", ".cfg", ".xml", ".html", ".csv"}
# 无扩展名但也要查的文件
NAMES = {"cmakelists.txt", "makefile", ".gitignore", ".gitattributes"}

# 仓库约定：C 源文件用 CRLF（其它文件不强制，只报混用）
CRLF_EXTS = {".c", ".h", ".s"}

MAX_DETAIL = 6          # 每个文件最多列出几条明细


def wanted(name, ext):
    return ext in EXTS or name.lower() in NAMES


def scan(path):
    """返回 (errors, warns)；两者都是字符串列表。"""
    d = open(path, "rb").read()
    if not d:
        return [], []

    errors, warns = [], []
    parts = d.split(b"\n")
    n_crlf = n_lf = 0
    bad_tail, bad_inner = [], []

    # parts[:-1] 是以 LF 结尾的完整行；parts[-1] 是文件末的残段（无 LF 结尾）
    for n, ln in enumerate(parts[:-1], 1):
        body = ln.rstrip(b"\r")
        tail = len(ln) - len(body)
        if tail == 1:
            n_crlf += 1
        elif tail == 0:
            n_lf += 1
        else:
            bad_tail.append((n, tail))
        if b"\r" in body:
            bad_inner.append(n)
    if parts[-1]:
        rest = parts[-1]
        if len(rest) - len(rest.rstrip(b"\r")) > 1 or b"\r" in rest.rstrip(b"\r"):
            bad_tail.append((len(parts), len(rest) - len(rest.rstrip(b"\r"))))

    if bad_tail:
        sample = ", ".join(str(n) for n, _ in bad_tail[:MAX_DETAIL])
        worst = max(t for _, t in bad_tail)
        errors.append("行尾 CR 加倍：%d 行（最多 %d 个 CR；如第 %s 行）"
                      % (len(bad_tail), worst, sample))
    if bad_inner:
        sample = ", ".join(str(n) for n in bad_inner[:MAX_DETAIL])
        errors.append("行内出现孤立 CR：%d 行（如第 %s 行）" % (len(bad_inner), sample))

    if n_crlf and n_lf:
        warns.append("行尾混用：CRLF %d 行 / LF %d 行" % (n_crlf, n_lf))
    ext = os.path.splitext(path)[1].lower()
    if ext in CRLF_EXTS and n_lf and not n_crlf:
        warns.append("按约定应为 CRLF，实际是纯 LF（%d 行）" % n_lf)

    return errors, warns


def git_eol_index(root):
    """git 权威视角：{相对路径: 'i/lf'|'i/crlf'|'i/mixed'|'i/-text'}"""
    try:
        r = subprocess.run(["git", "-C", root, "ls-files", "--eol"],
                           capture_output=True, timeout=60)
    except (OSError, subprocess.SubprocessError):
        return {}
    if r.returncode != 0:
        return {}
    out = {}
    for ln in r.stdout.decode("utf-8", "replace").splitlines():
        fields = ln.split("\t")
        if len(fields) != 2:
            continue
        out[fields[1]] = fields[0].split()[0]
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    verbose = any(a in ("-v", "--verbose") for a in sys.argv[1:])
    root = os.path.abspath(args[0]) if args else \
        os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    if not os.path.isdir(root):
        sys.exit("[check-eol] 目录不存在: %s" % root)

    idx = git_eol_index(root)
    n_file = n_err = n_warn = 0
    lines = []

    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = sorted(d for d in dirnames if d not in SKIP_DIRS)
        for fn in sorted(filenames):
            ext = os.path.splitext(fn)[1].lower()
            if not wanted(fn, ext):
                continue
            path = os.path.join(dirpath, fn)
            rel = os.path.relpath(path, root).replace(os.sep, "/")
            n_file += 1
            try:
                errors, warns = scan(path)
            except OSError as e:
                lines.append("ERROR  %-52s 读取失败: %s" % (rel, e))
                n_err += 1
                continue
            # 索引侧视角（仅对纳入白名单的文本文件有意义）
            if idx.get(rel) == "-text":
                warns.append("git 索引侧判为 -text（未参与行尾归一化，脏字节会直接入库）")
            if errors:
                n_err += 1
                lines.append("ERROR  %-52s %s" % (rel, "; ".join(errors)))
            if warns and (verbose or errors):
                n_warn += 1
                lines.append("WARN   %-52s %s" % (rel, "; ".join(warns)))

    print("[check-eol] 根目录: %s" % root)
    print("[check-eol] 已检查 %d 个文本文件" % n_file)
    if lines:
        print("")
        for ln in lines:
            print("  " + ln)
        print("")
    if n_err:
        print("[check-eol] 失败：%d 个文件有 error（行尾 CR 加倍 / 孤立 CR）" % n_err)
        print("[check-eol] 修法：按 LF 切分、逐行 rstrip(b'\\r') 后以单 LF 拼回；"
              "改前改后要断言「抹掉所有 CR 的字节完全相同」。")
        return 1
    print("[check-eol] 通过：无「行尾 CR 加倍 / 孤立 CR」")
    if n_warn:
        print("[check-eol] 另有 %d 个文件有 warn（加 -v 看明细；不影响退出码）" % n_warn)
    return 0


if __name__ == "__main__":
    sys.exit(main())
