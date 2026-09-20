#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
fw-flash.py —— STM32 J-Link 烧录（SDK 单源工具；取代已删除的 flash.bat）

为什么要有它（旧 flash.bat 为什么被删）：想让「烧录/调试都不依赖写死的 JLink 路径」，
bat 做不到干净——它只能用 findstr 抠 .ioc，抠出来的是 CubeMX 的 Mcu.UserName
（如 STM32F407ZGTx），而 J-Link 只认自家器件名（自带 ETC/JFlash/MCU.csv 里是
STM32F407ZG，全表 5207 项**没有任何以 Tx 结尾的名字**）。Python 侧可以做到：

    1) J-Link 安装目录逐级自检：显式参数 > 环境变量 > （注册表/目录扫描/PATH 收齐后取版本最高）；
    2) 器件名从 .ioc 的 Mcu.CPN 规范化（去尾 2 位 = 封装/温度码），若能在 J-Link 自带
       MCU.csv 里命中则额外确认（该表不覆盖全系列，只作加分不作否决）；
    3) 烧录成功后把检测结果**写回工程级 `.vscode/settings.json`**（root/device/interface/
       speed/svdFile 五键）。工程级不受 VS Code profile 影响，且由本脚本每次烧录后自动
       刷新——所以既不是「手写死的」，也不会因换机/升级 J-Link 而失效。这样 cortex-debug
       （它自己启动 JLinkGDBServerCL，脚本插不上手）也能用上同一份自动检测结果，
       整条链就都不写死了。

用法（前 6 个位置参数沿用旧 flash.bat 的顺序，行为参数缺省即空串，空串 = 自动检测）:
    python fw-flash.py [JLROOT] [ELF] [DEV] [ITF] [SPEED] [PROJ] [选项]

地址是怎么定的（依次尝试，全部从 .ld 的 FLASH ORIGIN 解析，**禁止写死偏移/地址**）:
    1) --ld / --slot 显式指定；
    2) ★ 与固件同目录、同名的 .ld（如 build/TP_MDC_A.elf ↔ build/TP_MDC_A.ld）—— 三分片
       工程的 CMake 把 TP_MDC{,_A,_B}.ld 生成到 .bin 同目录，于是「编译链接用的脚本」与
       「烧录解析地址用的脚本」永远是同一份；
    3) 否则扫工程根 *.ld：多份时自动选「不含 slot 字样的基版」（其 ORIGIN 即默认位置
       0x08000000）；若有多份基版则 fail-fast 列出让你显式指定，绝不静默猜。

    选项:
      --elf PATH           待烧固件的 ELF（取**同名同目录** .bin；OTA 双槽工程**必给**，
                           否则扫描 build/ 下多个槽的 .bin 无法自动分辨）
      --bin PATH           直接指定 .bin（优先级高于 --elf 的同名推导）
      --ld NAME|PATH       链接脚本（决定烧到哪：ORIGIN=槽基址）。一般不必给 —— 见上面
                           「地址是怎么定的」的自动解析顺序
      --slot A|B           双槽工程专用（向后兼容保留）：取基版 .ld → 派生槽 ORIGIN/LENGTH 并
                           **生成到 build/ 目录**（根目录无需预置 slotX.ld）。与 --ld 互斥；
                           选中的 .ld 触发 OTA 模式（擦该区 + loadfile）。
                           --slot-addr/--slot-len 可显式覆盖槽几何（默认按 F407 1MB 参考分区）
      --dry-run            只解析并打印「将要执行的 JLink 命令」，不烧录、不写 settings
      --settings-only      只做检测 + 写 settings.json（给 debug 用），不烧录
      --no-write-settings  正常烧录但不写 settings.json
      --color auto|always|never
                           彩色输出（默认 auto：终端与 VS Code 任务面板上色；输出被重定向到
                           文件/管道时自动关闭，不往日志里塞转义字符。沿用旧 flash.bat 的配色）
      --jlink-root PATH    显式指定 J-Link 安装目录（覆盖自检）
      --device NAME        显式指定 J-Link 器件名（覆盖 .ioc 推断）
      --interface SWD      显式指定接口
      --speed 4000         显式指定速率(kHz)
      --proj PATH          显式指定工程根（默认取第 6 个位置参数，再退到当前目录）
      --user-settings PATH 额外再把 jlink.root 写一份到用户级 settings.json（可选；注意
                          用户级是按 profile 隔离的，跑在具名 profile 下的工程读不到）
      -h, --help           本说明

