#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
sdk-check-oop.py — mysdk-stm32 HAL 泄漏检查器

原则：SDK 是单源真相、板无关。任何模块都不得：
  1. 包含 CubeMX 生成的工程头（main.h / tim.h / gpio.h / usart.h /
     config_network.h / mxconstants.h ...）——这些头里才有具体句柄与 IO。
  2. 直接引用具体全局句柄（&huart6 / &htim6 / &hiwdg ...）或 MX 生成的
     引脚宏（XXX_GPIO_Port / XXX_Pin）、MX 初始化函数（MX_xxx_Init）、
     工程全局网口（gnetif）等。
  3. 在 chip/ 之外的层直接调用 HAL_* 函数或定义 HAL_*Callback。HAL 封装
     必须下沉 chip/（如 oop_uart / oop_i2s / oop_tim / oop_iwdg），
     device / protocol 层一律只调 oop_*，不碰 HAL。

具体句柄 / IO / 定时器 / 网口一律由调用方（工程 board_cfg）注入。

用法：
    python tools/sdk-check-oop.py            # 扫描 library/chip library/devices library/protocols
    python tools/sdk-check-oop.py --strict   # 非零退出码以便接入 CI
    python tools/sdk-check-oop.py --root <工程>/MySDK
                                             # 审 sdk-pull 镜像（<layer> 直挂布局，自动识别）
    python tools/sdk-check-oop.py --root <dir> library|mirror
                                             # 布局识别有歧义时显式指定

第三方中间件（library/middleware/、library/protocols/cJSON）为 vendor 代码，跳过不查。
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# 扫描范围（跳过第三方中间件与 cJSON）
# services/ 是系统服务层（bootloader / ota 等），与 devices/protocols 同样禁止直调 HAL
LAYERS = ["chip", "devices", "protocols", "services"]
DEFAULT_LAYOUT = "library"     # SDK 源仓布局：library/<layer>
MIRROR_LAYOUT = "mirror"       # sdk-pull 镜像布局：<layer> 直挂（工程 MySDK/）


def scan_dirs(root: str, layout: str = "") -> list:
    """按布局给出待扫目录。layout 为空时按 root 内容自适应。

    SDK 源仓       : <root>/library/{chip,devices,protocols,services}
    工程镜像 MySDK : <root>/{chip,devices,protocols,services}
    """
    if not layout:
        layout = DEFAULT_LAYOUT if os.path.isdir(os.path.join(root, "library")) else MIRROR_LAYOUT
    prefix = "library" if layout == DEFAULT_LAYOUT else ""
    return [os.path.join(prefix, d) if prefix else d for d in LAYERS]

# 1) 禁止包含的 CubeMX 工程生成头
FORBIDDEN_INCLUDES = [
    r'"main\.h"',
    r'"tim\.h"',
    r'"gpio\.h"',
    r'"usart\.h"',
    r'"config_network\.h"',
    r'"mxconstants\.h"',
    r'"\w+_hal_msp\.h"',
]

# 2) 禁止出现的具体句柄 / IO / 工程全局（全部按原文大小写匹配，避免误伤小写形参）
FORBIDDEN_PATTERNS = [
    # 具体全局外设句柄取地址：&huart6 / &htim6 / &hiwdg / &hdac1 ...
    (r'&h(uart|tim|spi|i2c|iwdg|dac|adc|can|eth|sai|qspi|fmc|sdmmc|rtc|wwdg|hrtim)\w*',
     "具体全局外设句柄取地址（应由调用方注入）"),
    # MX 生成的引脚宏（全大写 SIGNAL_GPIO_Port / SIGNAL_Pin）；排除 HAL 标准形参 GPIO_Pin/GPIO_Port
    (r'[A-Z][A-Za-z0-9_]*(?<!GPIO)_(GPIO_Port|Pin)\b',
     "MX 生成的引脚宏（应由调用方注入）"),
    # MX 初始化函数调用 / 声明
    (r'MX_[A-Za-z0-9_]+_Init',
     "CubeMX 生成的 MX_xxx_Init（看门狗/时钟等初始化应由工程完成）"),
    # 工程全局网口（仅在“引用/使用”时报警；说明性注释里出现不报）
    (r'\bgnetif\b',
     "工程全局网口 gnetif（应由调用方注入 struct netif*）"),
    # 工程配置宏
    (r'TARGET_PC_MAC_BYTE',
     "工程 config_network.h 宏（目标 MAC 应由调用方注入）"),
    # 直接 extern 工程全局句柄
    (r'extern\s+IWDG_HandleTypeDef\s+hiwdg',
     "extern 工程全局 IWDG 句柄（应由调用方注入）"),
]

