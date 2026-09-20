#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
fw-ota-pack.py —— 把 APP 的 bin 打成 .otapkg（OTA 固件包）

为什么需要它：把「长度 / CRC32 / 目标地址」钉进包内，而不是塞在 MQTT JSON 或
HTTP 自定义头里 —— 这样任何下载通道（串口 / 网络 / TF 卡）都能自校验，BL 与 APP
也都能各自独立校验。

包格式（80 字节头，小端；与 SDK 的 services/ota_core/Inc/ota_image.h 一一对应）:
    off size field
    0   4    magic        'O','T','A','P'  (0x5041544F)
    4   2    hdr_ver      1
    6   2    hdr_size     80
    8   4    pkg_size     整包字节数
    12  4    fw_ver       major<<24 | minor<<16 | patch<<8 | stage
    16  4    build_id     构建时间戳
    20  1    seg_count    1（单段，串口用）或 2（双段，HTTP 可只取目标段）
    21  1    flags        bit0=含BL镜像 bit1=已签名
    22  2    reserved
    24  12   seg[0]       load_addr(4) + size(4) + crc32(4)
    36  12   seg[1]       同上（seg_count==1 时全 0）
    48  28   reserved
    76  4    hdr_crc32    前 76 字节的 CRC32

CRC 一律 CRC-32/ISO-HDLC（= zlib.crc32），与固件侧 ota_crc32() 完全一致。

用法:
    # 双段包（一个版本一个文件；HTTP 场景设备用 Range 只下自己那一段）
    python fw-ota-pack.py --slot-a build/app_slotA.bin --slot-b build/app_slotB.bin \\
                       --ver 1.2.3 -o dist/app_v1.2.3.otapkg

    # 单段包（串口 YMODEM 等无法 seek 的通道，省一半时间）
    python fw-ota-pack.py --slot b --bin build/app_slotB.bin --ver 1.2.3 \\
                       -o dist/app_v1.2.3_slotB.otapkg

    # 查看已有包
    python fw-ota-pack.py --list dist/app_v1.2.3.otapkg

