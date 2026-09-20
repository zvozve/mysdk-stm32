#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
fw_ota_ymodem.py 的回环自测：把 SDK library/protocols/ymodem 的接收侧
（check_frame / recv_feed / handle_header / handle_data / handle_eot /
recv_resend）忠实地移植成内存接收引擎，与 tools/ota_ymodem.py 的发送侧
用内存串口对接，验证「头包不被 NAK、整包走完 DONE」。

仅用于 CI / 本地回归，不进发布。运行：
  python tools/_fw_ota_ymodem_loopback_test.py
"""
import os
import sys
import time
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fw_ota_ymodem as S  # 待测发送侧

# ---- 协议常量（与 ymodem.h 对齐）----
SOH, STX, EOT, ACK, NAK, CAN, C = 0x01, 0x02, 0x04, 0x06, 0x15, 0x18, 0x43
DATA_128, DATA_1K, OVERHEAD = 128, 1024, 5


def crc16(data):
    crc = 0
    for b in data:
        crc ^= (b << 8)
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
    return crc


def check_frame(frame, frame_len):
    """移植 ymodem_check_frame：返回 (seq, data, dlen) 或 None。"""
    if frame_len < (OVERHEAD + DATA_128):
        return None
    if frame[0] == SOH:
        dlen = DATA_128
    elif frame[0] == STX:
        dlen = DATA_1K
    else:
        return None
    if frame_len != (dlen + OVERHEAD):
        return None
    if ((frame[1] + frame[2]) & 0xFF) != 0xFF:
        return None
    crc_rx = (frame[3 + dlen] << 8) | frame[3 + dlen + 1]
    if crc_rx != crc16(frame[3:3 + dlen]):
        return None
    return (frame[1], frame[3:3 + dlen], dlen)


class VSerial:
    """内存串口：sender.write -> rx（接收引擎读）；接收引擎写 -> tx（sender 读）。"""
    def __init__(self):
        self.rx = bytearray()
        self.tx = bytearray()

    def write(self, data):
        self.rx.extend(data)
        return 0

    def read(self, n):
        if self.tx:
            return bytes([self.tx.pop(0)])
        return b''


class RecvEngine:
    """忠实移植 ymodem.c 接收状态机（去掉了 on_header/on_data 回调，直接收数据）。"""
    def __init__(self, vs):
        self.vs = vs
        self.frame = bytearray(OVERHEAD + DATA_1K)
        self.fill = 0
        self.expect = 0
        self.last_resp = 0
        self.can_cnt = 0
        self.has_header = 0
        self.has_last = 0
        self.last_seq = 0
        self.state = 'HANDSHAKE'
        self.t_ref = 0.0
        self.retry = 0
        self.timeout_ms = 1000
        self.retry_max = 10
        self.file_size = 0
        self.got = 0
        self.pkts = 0
        self._touch()
        self.tx(C)  # ymodem_recv_start 起手发 'C'

    def _touch(self):
        self.t_ref = time.time() * 1000.0
        self.retry = 0

    def tx(self, b):
        self.last_resp = b
        self.vs.tx.append(b)

    def _ack(self):
        """发 ACK 并按 SDK 行为复位超时基准（recv_touch）。"""
        self.tx(ACK)
        self._touch()

    def _read(self):
        return self.vs.rx.pop(0) if self.vs.rx else None

    def _recv_feed(self):
        while self.state not in ('DONE', 'FAILED'):
            if self.fill == 0:
                b = self._read()
                if b is None:
                    break
                if b == EOT:
                    self._eot()
                    continue
                if b == CAN:
                    self.can_cnt += 1
                    if self.can_cnt >= 2:
                        self.state = 'FAILED'
                    continue
                if b == SOH:
                    self.expect = OVERHEAD + DATA_128
                elif b == STX:
                    self.expect = OVERHEAD + DATA_1K
                else:
                    continue  # 噪声丢弃
            else:
                b = self._read()
                if b is None:
                    break
            self.can_cnt = 0
            self.frame[self.fill] = b
            self.fill += 1
            if self.fill == self.expect:
                self._on_frame()
                self.fill = 0
                self.expect = 0
                return

    def _on_frame(self):
        r = check_frame(self.frame, self.fill)
        if r is None:
            self.tx(NAK)
            return
        seq, data, dlen = r
        if self.has_header == 0 and seq == 0:
            # parse_header
            name = bytearray()
            i = 0
            while i < dlen and data[i] != 0:
                name.append(data[i])
                i += 1
            i += 1
            sz = 0
            while i < dlen and 0x30 <= data[i] <= 0x39:
                sz = sz * 10 + (data[i] - 0x30)
                i += 1
            self.file_size = sz
            self.has_header = 1
            self.has_last = 0
            self.state = 'DATA'
            self._ack()
            return
        if self.state == 'FINAL' and seq == 0:
            self._ack()
            self.state = 'DONE'
            return
        if self.state != 'DATA':
            self.tx(NAK)
            return
        if self.has_last and seq == self.last_seq:
            self._ack()
            return
        if self.has_last:
            if seq != ((self.last_seq + 1) & 0xFF):
                self.tx(NAK)
                return
        elif seq != 1:
            self.tx(NAK)
            return
        self.got += (min(dlen, self.file_size - self.got) if self.file_size else dlen)
        self.last_seq = seq
        self.has_last = 1
        self.pkts += 1
        self._ack()

    def _eot(self):
        if self.has_header == 0:
            self._ack()
            return
        self._ack()
        if self.state == 'DATA':
            self.state = 'FINAL'

    def process(self):
        if self.state in ('DONE', 'FAILED'):
            return
        self._recv_feed()
        if self.state in ('DONE', 'FAILED'):
            return
        if (time.time() * 1000.0 - self.t_ref) >= self.timeout_ms:
            self.retry += 1
            if self.retry > self.retry_max:
                self.state = 'FAILED'
                return
            if self.state == 'HANDSHAKE' or self.last_resp == 0:
                self.tx(C)  # 握手噪声 'C'
            else:
                self.tx(self.last_resp)


def make_fake_pkg(path, size):
    with open(path, 'wb') as f:
        f.write(b'\xAA' * size)


def main():
    tmp = os.path.join(HERE, '_fake_app.otapkg')
    total = 83136  # 与用户日志里的大小一致
    make_fake_pkg(tmp, total)

    vs = VSerial()
    rec = RecvEngine(vs)
    result = {}

    def run_sender():
        # 用 1024 包长度、无触发延迟（回环里不需要）；校验真实大小
        result['rc'] = S.ymodem_send(
            vs, tmp, packet=1024, trigger=b'U', wait_trigger=0.0,
            c_timeout=5.0, pkt_timeout=2.0, retry=5,
            post_c_delay=0.0, debug=False)

    th = threading.Thread(target=run_sender)
    th.start()
    import itertools
    last_got = -1
    stall = 0
    for _ in itertools.count():
        if rec.state in ('DONE', 'FAILED'):
            break
        rec.process()
        time.sleep(0.0005)
        if rec.got == last_got:
            stall += 1
            if stall == 200:  # ~100ms 无进展 → 打印诊断
                print("[DIAG] 卡住: state=%s got=%d last_seq=%d fill=%d expect=%d "
                      "rx_head=%s tx=%s"
                      % (rec.state, rec.got, rec.last_seq, rec.fill, rec.expect,
                         bytes(rec.vs.rx[:32]).hex(), bytes(rec.vs.tx[:8]).hex()))
                break
        else:
            stall = 0
            last_got = rec.got
    th.join()

    ok = (result.get('rc') == 0 and rec.state == 'DONE' and rec.got == total)
    print("sender rc   =", result.get('rc'))
    print("recv state  =", rec.state)
    print("recv got    =", rec.got, "/", total)
    print("recv pkts   =", rec.pkts)
    print("RESULT      =", "PASS" if ok else "FAIL")
    os.remove(tmp)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
