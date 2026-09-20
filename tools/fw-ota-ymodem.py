#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
fw-ota-ymodem.py —— OTA 主机端：把固件包经 YMODEM 发给设备（OTA 服务端 / 发送侧）

配合 SDK 的 services.ota_src_uart（设备侧 YMODEM 接收）使用。设备侧是接收方，
本脚本是「服务端」：负责把 **raw .bin** 推下去（不再用自创的 .otapkg 格式——行业通用
的裸 bin 即可，设备侧自动选非运行槽写入）。

传输顺序（默认「元数据优先」，方案 2）：
  · 第 1 段：8 字节元数据文件 = size(u32 LE) + crc32(u32 LE)
    （crc32 与设备 ota_crc32 完全一致，即 CRC-32/ISO-HDLC = zlib.crc32）
  · 第 2 段：固件本体 raw .bin
  设备先收元数据 → 提前按 size 判目标区放得下、按 crc32 校验整段镜像（而非只做
  「内存 CRC == 闪存 CRC」自比，可发现 PC 端 bin 本身损坏）。`--no-meta` 退回单段 raw bin。

协议要点（必须与 SDK library/protocols/ymodem 的接收侧一致）:
  · 头包用 SOH(0x01) + 128 字节数据；数据包用 STX(0x02) + 1024 字节（默认）。
    **绝不能用 SOH 发 1024 字节**——接收侧 ymodem_check_frame 会按 128 解析，整包错位。
  · CRC16-XMODEM（poly 0x1021，初值 0，大端），与固件侧 ymodem_crc16 一致。
  · 收包方先发 'C'(0x43) 表示「用 CRC16、请开始」；本脚本等 'C' 后才发头包。
  · EOT → 等 ACK → 再发一个**空第 0 包**（SOH/128）收尾 → 等 ACK → 完成。
  · 收到 NAK / 超时 → 原样重发当前这包（不重新取数据）；收到 CAN CAN → 中止。
  · **'C' 是握手噪声，必须忽略**：接收侧在握手期会每 timeout_ms 重发 'C'，它不是
    「重发信号」。若在 'C' 上重发，会把多份头包字节喂进同一帧缓冲，导致对端
    ymodem_check_frame 永远 NAK（旧脚本死循环的根因）。

喂什么文件:
  · **直接传 raw .bin**（行业通用格式，如 build/ota_A/TP_MDC_A.bin）。
    设备侧 ota_flow 自动识别裸 bin（无 .otapkg 头），整段即镜像，按 YMODEM 头包里的
    文件大小判「目标区放得下」，并自动选非运行槽写入。
  · 头包里的文件名仅作展示，设备只看文件大小判「目标区放得下」。
  · 旧的 .otapkg 仍兼容（设备侧按 magic 自动识别），但新流程不再需要 fw-ota-pack.py 打包。

依赖: pyserial  (pip install pyserial)；--gui 另需 tkinter（通常随 Python 自带）。

用法:
  python fw-ota-ymodem.py dist/TP_MDC_A.bin              # 直接发 raw .bin（推荐，设备自动选槽）
  python fw-ota-ymodem.py --port COM13 --baud 115200 dist/TP_MDC_A.bin
  python fw-ota-ymodem.py --no-trigger --packet 128 dist/TP_MDC_A.bin   # 128 字节小包模式
  python fw-ota-ymodem.py --gui                                        # 弹文件选择框（默认过滤 .bin）
   python fw-ota-ymodem.py --gui --no-autoslot                          # 关掉自动挑槽（就发所选那份）

  ★ 自动挑槽（默认开）：设备收到触发字节后会回报一行 `#OTA target=A|B`（它自己用
    ota_area_select_target 算，会自动避开当前运行槽），脚本据此在同目录换成对应的那份
    镜像（TP_MDC.bin <-> TP_MDC_A.bin <-> TP_MDC_B.bin）。所以弹窗里**随便选一个**都行，
    「该发哪个槽」由设备决定，不靠人挑，更不写死地址。