默认段地址 = 分区表（doc/01）里的 SlotA/SlotB 基址；换布局用 --load-a/--load-b 覆盖。
"""
import argparse
import os
import struct
import sys
import time
import zlib

MAGIC = 0x5041544F          # 'OTAP'
HDR_VER = 1
HDR_SIZE = 80
SEG_MAX = 2

DEFAULT_LOAD_A = 0x08010000
DEFAULT_LOAD_B = 0x08080000

FLAG_HAS_BL = 0x01
FLAG_SIGNED = 0x02


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def parse_ver(text: str) -> int:
    """'1.2.3' / '1.2.3.4' -> 32 位版本号（stage 可选）"""
    parts = [p for p in str(text).split(".") if p != ""]
    if not parts or len(parts) > 4:
        raise ValueError("版本号格式应为 a.b.c 或 a.b.c.d，收到 %r" % text)
    nums = []
    for p in parts:
        v = int(p, 0)
        if not 0 <= v <= 255:
            raise ValueError("版本号各段应在 0~255：%r" % text)
        nums.append(v)
    while len(nums) < 4:          # 补到 4 段（stage 缺省 0），返回时要用 nums[3]
        nums.append(0)
    return (nums[0] << 24) | (nums[1] << 16) | (nums[2] << 8) | nums[3]


def ver_str(fw_ver: int) -> str:
    a, b, c, d = (fw_ver >> 24) & 0xFF, (fw_ver >> 16) & 0xFF, (fw_ver >> 8) & 0xFF, fw_ver & 0xFF
    return "%d.%d.%d.%d" % (a, b, c, d)


def build_header(segs, pkg_size: int, fw_ver: int, build_id: int, flags: int) -> bytes:
    if not 1 <= len(segs) <= SEG_MAX:
        raise ValueError("段数必须在 1~%d" % SEG_MAX)

    body = struct.pack("<IHHIIIBBH", MAGIC, HDR_VER, HDR_SIZE, pkg_size,
                       fw_ver, build_id, len(segs), flags, 0)
    for i in range(SEG_MAX):
        if i < len(segs):
            load, data = segs[i]
            body += struct.pack("<III", load, len(data), crc32(data))
        else:
            body += struct.pack("<III", 0, 0, 0)
    body += b"\x00" * 28

    assert len(body) == HDR_SIZE - 4, "头部主体应为 %d 字节，实际 %d" % (HDR_SIZE - 4, len(body))
    return body + struct.pack("<I", crc32(body))


def assemble(segs, fw_ver: int, build_id: int, flags: int) -> bytes:
    """段数据紧跟头部，每段按 4 字节对齐"""
    payload = b""
    for _load, data in segs:
        payload += data
        pad = (-len(data)) % 4
        if pad:
            payload += b"\xFF" * pad          # 对齐填充；段 CRC 只覆盖真实数据
    pkg_size = HDR_SIZE + len(payload)
    return build_header(segs, pkg_size, fw_ver, build_id, flags) + payload


def verify_pkg(path: str) -> int:
    """重新打开产物、解析、复算 CRC —— 防止打包自身出错"""
    with open(path, "rb") as f:
        blob = f.read()

    if len(blob) < HDR_SIZE:
        print("[FAIL] 文件小于 %d 字节" % HDR_SIZE)
        return 1

    magic, hdr_ver, hdr_size, pkg_size, fw_ver, build_id, seg_count, flags, _r = \
        struct.unpack("<IHHIIIBBH", blob[:24])
    hdr_crc = struct.unpack("<I", blob[76:80])[0]

    problems = []
    if magic != MAGIC:
        problems.append("magic 不符")
    if hdr_ver != HDR_VER:
        problems.append("hdr_ver 不符")
    if hdr_size != HDR_SIZE:
        problems.append("hdr_size 不符")
    if pkg_size != len(blob):
        problems.append("pkg_size(%d) != 实际长度(%d)" % (pkg_size, len(blob)))
    if crc32(blob[:76]) != hdr_crc:
        problems.append("头部 CRC 不符")
    if not 1 <= seg_count <= SEG_MAX:
        problems.append("seg_count 非法")

    off = HDR_SIZE
    for i in range(seg_count):
        load, size, crc = struct.unpack("<III", blob[24 + i * 12:36 + i * 12])
        if off + size > len(blob):
            problems.append("seg[%d] 数据越界" % i)
            break
        if crc32(blob[off:off + size]) != crc:
            problems.append("seg[%d] 段 CRC 不符" % i)
        off += (size + 3) & ~3

    if problems:
        for p in problems:
            print("[FAIL] " + p)
        return 1
    print("[OK] 自校验通过：头部 CRC + 各段 CRC 均一致")
    return 0


def read_bin(path: str) -> bytes:
    """读镜像文件；缺文件/是目录都给一句人话，而不是 traceback"""
    if not os.path.isfile(path):
        raise SystemExit("[ERROR] 找不到镜像文件: %s" % path)
    with open(path, "rb") as f:
        data = f.read()
    if len(data) == 0:
        raise SystemExit("[ERROR] 镜像文件为空: %s" % path)
    return data


def dump_list(path: str) -> int:
    if not os.path.isfile(path):
        print("[ERROR] 找不到包文件: %s" % path)
        return 2
    with open(path, "rb") as f:
        blob = f.read()
    if len(blob) < HDR_SIZE:
        print("[ERROR] 文件不足 %d 字节，不是 .otapkg: %s" % (HDR_SIZE, path))
        return 2
    magic, hdr_ver, hdr_size, pkg_size, fw_ver, build_id, seg_count, flags, _r = \
        struct.unpack("<IHHIIIBBH", blob[:24])
    hdr_crc = struct.unpack("<I", blob[76:80])[0]

    print("文件      : %s (%d 字节)" % (os.path.basename(path), len(blob)))
    print("magic     : 0x%08X %s" % (magic, "'OTAP'" if magic == MAGIC else "<-- 非法"))
    print("hdr       : ver=%d size=%d hdr_crc=0x%08X" % (hdr_ver, hdr_size, hdr_crc))
    print("pkg_size  : %d" % pkg_size)
    print("fw_ver    : %s (0x%08X)" % (ver_str(fw_ver), fw_ver))
    print("build_id  : %d (%s)" % (build_id, time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(build_id))))
    print("seg_count : %d   flags=0x%02X%s%s" % (
        seg_count, flags,
        " 含BL镜像" if flags & FLAG_HAS_BL else "",
        " 已签名" if flags & FLAG_SIGNED else ""))
    off = HDR_SIZE
    for i in range(seg_count):
        load, size, crc = struct.unpack("<III", blob[24 + i * 12:36 + i * 12])
        print("  seg[%d]  : load=0x%08X size=%d (%.1f KB) crc32=0x%08X data_off=%d"
              % (i, load, size, size / 1024.0, crc, off))
        off += (size + 3) & ~3
    return verify_pkg(path)


def main() -> int:
    ap = argparse.ArgumentParser(
        description="把 bin 打成 .otapkg（OTA 固件包）",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--slot-a", metavar="BIN", help="槽 A 镜像（双段包）")
    ap.add_argument("--slot-b", metavar="BIN", help="槽 B 镜像（双段包）")
    ap.add_argument("--slot", choices=["a", "b"], help="单段包的目标槽")
    ap.add_argument("--bin", metavar="BIN", help="单段包的镜像文件")
    ap.add_argument("--load-a", default=hex(DEFAULT_LOAD_A), help="槽 A 链接地址（默认 0x08010000）")
    ap.add_argument("--load-b", default=hex(DEFAULT_LOAD_B), help="槽 B 链接地址（默认 0x08080000）")
    ap.add_argument("--ver", default="0.0.0", help="固件版本，如 1.2.3")
    ap.add_argument("--build-id", default="auto", help="构建 ID：auto / 十进制 / 0x 十六进制")
    ap.add_argument("--has-bl", action="store_true", help="标记包内含 BL 镜像")
    ap.add_argument("--signed", action="store_true", help="标记包已签名（本脚本不签名）")
    ap.add_argument("-o", "--out", metavar="PKG", help="输出 .otapkg")
    ap.add_argument("--list", metavar="PKG", help="查看并校验已有包")
    args = ap.parse_args()

    if args.list:
        return dump_list(args.list)

    segs = []
    load_a = int(args.load_a, 0)
    load_b = int(args.load_b, 0)
    if args.slot_a or args.slot_b:
        if args.slot_a:
            segs.append((load_a, read_bin(args.slot_a)))
        if args.slot_b:
            segs.append((load_b, read_bin(args.slot_b)))
    elif args.slot and args.bin:
        segs.append((load_a if args.slot == "a" else load_b, read_bin(args.bin)))
    else:
        ap.error("要么给 --slot-a/--slot-b（双段包），要么给 --slot {a,b} --bin（单段包）")

    if not args.out:
        ap.error("缺少 -o/--out")

    try:
        fw_ver = parse_ver(args.ver)
    except ValueError as e:
        ap.error(str(e))

    if args.build_id == "auto":
        build_id = int(time.time())
    else:
        build_id = int(args.build_id, 0) & 0xFFFFFFFF

    flags = 0
    if args.has_bl:
        flags |= FLAG_HAS_BL
    if args.signed:
        flags |= FLAG_SIGNED

    blob = assemble(segs, fw_ver, build_id, flags)

    out_dir = os.path.dirname(os.path.abspath(args.out))
    if out_dir and not os.path.isdir(out_dir):
        os.makedirs(out_dir, exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(blob)

    print("打包完成: %s" % args.out)
    print("  版本    : %s (0x%08X)  build_id=%d" % (ver_str(fw_ver), fw_ver, build_id))
    for i, (load, data) in enumerate(segs):
        print("  seg[%d]  : load=0x%08X size=%d (%.1f KB) crc32=0x%08X"
              % (i, load, len(data), len(data) / 1024.0, crc32(data)))
    print("  包大小  : %d 字节" % len(blob))

    # 产物自校验：重新打开、解析、复算 —— 打包自身出错必须在这里拦下
    return verify_pkg(args.out)


if __name__ == "__main__":
    sys.exit(main())