退出码: 0 = 成功；1 = 失败。成功时 stdout 末尾有 FLASH SUCCESS 横幅（便于任务识别）。
"""
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

DEFAULT_IF = "swd"
DEFAULT_SPEED = "4000"
DEFAULT_ERASE_END = "0x080FFFFF"           # .ld 里拿不到长度时的兜底（1MB 器件）
ERR_HINTS = ("cannot connect", "could not connect", "error:", "failed", "timeout", "unable to")

# ────────────────────────────── ANSI 彩色输出 ──────────────────────────────
# 沿用旧 flash.bat 的配色：成功=黑字绿底三行框 + 亮绿箭头行；失败=白字红底 + 亮红箭头行。
RESET = "\x1b[0m"
BOLD = "\x1b[1m"
DIM = "\x1b[90m"
RED = "\x1b[91m"
GREEN = "\x1b[92m"
YELLOW = "\x1b[93m"
CYAN = "\x1b[96m"
OK_BAR = "\x1b[30;42m"                     # 黑字 / 绿底
ERR_BAR = "\x1b[97;41m"                    # 亮白字 / 红底
BOX_W = 60

COLOR = False


def c(s, code):
    """按需着色：COLOR=False（重定向到文件/管道）时原样返回，不往日志里塞转义字符。"""
    return ("%s%s%s" % (code, s, RESET)) if (COLOR and code) else s


def _enable_vt():
    """Windows 控制台默认不解析 ANSI，需要打开 ENABLE_VIRTUAL_TERMINAL_PROCESSING。"""
    if os.name != "nt":
        return
    try:
        import ctypes
        k = ctypes.windll.kernel32
        h = k.GetStdHandle(-11)            # STD_OUTPUT_HANDLE
        mode = ctypes.c_uint()
        if k.GetConsoleMode(h, ctypes.byref(mode)):
            k.SetConsoleMode(h, mode.value | 0x0004)
    except Exception:
        pass


def init_color(mode):
    """mode ∈ {auto, always, never}。auto：是终端（或看起来像 VS Code 终端）才上色。"""
    global COLOR
    if mode == "always":
        COLOR = True
    elif mode == "never":
        COLOR = False
    else:
        env = os.environ
        looks_like_term = any(env.get(k) for k in
                              ("WT_SESSION", "TERM_PROGRAM", "COLORTERM", "VSCODE_INJECTION", "VSCODE_IPC_HOOK_CLI"))
        COLOR = bool(sys.stdout.isatty() or looks_like_term) and not env.get("NO_COLOR")
    if COLOR:
        _enable_vt()
    return COLOR


def log(msg):
    print("%s %s" % (c("[flash]", CYAN), msg))


def warn(msg):
    print("%s %s" % (c("[flash][!]", YELLOW), msg))


def die(msg, code=1):
    print("%s %s" % (c("[flash][x]", RED), msg))
    sys.exit(code)


def result_box(ok, dev, detail):
    """烧录结果三行框（与旧 flash.bat 同款配色），供任务面板一眼看清成败。"""
    bar = OK_BAR if ok else ERR_BAR
    head = "FLASH SUCCESS" if ok else "FLASH FAILED"
    text = "####   %s   ####   device=%s" % (head, dev)
    print("=" * 66)
    if COLOR:
        print(bar + " " * BOX_W + RESET)
        print(bar + text.ljust(BOX_W)[:BOX_W] + RESET)
        print(bar + " " * BOX_W + RESET)
        print(c(">>> Download OK. Target reset and running." if ok
                else ">>> See JLink output above.", GREEN if ok else RED))
    else:
        print(text)
        print(">>> Download OK. Target reset and running." if ok else ">>> See JLink output above.")
    print("%s %s  %s" % (c("[flash]", CYAN), head, detail))
    return 0 if ok else 1


# ────────────────────────────── J-Link 安装目录自检 ──────────────────────────────
def _reg_install_paths():
    """从注册表取 J-Link 安装目录（Keil 装的 / SEGGER 官方装的）。"""
    out = []
    if os.name != "nt":
        return out
    try:
        import winreg
    except ImportError:
        return out
    for hive, sub, val in (
        (winreg.HKEY_CURRENT_USER, r"Software\Keil\ARM\JLink", "InstallPath"),
        (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\SEGGER\J-Link", "InstallPath"),
    ):
        try:
            with winreg.OpenKey(hive, sub) as k:
                v, _ = winreg.QueryValueEx(k, val)
                if v:
                    out.append(os.path.expandvars(str(v)))
        except OSError:
            pass
    return out


def _scan_install_dirs():
    """扫 Program Files 下的 SEGGER\\JLink*，版本号高的排前面（多版本时用最新的）。"""
    cands = []
    for pat in (r"C:\Program Files\SEGGER\JLink*", r"C:\Program Files (x86)\SEGGER\JLink*"):
        cands += glob.glob(pat)

    def ver(d):
        m = re.search(r"JLink_V(\d+)([a-zA-Z]?)", os.path.basename(d))
        return (int(m.group(1)), m.group(2).lower()) if m else (0, "")

    return sorted(cands, key=ver, reverse=True)


def _ver_key(path):
    m = re.search(r"JLink_V(\d+)([a-zA-Z]?)", os.path.basename(path.rstrip("\\/")) or path)
    return (int(m.group(1)), m.group(2).lower()) if m else (0, "")


def resolve_jlink_root(explicit):
    """
    返回 (根目录, 来源说明)。
    显式参数 / 环境变量直接采信（用户说了算）；其余来源（注册表、Program Files 扫描、PATH）
    全部收齐后**取版本号最高的那个**——注册表里常留着 Keil 装的老版本（本机就有 V650a），
    单纯按来源优先级会误选旧版。
    """
    for path, src in ((explicit, "显式参数"), (os.environ.get("JLINK_ROOT"), "环境变量 JLINK_ROOT")):
        if not path:
            continue
        p = path.rstrip("\\/")
        if os.path.isfile(os.path.join(p, "JLink.exe")):
            return p, src
        warn("%s=%s 下没有 JLink.exe，忽略" % (src, path))

    found = []
    cand_srcs = [(0, "注册表", _reg_install_paths()), (1, "扫描 Program Files", _scan_install_dirs())]
    w = shutil.which("JLink.exe")
    cand_srcs.append((2, "PATH", [os.path.dirname(w)] if w else []))
    for prio, src, paths in cand_srcs:
        for p in paths:
            if p and os.path.isfile(os.path.join(p, "JLink.exe")):
                found.append((_ver_key(p), -prio, p.rstrip("\\/"), src))
    if not found:
        return None, "注册表 / Program Files / PATH 里都没有 JLink.exe"

    uniq = {}
    for ver, negprio, p, src in found:
        uniq.setdefault(os.path.normcase(os.path.abspath(p)), (ver, negprio, p, src))
    found = sorted(uniq.values(), key=lambda x: (x[0], x[1]), reverse=True)
    best = found[0]
    if len(found) > 1:
        log("发现 %d 个 J-Link 安装：%s(%s) 版本最高 → 采用；其余 %s"
            % (len(found), os.path.basename(best[2]), best[3],
               ", ".join("%s(%s)" % (os.path.basename(c[2]), c[3]) for c in found[1:])))
    return best[2], best[3]


# ────────────────────────────── 器件名（.ioc → J-Link） ──────────────────────────────
def _mcu_csv_names(root):
    """读 J-Link 自带的器件清单（ETC/JFlash/MCU.csv，分号分隔）→ set(names)。"""
    for rel in (os.path.join("ETC", "JFlash", "MCU.csv"), "MCU.csv"):
        p = os.path.join(root, rel)
        if os.path.isfile(p):
            names = set()
            with open(p, encoding="utf-8", errors="replace") as f:
                for line in f:
                    parts = line.split(";")
                    if len(parts) > 1:
                        names.add(parts[1].strip())
            return names
    return None


def _ioc_fields(proj):
    """.ioc → (CPN, Name)。只认工程根下的 .ioc（与旧 flash.bat 一致）。"""
    for f in sorted(glob.glob(os.path.join(proj, "*.ioc"))):
        txt = open(f, encoding="utf-8", errors="replace").read()
        cpn = re.search(r"^Mcu\.CPN=(.+)$", txt, re.M)
        nm = re.search(r"^Mcu\.Name=(.+)$", txt, re.M)
        return ((cpn.group(1).strip() if cpn else None),
                (nm.group(1).strip() if nm else None),
                os.path.basename(f))
    return None, None, None


def resolve_device(explicit, proj, jlink_root):
    """
    返回 (器件名, 来源说明)。优先显式参数，其次 .ioc 的 Mcu.CPN 去尾 2 位。
    MCU.csv 只用来**加分**（命中说明板上确认收录），绝不用于否决——它是 J-Flash 的器件表，
    并不覆盖全系列（实测 V798b 的 MCU.csv 里 G4 系列整族缺失，但 J-Link 本身支持 G474RE）。
    """
    if explicit:
        return explicit, "显式参数"
    cpn, _name, ioc = _ioc_fields(proj)
    if not cpn:
        return None, "工程根下没有 .ioc / 里面没有 Mcu.CPN"
    cand = []
    if re.match(r"^STM32", cpn) and len(cpn) >= 8:
        cand.append(cpn[:-2])          # CPN 末 2 位 = 封装 + 温度码：STM32F407ZGT6 → STM32F407ZG
    cand.append(cpn)
    names = _mcu_csv_names(jlink_root) if jlink_root else None
    if names:
        for c in cand:
            if c in names:
                return c, "%s: Mcu.CPN=%s → %s（MCU.csv 校验通过）" % (ioc, cpn, c)
        return cand[0], ("%s: Mcu.CPN=%s → %s（MCU.csv 未收录，未校验；该表不覆盖全系列，"
                         "如 G4 整族缺失。若烧不进再用 --device 覆盖）" % (ioc, cpn, cand[0]))
    return cand[0], "%s: Mcu.CPN=%s → %s（未找到 MCU.csv，未校验）" % (ioc, cpn, cand[0])


# ────────────────────────────── 分区 / 固件 ──────────────────────────────
def _size_to_int(s):
    s = s.strip()
    mult = 1
    if s[-1] in "Kk":
        mult, s = 1024, s[:-1]
    elif s[-1] in "Mm":
        mult, s = 1024 * 1024, s[:-1]
    return int(s, 0) * mult


def parse_ld(proj, ld_path=None):
    """
    读 .ld 的 FLASH (rx) : ORIGIN = ..., LENGTH = ... → (origin, length, 文件名, 完整路径)。
    - 显式 ld_path（--ld，可相对工程根）：只解析那一份。
    - 不给：扫工程根 *.ld；**多于一份含 FLASH 行时返回 AMBIGUOUS** ——
      OTA 双槽工程根下有 1024K 原版 + slotA + slotB 三份，字典序第一个是原版，
      静默取它会把 ORIGIN 判成 0x08000000 → 走「不带地址的 loadfile」→ bin 落在
      0x08000000 **盖掉 BL**。宁可 fail-fast，也不要替用户猜错地方。
    """
    if ld_path:
        p = ld_path if os.path.isabs(ld_path) else os.path.join(proj, ld_path)
        files = [p]
    else:
        files = sorted(glob.glob(os.path.join(proj, "*.ld")))

    hits = []
    for f in files:
        if not os.path.isfile(f):
            continue
        txt = open(f, encoding="utf-8", errors="replace").read()
        m = re.search(r"FLASH\s*\([^)]*\)\s*:\s*ORIGIN\s*=\s*([^,\s]+)\s*,\s*LENGTH\s*=\s*([^\s,]+)",
                      txt, re.S)
        if m:
            try:
                hits.append((_size_to_int(m.group(1)), _size_to_int(m.group(2)),
                             os.path.basename(f), f))
            except ValueError:
                pass

    if not ld_path and len(hits) > 1:
        # OTA 双槽工程根下有多份 .ld（基版 1024K + slotA + slotB）。
        # 默认 Flash 指向「不含 slot 字样的基版 .ld」—— 其 ORIGIN 即 0x08000000（设备的「默认位置」）。
        # 基版唯一则用它（地址仍从 .ld 解析，绝不写死）；否则 fail-fast 让用户用
        # --slot / --ld 显式指定，避免静默烧错槽盖掉 BL。
        base_hits = [h for h in hits if "slot" not in h[2].lower()]
        if len(base_hits) == 1:
            log("多份 .ld：默认选基版（不含 slot）→ %s 作为默认位置（0x%08X）"
                % (c(base_hits[0][2], CYAN), base_hits[0][0]))
            return base_hits[0]
        return ("AMBIGUOUS", hits, None, None)
    if hits:
        return (hits[0][0], hits[0][1], hits[0][2], hits[0][3])
    return (None, None, None, None)


def find_slot_ld(proj, slot):
    """
    双槽工程专用：按 A/B 在工程根选 *slotA*.ld / *slotB*.ld，返回完整路径。
    找不到 / 找到多份都 die（多份会让人分不清到底烧哪个槽）。
    """
    pat = "*slot%s*.ld" % slot.upper()
    cands = sorted(glob.glob(os.path.join(proj, pat)))
    if not cands:
        die("工程根下找不到匹配 %s 的链接脚本（需存在如 STM32F407xx_FLASH_slot%s.ld）"
            % (pat, slot.upper()))
    if len(cands) > 1:
        die("%s 匹配到多份 .ld，无法确定用哪个:\n    %s"
            % (pat, "\n    ".join(cands)))
    return cands[0]


def slot_geometry(base_origin, base_len, slot, addr_override=None, len_override=None):
    """
    由基版 .ld 的 FLASH 范围推导 A/B 槽的 ORIGIN/LENGTH。
    优先用显式 --slot-addr/--slot-len（不写进 tasks.json，需要时临时给）；
    否则用参考分区（与设备侧 ota_areas.c 对齐）：F407 1MB，BL+CFG 共 64K 预留。
    返回 (origin, length)。地址一律从基版 .ld 派生，绝不写死。
    """
    if addr_override is not None and len_override is not None:
        try:
            return (_size_to_int(addr_override), _size_to_int(len_override))
        except ValueError:
            die("--slot-addr/--slot-len 不是合法数字")
    if base_len == 0x100000:                       # 1MB：F407ZG 参考分区
        if slot.upper() == "A":
            return (base_origin + 0x10000, 0x70000)   # 0x08010000 / 448K
        return (base_origin + 0x80000, 0x80000)        # 0x08080000 / 512K
    die("未知 Flash 容量 0x%X，无法确定 slot %s 几何；请用 --slot-addr/--slot-len 指定"
        % (base_len, slot.upper()))


def gen_slot_ld(proj, base_ld_path, slot, origin, length):
    """
    把基版 .ld 复制进 <proj>/build/，改写 FLASH 的 ORIGIN/LENGTH 为指定槽，返回生成路径。
    地址从基版 .ld 派生（绝不写死）；生成物落在 build 目录，**根目录无需预置 slot .ld**。
    """
    build_dir = os.path.join(proj, "build")
    os.makedirs(build_dir, exist_ok=True)
    stem, ext = os.path.splitext(os.path.basename(base_ld_path))
    out_name = "%s_slot%s%s" % (stem, slot.upper(), ext)
    out_path = os.path.join(build_dir, out_name)
    txt = open(base_ld_path, encoding="utf-8", errors="replace").read()
    pat = re.compile(r"(FLASH\s*\([^)]*\)\s*:\s*ORIGIN\s*=\s*)([^\s,]+)"
                     r"(\s*,\s*LENGTH\s*=\s*)([^\s,]+)")
    new_txt, n = pat.subn(lambda m: "%s0x%08X%s0x%08X"
                           % (m.group(1), origin, m.group(3), length), txt)
    if n == 0:
        die("在 %s 没找到 FLASH ORIGIN/LENGTH，无法改写槽地址" % base_ld_path)
    with open(out_path, "w", encoding="utf-8", newline="") as f:
        f.write(new_txt)
    log("生成槽 .ld  : %s  (ORIGIN=0x%08X LENGTH=0x%X)" % (out_path, origin, length))
    return out_path


def resolve_slot_ld(proj, slot, addr_override, len_override):
    """
    --slot A|B：优先「基版 .ld → 派生槽 .ld 并生成到 build/」；
    工程根没有基版 .ld（只有 slotX.ld）时，退回根目录的 *slotX*.ld（向后兼容）。
    """
    res = parse_ld(proj)                            # 多 .ld 时自动选「不含 slot」的基版
    if res[0] == "AMBIGUOUS":
        return find_slot_ld(proj, slot)
    base_origin, base_len, _name, base_path = res
    origin, length = slot_geometry(base_origin, base_len, slot, addr_override, len_override)
    return gen_slot_ld(proj, base_path, slot, origin, length)


def find_sibling_ld(bin_path):
    """
    「与固件同目录、同名（扩展名换成 .ld）」的链接脚本。
    三分片工程的 CMake 会把 TP_MDC{,_A,_B}.ld 生成到 .bin 同目录 —— 用它就不必再按
    --slot 现场派生，而且保证「编译链接用的脚本」与「烧录解析地址用的脚本」是同一份
    文件（少一个走偏的机会）。地址仍从 .ld 解析，绝不写死。
    """
    if not bin_path:
        return None
    cand = os.path.splitext(bin_path)[0] + ".ld"
    return cand if os.path.isfile(cand) else None


def find_bin(elf, proj, bin_path=None):
    """
    定位待烧录 .bin：--bin 显式 → ELF 同名同目录 → 扫 build/{Release,Debug}、build/*/、build/。
    扫描拿到多个 .bin 时 **fail-fast 列出**（双槽工程的 build/ 下同时有 ota_A/ota_B/Release
    三个 bin，sorted 后 Release（BL 的产物）排最前，静默取第一个会烧错固件）。
    """
    if bin_path:
        p = bin_path if os.path.isabs(bin_path) else os.path.join(proj, bin_path)
        if not os.path.isfile(p):
            die("--bin 指定的文件不存在: %s" % p)
        return p

    if elf:
        e = elf if os.path.isabs(elf) else os.path.join(proj, elf)
        cand = os.path.splitext(e)[0] + ".bin"
        if os.path.isfile(cand):
            return cand

    seen = []
    for pat in (os.path.join(proj, "build", "Release", "*.bin"),
                os.path.join(proj, "build", "Debug", "*.bin"),
                os.path.join(proj, "build", "*", "*.bin"),
                os.path.join(proj, "build", "*.bin")):
        seen += sorted(glob.glob(pat))
    uniq = sorted(set(os.path.normcase(h) for h in seen))

    if len(uniq) > 1:
        die("扫到 %d 个 .bin（OTA 双槽工程的 build/ 下同时有多个槽的产物），"
            "自动选取会烧错固件。用 --elf <ELF 路径>（取同名 .bin）或 --bin <bin 路径>"
            "显式指定:\n    %s" % (len(uniq), "\n    ".join(uniq)))
    return uniq[0] if uniq else None


# ────────────────────────────── settings.json 就地增/改 ──────────────────────────────
def strip_jsonc(src):
    """去掉 // 行注释（尊重字符串），供 JSON 合法性校验用。"""
    out, i, n, instr = [], 0, len(src), False
    while i < n:
        c = src[i]
        if instr:
            out.append(c)
            if c == "\\" and i + 1 < n:
                out.append(src[i + 1]); i += 2; continue
            if c == '"':
                instr = False
            i += 1
            continue
        if c == '"':
            instr = True; out.append(c); i += 1; continue
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            while i < n and src[i] != "\n":
                i += 1
            continue
        out.append(c); i += 1
    return "".join(out)


def ensure_json_key(path, key, value, create=False):
    """
    在 JSONC 文件里就地增/改一个字符串键，**保留原有格式与注释**（不做 load+dump 重写）。
    返回 (状态, 说明)，状态 ∈ {same, inserted, updated, created, skip, error}。
    """
    key_pat = '"%s"\\s*:\\s*"((?:[^"\\\\]|\\\\.)*)"' % re.escape(key)
    if not os.path.isfile(path):
        if not create:
            return ("skip", "文件不存在，已跳过: %s" % path)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        txt = "{\n    %s: %s\n}\n" % (json.dumps(key), json.dumps(value))
        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write(txt)
        return ("created", path)

    with open(path, encoding="utf-8", newline="") as f:
        txt = f.read()
    nl = "\r\n" if "\r\n" in txt else "\n"
    m = re.search(key_pat, txt)
    if m:
        raw = m.group(1)
        try:
            cur = json.loads('"%s"' % raw)     # 反转义后再比，否则 "C:\\x" 与 "C:\x" 会被判成不同
        except ValueError:
            cur = raw
        if cur == value:
            return ("same", "已是该值")
        new = txt[:m.start()] + '"%s": %s' % (key, json.dumps(value)) + txt[m.end():]
        status = "updated"
    else:
        i = txt.rfind("}")
        if i < 0:
            return ("error", "不是合法 JSON 对象: %s" % path)
        head, tail = txt[:i], txt[i:]
        sep = "" if strip_jsonc(head).rstrip().endswith("{") else ","
        new = (head.rstrip() + sep + nl + "    %s: %s" % (json.dumps(key), json.dumps(value))
               + nl + tail)
        status = "inserted"

    try:
        json.loads(strip_jsonc(new))
    except ValueError as e:
        return ("error", "改写后 JSON 非法，已放弃写回（%s）: %s" % (e, path))
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(new)
    return (status, path)


def user_settings_path(explicit):
    """只在显式给了 --user-settings 时才写用户级（默认 profile 专用，见 write_debug_settings）。"""
    return explicit


def write_debug_settings(proj, root, dev, itf, speed, user_settings, dry):
    """
    **默认全部写工程级 `<proj>/.vscode/settings.json`** —— 因为它不受 VS Code profile 影响。
    用户级 `%APPDATA%\\Code\\User\\settings.json` 只是「默认 profile」的配置：本机有多个具名
    profile（STM32Cube / ESP-IDF / QT …），工程大多跑在具名 profile 下，写用户级**不生效**
    （实测：把 jlink.root 挪到用户级后，STM32Cube profile 里的工程按 F8 仍报
    `Variable ${config:jlink.root} can not be resolved`）。
    `jlink.root` 虽是机器事实，但它由本脚本每次烧录后自动刷新，所以放工程级不会「写死」——
    换机/升级 J-Link 后跑一次 flash 就矫正了。
    若显式给了 `--user-settings PATH`，额外再写一份用户级（默认 profile 的工程可直接受益）。
    """
    log("settings.json 同步（供 cortex-debug 使用）:")
    root = root.replace("\\", "/")          # 与既有工程风格一致（VS Code 两者都吃）

    ws_dir = os.path.join(proj, ".vscode")
    if not os.path.isdir(ws_dir):
        warn("工程没有 .vscode/ 目录 → 跳过写回（debug 配置本身也在那里，先建好再跑本脚本）")
        return
    ws_path = os.path.join(ws_dir, "settings.json")

    svd = sorted(glob.glob(os.path.join(ws_dir, "*.svd")))
    svd_val = "${workspaceFolder}/.vscode/%s" % os.path.basename(svd[0]) if svd else ""
    if not svd:
        warn("`.vscode/` 下没有 *.svd → jlink.svdFile 写空串（避免变量解析失败；"
             "把 svd 放进 .vscode/ 后重跑本脚本即可自动指上）")

    items = [
        (ws_path, "jlink.root", root, "机器事实·自动刷新"),
        (ws_path, "jlink.device", dev, "工程事实"),
        (ws_path, "jlink.interface", itf, "工程事实"),
        (ws_path, "jlink.speed", speed, "工程事实"),
        (ws_path, "jlink.svdFile", svd_val, "工程事实"),
    ]
    if user_settings:
        items.append((user_settings, "jlink.root", root, "用户级·默认 profile 专用"))

    for path, key, value, tag in items:
        if dry:
            log("  (dry-run) %-16s = %-42s → %s [%s]" % (key, repr(value), path, tag))
            continue
        st, detail = ensure_json_key(path, key, value)
        if st == "error":
            warn("  %-16s = %-42s → %s" % (key, repr(value), detail))
        else:
            log("  %-16s = %-42s [%s | %s]" % (key, repr(value), tag, st))


# ────────────────────────────── 主流程 ──────────────────────────────
def parse_argv(argv):
    pos, opt = [], {}
    flags = {"dry_run": False, "settings_only": False, "no_write": False}
    i = 0
    while i < len(argv):
        a = argv[i]
        if a in ("-h", "--help"):
            print(__doc__)
            sys.exit(0)
        elif a == "--dry-run":
            flags["dry_run"] = True
        elif a == "--settings-only":
            flags["settings_only"] = True
        elif a == "--no-write-settings":
            flags["no_write"] = True
        elif a in ("--jlink-root", "--device", "--interface", "--speed",
                   "--proj", "--user-settings", "--color", "--elf", "--ld", "--bin",
                   "--slot", "--slot-addr", "--slot-len"):
            if i + 1 >= len(argv):
                die("%s 缺少参数值" % a)
            opt[a.lstrip("-").replace("-", "_")] = argv[i + 1]
            i += 1
        elif a.startswith("--"):
            die("未知选项: %s（用 --help 看用法）" % a)
        else:
            pos.append(a)
        i += 1
    return pos, opt, flags


def main():
    pos, opt, flags = parse_argv(sys.argv[1:])
    init_color(opt.get("color", "auto"))
    jlroot_in = opt.get("jlink_root", pos[0] if len(pos) > 0 else "")
    elf = opt.get("elf", pos[1] if len(pos) > 1 else "")
    dev_in = opt.get("device", pos[2] if len(pos) > 2 else "")
    itf = opt.get("interface", pos[3] if len(pos) > 3 else "") or os.environ.get("JLINK_IF", "")
    speed = opt.get("speed", pos[4] if len(pos) > 4 else "") or os.environ.get("JLINK_SPEED", "")
    proj = opt.get("proj", pos[5] if len(pos) > 5 else "") or os.getcwd()
    proj = os.path.abspath(proj)

    print(c("=" * 66, DIM))
    log("工程根      : %s" % c(proj, CYAN))
    if not os.path.isdir(proj):
        die("工程根不存在: %s" % proj)

    # ---- J-Link 安装目录 ----
    root, src = resolve_jlink_root(jlroot_in)
    if not root:
        die("找不到 J-Link 安装目录（下面这些地方都试过了）: %s\n"
            "    可显式指定: --jlink-root \"C:/Program Files/SEGGER/JLink_V7xx\"" % src)
    log("J-Link      : %s  %s" % (c(root, CYAN), c("[来源: %s]" % src, DIM)))

    # ---- 器件名 ----
    dev, dev_src = resolve_device(dev_in, proj, root)
    if not dev:
        die("拿不到 J-Link 器件名: %s\n    可显式指定: --device STM32F407ZG" % dev_src)
    log("Device      : %s  %s" % (c(dev, BOLD + CYAN), c("[%s]" % dev_src, DIM)))

    # ---- 接口 / 速率 ----
    itf = (itf or DEFAULT_IF).lower()
    speed = str(speed or DEFAULT_SPEED)
    log("接口 / 速率 : %s%s" % (c("%s / %s kHz" % (itf.upper(), speed), CYAN),
        c("  (默认值)", DIM) if (itf == DEFAULT_IF and speed == DEFAULT_SPEED) else ""))

    # ---- 固件（先定位：下面「与固件同目录的 .ld」规则要用到它的目录）----
    bin_path = find_bin(elf, proj, opt.get("bin"))
    if not bin_path:
        die("找不到 .bin（ELF 参数=%r；已扫 build/Release、build/Debug、build/*/）—— 先编译" % elf)
    log("固件        : %s  %s" % (c(bin_path, CYAN),
                                 c("(%.1f KB)" % (os.path.getsize(bin_path) / 1024.0), DIM)))

    # ---- .ld → 模式 ----
    slot = opt.get("slot")
    ld_path = opt.get("ld")
    if slot and ld_path:
        die("--slot 与 --ld 互斥，二选一即可")
    if slot:
        if slot.upper() not in ("A", "B"):
            die("--slot 只接受 A 或 B（大小写均可）")
        ld_path = resolve_slot_ld(proj, slot, opt.get("slot_addr"), opt.get("slot_len"))
        log("选槽        : --slot %s → %s" % (slot.upper(), c(os.path.basename(ld_path), CYAN)))
    elif not ld_path:
        # ★ 优先「与固件同目录的同名 .ld」：三分片工程的 CMake 把三份 .ld 生成到 .bin
        #   同目录，用它可保证编译链接与烧录解析同源（不必再按 --slot 现场派生）。
        sib = find_sibling_ld(bin_path)
        if sib:
            ld_path = sib
            log("链接脚本    : 同目录 %s（三片构建生成；编译链接与烧录解析同源）"
                % c(os.path.basename(sib), CYAN))
    origin, length, ld, _ld_path = parse_ld(proj, ld_path)
    if origin == "AMBIGUOUS":
        die("工程根下有 %d 份含 FLASH 分区的 .ld，无法自动选（字典序第一份是 1024K 原版，"
            "静默使用会把 bin 烧到 0x08000000 盖掉 BL）:\n    %s\n"
            "    用 --ld <文件名> 显式指定（如 --ld STM32F407xx_FLASH_slotA.ld）"
            % (len(length), "\n    ".join(h[3] for h in length)))
    ota = origin is not None and origin != 0x08000000
    if origin is None:
        warn(".ld 里没解析出 FLASH ORIGIN → 按普通模式（不擦除）")
        mode = "普通（.ld 未解析）"
        erase_end = DEFAULT_ERASE_END
    elif ota:
        mode = "OTA（擦 APP 区，保留 BL）"
        erase_end = "0x%08X" % (origin + length - 1) if length else DEFAULT_ERASE_END
        log("分区        : %s" % (c("%s FLASH ORIGIN=0x%08X LENGTH=0x%X → 擦 0x%08X..%s"
                                    % (ld, origin, length or 0, origin, erase_end), CYAN)))
    else:
        mode = "普通（不擦除，沿用旧 flash.bat 行为）"
        erase_end = DEFAULT_ERASE_END

    log("模式        : %s" % c(mode, YELLOW if ota else DIM))

    # ---- settings.json 写回（给 debug 用）----
    if flags["no_write"]:
        log("settings.json 同步: 已按 --no-write-settings 跳过")
    else:
        write_debug_settings(proj, root, dev, itf, speed, user_settings_path(opt.get("user_settings")),
                             flags["dry_run"])

    if flags["settings_only"]:
        log("--settings-only：不烧录，退出")
        return 0

    # ---- 生成 J-Link 命令脚本（沿用旧 flash.bat 的语义）----
    lines = ["r", "h"]
    if ota:
        lines.append("erase 0x%08X, %s" % (origin, erase_end))
        lines.append('loadfile "%s", 0x%08X' % (bin_path, origin))
    else:
        lines.append('loadfile "%s"' % bin_path)
    lines += ["r", "g", "exit"]

    jlink = os.path.join(root, "JLink.exe")
    cmd = [jlink, "-device", dev, "-if", itf, "-speed", speed, "-autoconnect", "1", "-nogui", "1"]

    if flags["dry_run"]:
        log("--dry-run：将执行（不烧录）:")
        log("  %s -CommanderScript <临时脚本>" % " ".join('"%s"' % c for c in cmd))
        log("  脚本内容: %s" % " | ".join(lines))
        return 0

    tmpdir = tempfile.mkdtemp(prefix="jlink_")
    script = os.path.join(tmpdir, "flash.jlink")
    with open(script, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    log("烧录中 ...")
    proc = subprocess.run(cmd + ["-CommanderScript", script], capture_output=True)
    out = (proc.stdout or b"").decode("utf-8", "replace") + (proc.stderr or b"").decode("utf-8", "replace")
    print(out)
    shutil.rmtree(tmpdir, ignore_errors=True)

    low = out.lower()
    ok = not (any(h in low for h in ERR_HINTS) or "O.K." not in out)
    return result_box(ok, dev, bin_path)


if __name__ == "__main__":
    sys.exit(main())