INCLUDE_RE = re.compile("|".join(FORBIDDEN_INCLUDES))
PATTERN_RE = [(re.compile(p), why) for p, why in FORBIDDEN_PATTERNS]

# 仅 chip/ 层允许直接调用 HAL_* 函数 / 定义 HAL_*Callback；device/protocol 层禁止
# （HAL 封装必须下沉 chip/，如 oop_uart/oop_i2s/oop_tim/oop_iwdg）
HAL_DIRECT_RE = [
    (re.compile(r'\bHAL_[A-Z][A-Za-z0-9_]*\s*\('),
     "非 chip 层直接调用 HAL_* 函数（HAL 封装应下沉 chip/，如 oop_uart/oop_i2s/oop_tim/oop_iwdg）"),
]

# 去掉 C 注释后再查具体句柄/IO，避免注释里的举例/说明被误报
_COMMENT_RE = re.compile(r"/\*.*?\*/|//[^\n]*", re.DOTALL)


def _strip_comments(text):
    # 整文件去除 /* */ 与 // 注释，避免多行块注释里的举例/说明被误报
    return _COMMENT_RE.sub("", text)


def scan_file(path, allow_hal=False):
    hits = []
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            content = f.read()
    except Exception as e:
        print("  [warn] cannot read %s: %s" % (path, e))
        return hits
    cleaned = _strip_comments(content)
    for i, line in enumerate(cleaned.splitlines(), 1):
        if INCLUDE_RE.search(line):
            hits.append((i, line.strip(), "禁止包含 CubeMX 工程生成头"))
        for rx, why in PATTERN_RE:
            if rx.search(line):
                hits.append((i, line.strip(), why))
        if not allow_hal:
            for rx, why in HAL_DIRECT_RE:
                if rx.search(line):
                    hits.append((i, line.strip(), why))
    return hits


def main():
    args = sys.argv[1:]
    strict = "--strict" in args
    root = ROOT
    layout = ""
    for i, a in enumerate(args):
        if a == "--root" and i + 1 < len(args):
            root = os.path.abspath(args[i + 1])
        elif a.startswith("--root="):
            root = os.path.abspath(a.split("=", 1)[1])
        elif a in (DEFAULT_LAYOUT, MIRROR_LAYOUT):
            layout = a

    if not os.path.isdir(root):
        print("[ERR] 根目录不存在: %s" % root)
        return 2

    dirs = scan_dirs(root, layout)
    if not any(os.path.isdir(os.path.join(root, d)) for d in dirs):
        # 静默全过是最坏结果：宁可报错，让人知道扫错了地方
        print("[ERR] 在 %s 下未找到任何待扫层目录（尝试过: %s）" % (root, ", ".join(dirs)))
        return 2

    effective = layout or (DEFAULT_LAYOUT if os.path.isdir(os.path.join(root, "library")) else MIRROR_LAYOUT)
    print("[sdk-check-oop] 根目录: %s（%s 布局）" % (root, effective))

    total = 0
    # 仅 chip/ 层允许直接调用 HAL；devices/ / protocols/ / services/ 一律禁止直调 HAL_*
    for d in dirs:
        base = os.path.join(root, d)
        if not os.path.isdir(base):
            continue
        allow_hal = d.replace("\\", "/").endswith("/chip") or d == "chip"
        for cur, _dirs, files in os.walk(base):
            rel_dir = os.path.relpath(cur, root).replace("\\", "/")
            # 跳过第三方
            if "middleware" in rel_dir:
                continue
            for fn in files:
                if not fn.endswith((".c", ".h")):
                    continue
                rel = os.path.join(rel_dir, fn).replace("\\", "/")
                if "protocols/cJSON" in rel:
                    continue
                hits = scan_file(os.path.join(cur, fn), allow_hal)
                if hits:
                    print("[LEAK] %s" % rel)
                    for i, text, why in hits:
                        print("    L%-4d %-38s %s" % (i, text[:60], why))
                        total += 1
    if total == 0:
        print("OK: 未发现 HAL 泄漏（SDK 板无关，句柄/IO 均由调用方注入）")
        return 0
    print("\n发现 %d 处 HAL 泄漏，请改为由工程 board_cfg 注入。" % total)
    return 1 if strict else 0


if __name__ == "__main__":
    sys.exit(main())
