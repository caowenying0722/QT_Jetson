#!/usr/bin/env python3
"""PTY module emulator. A and B are synthetic tokens, never fingerprint images."""

import os
import pty
import select
import struct
import threading
import tty

from fingerprint_uart import ACK, COMMAND, HEADER, packet


class FingerprintSimulator:
    capacity = 200

    def __init__(self):
        self.master, self.slave = pty.openpty()
        tty.setraw(self.slave)
        self.port = os.ttyname(self.slave)
        self.finger = ""
        self.templates = {}
        self.buffers = {}
        self.image = ""
        self.bad_checksum = False
        self.stopped = threading.Event()
        self.thread = threading.Thread(target=self.serve)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *_):
        self.stopped.set()
        self.thread.join()
        os.close(self.master)
        os.close(self.slave)

    def serve(self):
        received = bytearray()
        while not self.stopped.is_set():
            if not select.select([self.master], [], [], 0.1)[0]:
                continue
            received.extend(os.read(self.master, 256))
            while len(received) >= 9:
                kind, length = struct.unpack_from(">BH", received, 6)
                if received[:6] != HEADER or kind != COMMAND or not 3 <= length <= 256:
                    del received[0]
                    continue
                end = 9 + length
                if len(received) < end:
                    break
                body = received[6:end - 2]
                checksum = struct.unpack_from(">H", received, end - 2)[0]
                del received[:end]
                result = self.reply(body[3], body[4:]) if sum(body) & 0xFFFF == checksum else b"\x01"
                reply = packet(ACK, result)
                if self.bad_checksum:
                    self.bad_checksum = False
                    reply = reply[:-1] + bytes((reply[-1] ^ 1,))
                # 分两次发送，覆盖串口半包读取。
                os.write(self.master, reply[:5])
                os.write(self.master, reply[5:])

    def reply(self, opcode, args):
        if opcode == 0x13 and args == bytes(4):
            return b"\x00"
        if opcode == 0x0F and not args:
            return b"\x00" + struct.pack(">HHHHIHH", 0, 0, self.capacity, 3, 0xFFFFFFFF, 2, 6)
        if opcode == 0x1F and len(args) == 1:
            table = bytearray(32)
            for slot in self.templates:
                if slot // 256 == args[0]:
                    bit = slot % 256
                    table[bit // 8] |= 1 << (bit % 8)
            return b"\x00" + table
        if opcode == 0x01 and not args:
            self.image = self.finger
            return b"\x00" if self.image else b"\x02"
        if opcode == 0x02 and args in (b"\x01", b"\x02"):
            if not self.image:
                return b"\x06"
            self.buffers[args[0]] = self.image
            return b"\x00"
        if opcode == 0x05 and not args:
            first = self.buffers.get(1)
            return b"\x00" if first and first == self.buffers.get(2) else b"\x0a"
        if opcode == 0x06 and len(args) == 3:
            buffer, slot = struct.unpack(">BH", args)
            if slot >= self.capacity or slot in self.templates or buffer not in self.buffers:
                return b"\x0b"
            self.templates[slot] = self.buffers[buffer]
            return b"\x00"
        if opcode == 0x04 and len(args) == 5:
            buffer, start, count = struct.unpack(">BHH", args)
            for slot, token in self.templates.items():
                if start <= slot < start + count and self.buffers.get(buffer) == token:
                    return b"\x00" + struct.pack(">HH", slot, 180)
            return b"\x09"
        return b"\x01"


def main():
    with FingerprintSimulator() as emulator:
        print(f"PORT={emulator.port}", flush=True)
        print("模拟指纹：输入 A 或 B 表示按压，空行表示松开，q 退出。", flush=True)
        for line in iter(input, "q"):
            token = line.strip().upper()
            if token in ("A", "B", ""):
                emulator.finger = token
                print(f"当前：{token or '已松开'}", flush=True)
            else:
                print("请输入 A、B 或空行。", flush=True)


if __name__ == "__main__":
    try:
        main()
    except (EOFError, KeyboardInterrupt):
        pass