"""
import argparse
import os
import re
import struct
import sys
import time
import zlib

# 协议常量（与 library/protocols/ymodem/Inc/ymodem.h 对齐）
SOH = 0x01
STX = 0x02
EOT = 0x04
ACK = 0x06
NAK = 0x15
CAN = 0x18
C   = 0x43

DATA_128 = 128
DATA_1K  = 1024


def crc16(data: bytes) -> int:
    """CRC16-XMODEM：poly 0x1021，初值 0，与固件侧 ymodem_crc16 完全一致。"""
    crc = 0
    for b in data:
        crc ^= (b << 8)
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def build_frame(seq: int, data: bytes) -> bytes:
    """组一帧：SOH/STX 由 data 长度决定（128→SOH，1024→STX）。"""
    if len(data) == DATA_128:
        kind = SOH
    elif len(data) == DATA_1K:
        kind = STX
    else:
        raise ValueError("数据长度必须是 128 或 1024，收到 %d" % len(data))
    pkt = bytearray()
    pkt.append(kind)
    pkt.append(seq & 0xFF)
    pkt.append((0xFF - seq) & 0xFF)
    pkt += data
    crc = crc16(data)
    pkt.append((crc >> 8) & 0xFF)
    pkt.append(crc & 0xFF)
    return bytes(pkt)


def wait_c(ser, timeout: float) -> bool:
    """等设备的 'C'（握手开始）；超时返回 False。"""
    t0 = time.time()
    while time.time() - t0 < timeout:
        b = ser.read(1)
        if b == bytes([C]):
            return True
    return False


def send_packet(ser, seq, data, pkt_timeout, retry, label, debug=False):
    """发一包，等 ACK/NAK/CAN；超时或 NAK 时原样重发（最多 retry 次）。
    返回 'ok' / 'abort'。注意：重发时 seq 不变（不重新取数据）。

    关键修正：接收侧在握手期会**周期性重发 'C'**（每 timeout_ms 一次，见
    library/protocols/ymodem 的 recv_resend），这是噪声，绝不是「重发信号」。
    所以这里**忽略 'C'**（以及其它非 ACK/NAK/CAN 的控制字节），只认：
      · ACK   → 本包成功，推进
      · NAK   → 对端没收到，原样重发
      · CAN CAN → 对端中止
      · 超时  → 原样重发
    若把 'C' 当重发，会一边收 'C' 一边重发，把多份头包字节喂进同一帧缓冲，
    导致对端 ymodem_check_frame 永远 NAK（这正是之前死循环的根因）。
    """
    frame = build_frame(seq, data)
    if debug:
        print("  [DBG] %s seq=%d frame(%d)=%s" % (label, seq, len(frame), frame.hex()))
    for attempt in range(retry + 1):
        ser.write(frame)
        if debug:
            print("  [DBG] %s seq=%d -> 已发 %d 字节" % (label, seq, len(frame)))
        t0 = time.time()
        while time.time() - t0 < pkt_timeout:
            b = ser.read(1)
            if not b:
                continue
            v = b[0]
            if v == ACK:
                if debug:
                    print("  [DBG] %s seq=%d <- ACK" % (label, seq))
                return "ok"
            if v == CAN:
                b2 = ser.read(1)
                if b2 and b2[0] == CAN:
                    print("[ABORT] 收到 CAN CAN，对端已中止")
                    return "abort"
                continue  # 单个 CAN 后可能紧跟第二个，继续等
            if v == NAK:
                if debug:
                    print("  [DBG] %s seq=%d <- NAK" % (label, seq))
                if attempt < retry:
                    print("  [WARN] %s 包 seq=%d 收到 NAK，原样重发" % (label, seq))
                    break  # 跳出内层，外层再发同一包
                print("[FAIL] %s 包 seq=%d 重试用尽" % (label, seq))
                return "abort"
            # 'C'（握手噪声 / 旧 lrzsz 风格）及其它控制字节：忽略，继续等 ACK/NAK
            if debug:
                print("  [DBG] %s seq=%d <- 0x%02X(忽略)" % (label, seq, v))
            continue
        # 内层超时（没收到任何响应）→ 外层循环再发
        if attempt < retry:
            print("  [WARN] %s 包 seq=%d 响应超时，原样重发" % (label, seq))
            continue
        print("[FAIL] %s 包 seq=%d 响应超时（重试用尽）" % (label, seq))
        return "abort"
    return "abort"


def ymodem_send(ser, filepath, packet=DATA_1K, trigger=b'U', wait_trigger=1.0,
                c_timeout=10.0, pkt_timeout=5.0, retry=10, post_c_delay=0.05,
                debug=False):
    filename = os.path.basename(filepath)
    filesize = os.path.getsize(filepath)
    print("[INFO] 文件 %s  大小 %d 字节 (%.1f KB)" % (filename, filesize, filesize / 1024.0))

    # 可选：触发设备进入 OTA 接收（如 APP 的 'U' 命令）
    if trigger:
        ser.write(trigger)
        print("[INFO] 已发送触发字节 %r，等待 %g s" % (trigger, wait_trigger))
        time.sleep(wait_trigger)

    # 等设备发 'C'
    print("[INFO] 等待设备 'C'（握手）…")
    if not wait_c(ser, c_timeout):
        print("[FAIL] 超时未收到 'C' —— 设备没进入 YMODEM 接收（检查触发字节 / 串口 / 波特率）")
        return 1
    print("[INFO] 收到 'C'，开始发送")

    # 接收侧在握手期会周期重发 'C'（噪声）。等一小会把这些缓冲里的 'C' 清掉，
    # 避免它们被当成头包的响应；即便没清干净，send_packet 也会忽略 'C'。
    if post_c_delay > 0:
        time.sleep(post_c_delay)
    while True:
        b = ser.read(1)
        if b and b[0] == C:
            continue
        break

    # 头包（SOH/128）：文件名 + '\0' + 十进制大小
    hdr = bytearray()
    hdr += filename.encode()
    hdr.append(0)
    hdr += str(filesize).encode()
    hdr = hdr.ljust(DATA_128, b'\x00')
    if send_packet(ser, 0, bytes(hdr), pkt_timeout, retry, "头", debug) != "ok":
        return 1

    # 数据包
    seq = 1
    sent = 0
    with open(filepath, "rb") as f:
        while True:
            chunk = f.read(packet)
            if not chunk:
                break
            if len(chunk) < packet:
                chunk = chunk.ljust(packet, b'\x1A')  # 末包填充 0x1A
            if send_packet(ser, seq, chunk, pkt_timeout, retry, "数据") != "ok":
                return 1
            sent += len(chunk)
            shown = min(sent, filesize)
            pct = (shown * 100 // filesize) if filesize else 100
            print("  [SEND] seq=%d  %d/%d 字节 (%d%%)" % (seq, shown, filesize, pct))
            seq += 1

    # 结束：EOT → 等 ACK → 空第 0 包（SOH/128）收尾 → 等 ACK
    ser.write(bytes([EOT]))
    t0 = time.time()
    got_eot_ack = False
    while time.time() - t0 < pkt_timeout:
        b = ser.read(1)
        if not b:
            continue
        v = b[0]
        if v == ACK:
            got_eot_ack = True
            break
        if v == CAN:
            print("[ABORT] EOT 后收到 CAN")
            return 1
        # 'C' 等噪声忽略，继续等 ACK
        continue
    if not got_eot_ack:
        print("[WARN] EOT 后未及时收到 ACK（仍发收尾包）")

    final = bytes(DATA_128)  # 全 0 的空第 0 包
    if send_packet(ser, 0, final, pkt_timeout, retry, "收尾", debug) != "ok":
        return 1

    print("[OK] 发送完成")
    return 0


def send_meta_first(ser, filepath, packet=DATA_1K, trigger=b'U', wait_trigger=1.0,
                    c_timeout=10.0, pkt_timeout=5.0, retry=10, post_c_delay=0.05,
                    debug=False, autoslot=True):
    """
    元数据优先模式（--meta）：先发一个 8 字节元数据文件（size(u32 LE) + crc32(u32 LE)），
    再发真正的 bin。设备先收元数据、记住「期望大小 + 期望 CRC32」，随后收 bin 时即可：
      · 提前按 size 判目标区放得下（不必等整包收完才发觉空间不足）；
      · 收完按记下的 CRC32 校验（而非仅「内存 CRC == 闪存 CRC」自比），
        从而能发现 PC 端 bin 本身已损坏的情况。
    CRC32 与设备侧 ota_crc32 完全一致（CRC-32/ISO-HDLC = zlib.crc32）。
    注意：设备固件需支持「先收元数据」模式（两段 YMODEM）；未支持时请用普通模式。

    自动挑槽（默认开，--no-autoslot 关）：发完触发字节后设备会回报 `#OTA target=A|B`，
    本函数据此在同目录换成对应后缀的那一份镜像 —— 操作上「随便选一个 .bin」即可。
    """
    # ---- 1) 触发设备，并（默认）让它自报目标槽 -> 自动换成该槽对应的那一份镜像 ----
    if trigger:
        ser.write(trigger)
        print("[INFO] 已发送触发字节 %r，等待 %g s" % (trigger, wait_trigger))
        time.sleep(wait_trigger)
        if autoslot:
            filepath = pick_target_fw(ser, filepath)

    # ---- 2) 按「实际要发的那一份」算 size / crc32（上面可能换过文件）----
    size = os.path.getsize(filepath)
    crc = 0
    with open(filepath, "rb") as f:
        while True:
            chunk = f.read(4096)
            if not chunk:
                break
            crc = zlib.crc32(chunk, crc)
    crc &= 0xFFFFFFFF
    print("[INFO] 固件 size=0x%X (%d B)  crc32=0x%08X" % (size, size, crc))

    import tempfile
    meta = struct.pack("<II", size, crc)          # size(u32 LE) + crc32(u32 LE)
    tf = tempfile.NamedTemporaryFile(suffix=".meta", delete=False)
    tf.write(meta)
    tf.close()
    kw = dict(packet=packet, trigger=b"", wait_trigger=0.0,
              c_timeout=c_timeout, pkt_timeout=pkt_timeout, retry=retry,
              post_c_delay=post_c_delay, debug=debug)
    try:
        print("[INFO] 即将发送: %s" % filepath)
        print("[INFO] >>> 第 1 段：元数据 (size + crc32)")
        if ymodem_send(ser, tf.name, **kw) != 0:
            return 1
        print("[INFO] >>> 第 2 段：固件本体 bin")
        return ymodem_send(ser, filepath, **kw)
    finally:
        try:
            os.unlink(tf.name)
        except OSError:
            pass


def read_slot_announce(ser, timeout=3.0):
    """
    读设备回报的一行 `#OTA target=X`（ASCII，以 '\\n' 结束）。
    读到 '\\n' 立即返回，**绝不多读一个字节** —— 设备紧接着要发 YMODEM 的握手 'C'，
    多读就会把它吞掉。返回 'A'/'B'；设备答了但目标槽非法（无槽可写）返回 ''；
    设备没回报（旧固件）返回 None。
    """
    t0 = time.time()
    buf = b""
    while (time.time() - t0) < timeout:
        ch = ser.read(1)
        if not ch:
            continue
        buf += ch
        if ch == b"\n":
            line = buf.decode("ascii", "replace").strip()
            buf = b""
            if not line.startswith("#OTA"):
                continue                     # 噪声/回显，继续等公告
            m = re.search(r"target\s*=\s*([AB])", line)
            return m.group(1) if m else ""
    return None


def find_sibling_fw(filepath, slot):
    """
    在同目录里找「目标槽」那一份镜像：
        TP_MDC.bin   + 槽 B -> TP_MDC_B.bin
        TP_MDC_A.bin + 槽 B -> TP_MDC_B.bin     （先剥掉已有的 _A/_B 后缀）
    找不到返回 None —— 调用方必须报错，绝不随便发一份地址不匹配的镜像（会写坏一个槽）。
    """
    d = os.path.dirname(os.path.abspath(filepath))
    b = os.path.basename(filepath)
    stem = re.sub(r"_[AB](\.[^.]*)$", r"\1", b)
    cand = os.path.join(d, re.sub(r"(\.[^.]*)$", r"_%s\1" % slot, stem))
    return cand if os.path.isfile(cand) else None


def pick_target_fw(ser, filepath, timeout=3.0):
    """
    问设备「你要收哪个槽」，据此自动挑同目录的另一份镜像。
    这是「不写死地址、不手动选分区」的最后一环：目标槽由设备侧的
    ota_area_select_target() 算（自动避开当前运行槽），主机只负责把对应文件找出来。
    """
    slot = read_slot_announce(ser, timeout)
    if slot is None:
        print("[INFO] 设备未回报槽位（旧固件？）-> 按所选文件原样发送")
        return filepath
    if slot == "":
        sys.exit("[ota_ymodem] 设备自报「没有可写入的目标槽」—— 这份固件可能不是槽内固件")
    target = find_sibling_fw(filepath, slot)
    if target is None:
        sys.exit("[ota_ymodem] 设备要槽 %s 的镜像，但 %s 同目录里没有 *_%s.bin\n"
                 "             请先用 VS Code 的 Build 产出三分片（TP_MDC / TP_MDC_A / TP_MDC_B）"
                 % (slot, os.path.dirname(os.path.abspath(filepath)), slot))
    print("[OK] 设备要槽 %s -> 自动选用 %s" % (slot, os.path.basename(target)))
    return target


def main() -> int:
    ap = argparse.ArgumentParser(
        description="OTA 主机端：经 YMODEM 把固件包发给设备（配合 SDK services.ota_src_uart）",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file", nargs="?", help="要发送的固件（优先 .bin；也可 .otapkg）")
    ap.add_argument("-p", "--port", default="COM1", help="串口（默认 COM1）")
    ap.add_argument("-b", "--baud", type=int, default=115200, help="波特率（默认 115200）")
    ap.add_argument("--packet", type=int, default=1024, choices=[128, 1024],
                    help="数据包长度（默认 1024=STX；128=SOH 小包）")
    ap.add_argument("--trigger", default="U",
                    help="发送前先发的触发字节（默认 'U'；--no-trigger 跳过）")
    ap.add_argument("--no-trigger", action="store_true", help="不发送触发字节")
    ap.add_argument("--trigger-delay", type=float, default=1.0, help="发触发字节后的等待（默认 1.0 s）")
    ap.add_argument("--c-timeout", type=float, default=10.0, help="等 'C' 超时（默认 10 s）")
    ap.add_argument("--pkt-timeout", type=float, default=5.0, help="每包 ACK 超时（默认 5 s）")
    ap.add_argument("--retry", type=int, default=10, help="每包最大重发次数（默认 10）")
    ap.add_argument("--post-c-delay", type=float, default=0.05,
                    help="收到握手 'C' 后、发头包前的静默等待（默认 0.05 s），"
                         "让设备清掉缓冲里的握手噪声 'C'")
    ap.add_argument("--debug", action="store_true",
                    help="打印每帧十六进制及接收到的原始响应字节，便于排查对端 NAK 真因")
    ap.add_argument("--no-meta", dest="meta", action="store_false",
                    help="关闭「元数据优先」（默认开）：不发 size+crc32 元数据、直接发 raw bin。"
                         "仅当设备固件是旧版（不支持「先收元数据」）时才需要")
    ap.set_defaults(meta=True)
    ap.add_argument("--gui", action="store_true", help="未给 file 时用文件选择框（需 tkinter）")
    ap.add_argument("--no-autoslot", dest="autoslot", action="store_false",
                    help="关闭「按设备自报的目标槽自动挑同目录镜像」（默认开）。"
                         "设备不回报槽位时本开关无影响（本来就会回退为原样发送）")
    ap.set_defaults(autoslot=True)
    args = ap.parse_args()

    filepath = args.file
    if not filepath:
        if args.gui:
            try:
                from tkinter import Tk, filedialog
            except ImportError:
                sys.exit("[ota_ymodem] 需要 tkinter 才能用 --gui；或改用 --file 指定文件")
            Tk().withdraw()
            filepath = filedialog.askopenfilename(
                title="选择 OTA 固件（优先 .bin）",
                filetypes=[("BIN 固件", "*.bin"), ("OTA 包", "*.otapkg"), ("All", "*.*")])
            if not filepath:
                sys.exit("[ota_ymodem] 未选择文件")
        else:
            sys.exit("用法: fw-ota-ymodem.py <file> [--port COMx] [--baud 115200] [--gui]")

    if not os.path.isfile(filepath):
        sys.exit("[ota_ymodem] 找不到文件: %s" % filepath)

    if args.no_trigger or not args.trigger:
        trigger = b''
    else:
        trigger = args.trigger.encode()
        if len(trigger) != 1:
            sys.exit("[ota_ymodem] --trigger 只能是一个字节")

    try:
        import serial
    except ImportError:
        sys.exit("[ota_ymodem] 需要 pyserial：pip install pyserial")

    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.1)
    except Exception as e:
        sys.exit("[ota_ymodem] 打不开串口 %s: %s" % (args.port, e))

    print("[OK] 串口 %s @ %d 已打开" % (args.port, args.baud))
    if args.meta:
        rc = send_meta_first(ser, filepath, packet=args.packet, trigger=trigger,
                             wait_trigger=args.trigger_delay, c_timeout=args.c_timeout,
                             pkt_timeout=args.pkt_timeout, retry=args.retry,
                             post_c_delay=args.post_c_delay, debug=args.debug,
                             autoslot=args.autoslot)
    else:
        rc = ymodem_send(ser, filepath, packet=args.packet, trigger=trigger,
                         wait_trigger=args.trigger_delay, c_timeout=args.c_timeout,
                         pkt_timeout=args.pkt_timeout, retry=args.retry,
                         post_c_delay=args.post_c_delay, debug=args.debug)
    ser.close()
    return rc


if __name__ == "__main__":
    sys.exit(main())
