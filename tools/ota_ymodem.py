#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ota_ymodem.py —— OTA 主机端：把固件包经 YMODEM 发给设备（OTA 服务端 / 发送侧）

配合 SDK 的 services.ota_src_uart（设备侧 YMODEM 接收）使用。设备侧是接收方，
本脚本是「服务端」：负责把 .otapkg 推下去。

协议要点（必须与 SDK library/protocols/ymodem 的接收侧一致）:
  · 头包用 SOH(0x01) + 128 字节数据；数据包用 STX(0x02) + 1024 字节（默认）。
    **绝不能用 SOH 发 1024 字节**——接收侧 ymodem_check_frame 会按 128 解析，整包错位。
  · CRC16-XMODEM（poly 0x1021，初值 0，大端），与固件侧 ymodem_crc16 一致。
  · 收包方先发 'C'(0x43) 表示「用 CRC16、请开始」；本脚本等 'C' 后才发头包。
  · EOT → 等 ACK → 再发一个**空第 0 包**（SOH/128）收尾 → 等 ACK → 完成。
  · 收到 NAK / 额外的 'C' 就原样重发当前这包（不重新取数据）；收到 CAN CAN 即中止。

喂什么文件:
  · 设备侧 ota_flow 按 services/ota_core 的包格式解析，所以**传 ota_pack.py 打好的
    .otapkg**（串口是流式源，建议单段包：ota_pack.py --slot a --bin ...）。
  · 头包里的文件名仅作展示，设备只看文件大小判「目标区放得下」。

依赖: pyserial  (pip install pyserial)；--gui 另需 tkinter（通常随 Python 自带）。

用法:
  python ota_ymodem.py dist/app.otapkg
  python ota_ymodem.py --port COM13 --baud 115200 dist/app.otapkg
  python ota_ymodem.py --no-trigger --packet 128 dist/app.otapkg   # 128 字节小包模式
  python ota_ymodem.py --gui                                        # 弹文件选择框
"""
import argparse
import os
import sys
import time

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


def send_packet(ser, seq, data, pkt_timeout, retry, label):
    """发一包，等 ACK/NAK/C/CAN；超时或 NAK/C 时原样重发（最多 retry 次）。
    返回 'ok' / 'abort'。注意：重发时 seq 不变（不重新取数据）。"""
    frame = build_frame(seq, data)
    for attempt in range(retry + 1):
        ser.write(frame)
        t0 = time.time()
        while time.time() - t0 < pkt_timeout:
            b = ser.read(1)
            if not b:
                continue
            v = b[0]
            if v == ACK:
                return "ok"
            if v == CAN:
                b2 = ser.read(1)
                if b2 and b2[0] == CAN:
                    print("[ABORT] 收到 CAN CAN，对端已中止")
                    return "abort"
                continue  # 单个 CAN 后可能紧跟第二个，继续等
            if v == NAK or v == C:
                if attempt < retry:
                    print("  [WARN] %s 包 seq=%d 收到 %s，原样重发"
                          % (label, seq, "NAK" if v == NAK else "C"))
                    break  # 跳出内层，外层再发同一包
                print("[FAIL] %s 包 seq=%d 重试用尽" % (label, seq))
                return "abort"
        # 内层超时（没收到任何响应）→ 外层循环再发
        if attempt < retry:
            print("  [WARN] %s 包 seq=%d 响应超时，原样重发" % (label, seq))
            continue
        print("[FAIL] %s 包 seq=%d 响应超时（重试用尽）" % (label, seq))
        return "abort"
    return "abort"


def ymodem_send(ser, filepath, packet=DATA_1K, trigger=b'U', wait_trigger=1.0,
                c_timeout=10.0, pkt_timeout=5.0, retry=10):
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

    # 头包（SOH/128）：文件名 + '\0' + 十进制大小
    hdr = bytearray()
    hdr += filename.encode()
    hdr.append(0)
    hdr += str(filesize).encode()
    hdr = hdr.ljust(DATA_128, b'\x00')
    if send_packet(ser, 0, bytes(hdr), pkt_timeout, retry, "头") != "ok":
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
        if b and b[0] == ACK:
            got_eot_ack = True
            break
        if b and b[0] == CAN:
            print("[ABORT] EOT 后收到 CAN")
            return 1
    if not got_eot_ack:
        print("[WARN] EOT 后未及时收到 ACK（仍发收尾包）")

    final = bytes(DATA_128)  # 全 0 的空第 0 包
    if send_packet(ser, 0, final, pkt_timeout, retry, "收尾") != "ok":
        return 1

    print("[OK] 发送完成")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(
        description="OTA 主机端：经 YMODEM 把固件包发给设备（配合 SDK services.ota_src_uart）",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file", nargs="?", help="要发送的 .otapkg（或原始 bin）")
    ap.add_argument("-p", "--port", default="COM13", help="串口（默认 COM13）")
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
    ap.add_argument("--gui", action="store_true", help="未给 file 时用文件选择框（需 tkinter）")
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
                title="选择 OTA 固件包",
                filetypes=[("OTA 包", "*.otapkg"), ("BIN", "*.bin"), ("All", "*.*")])
            if not filepath:
                sys.exit("[ota_ymodem] 未选择文件")
        else:
            sys.exit("用法: ota_ymodem.py <file> [--port COMx] [--baud 115200] [--gui]")

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
    rc = ymodem_send(ser, filepath, packet=args.packet, trigger=trigger,
                     wait_trigger=args.trigger_delay, c_timeout=args.c_timeout,
                     pkt_timeout=args.pkt_timeout, retry=args.retry)
    ser.close()
    return rc


if __name__ == "__main__":
    sys.exit(main())
