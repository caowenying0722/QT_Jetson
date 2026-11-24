#!/usr/bin/env python3
"""Jetson 上的 PAJ7620U2 读取助手。每行输出一个 JSON 事件。"""

import argparse
import ctypes
import fcntl
import json
import os
import time


ADDRESS = 0x73
CHIP_ID = bytes((0x20, 0x76))
BANK = 0xEF
RESULT = 0x43
I2C_RDWR = 0x0707
I2C_M_RD = 0x0001

# 初始化表来自 Seeed-Studio/Grove_Gesture 的 src/paj7620.h。
# https://github.com/Seeed-Studio/Grove_Gesture/blob/master/src/paj7620.h
# Copyright (c) 2023 seeed technology inc.
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.
INIT = (
    # Bank 0：配置手势检测相关参数。
    (0xEF, 0x00), (0x37, 0x07), (0x38, 0x17), (0x39, 0x06), (0x42, 0x01),
    (0x46, 0x2D), (0x47, 0x0F), (0x48, 0x3C), (0x49, 0x00), (0x4A, 0x1E),
    (0x4C, 0x20), (0x51, 0x10), (0x5E, 0x10), (0x60, 0x27), (0x80, 0x42),
    (0x81, 0x44), (0x82, 0x04), (0x8B, 0x01), (0x90, 0x06), (0x95, 0x0A),
    (0x96, 0x0C), (0x97, 0x05), (0x9A, 0x14), (0x9C, 0x3F), (0xA5, 0x19),
    (0xCC, 0x19), (0xCD, 0x0B), (0xCE, 0x13), (0xCF, 0x64), (0xD0, 0x21),
    # Bank 1：配置运行参数并启用检测，最后切回 Bank 0 读结果。
    (0xEF, 0x01), (0x02, 0x0F), (0x03, 0x10), (0x04, 0x02), (0x25, 0x01),
    (0x27, 0x39), (0x28, 0x7F), (0x29, 0x08), (0x3E, 0xFF), (0x5E, 0x3D),
    (0x65, 0x96), (0x67, 0x97), (0x69, 0xCD), (0x6A, 0x01), (0x6D, 0x2C),
    (0x6E, 0x01), (0x72, 0x01), (0x73, 0x35), (0x77, 0x01), (0xEF, 0x00),
)

# 低字节为 0x43，高字节为 0x44。方向以芯片坐标系为准。
GESTURES = {
    0x001: "右", 0x002: "左", 0x004: "上", 0x008: "下",
    0x010: "前推", 0x020: "后拉", 0x040: "顺时针",
    0x080: "逆时针", 0x100: "挥手",
}
UNLOCK_SEQUENCE = (0x002, 0x004, 0x040)
STEP_SECONDS = 5.0
BETWEEN_GESTURES_SECONDS = 0.5


class I2CMessage(ctypes.Structure):
    _fields_ = [
        ("addr", ctypes.c_uint16), ("flags", ctypes.c_uint16),
        ("length", ctypes.c_uint16), ("buffer", ctypes.c_void_p),
    ]


class I2CTransfer(ctypes.Structure):
    _fields_ = [("messages", ctypes.POINTER(I2CMessage)), ("count", ctypes.c_uint32)]


class Sensor:
    def __init__(self, fd):
        self.fd = fd
        self.ioctl = ctypes.CDLL(None, use_errno=True).ioctl
        self.ioctl.argtypes = [ctypes.c_int, ctypes.c_ulong, ctypes.c_void_p]
        self.ioctl.restype = ctypes.c_int

    def transfer(self, outgoing, size=0):
        # 读寄存器时，一次 ioctl 提交写地址和读数据，中间使用重复起始。
        # ctypes 缓冲区在系统调用返回前一直有效。
        write = (ctypes.c_uint8 * len(outgoing))(*outgoing)
        messages = [I2CMessage(ADDRESS, 0, len(write), ctypes.addressof(write))]
        read = (ctypes.c_uint8 * size)()
        if size:
            messages.append(I2CMessage(ADDRESS, I2C_M_RD, size, ctypes.addressof(read)))
        array = (I2CMessage * len(messages))(*messages)
        transfer = I2CTransfer(array, len(array))
        completed = self.ioctl(self.fd, I2C_RDWR, ctypes.byref(transfer))
        if completed < 0:
            error = ctypes.get_errno()
            raise OSError(error, os.strerror(error))
        if completed != len(array):
            raise OSError("I²C 事务未完整执行")
        return bytes(read)

    def probe(self):
        # 实机第一次寻址曾无应答，间隔 100 ms 重试并检查两个 ID 字节。
        for attempt in range(3):
            if attempt:
                time.sleep(0.1)
            try:
                chip_id = self.transfer((0x00,), 2)
            except OSError as exc:
                problem = str(exc)
                continue
            if chip_id == CHIP_ID:
                return
            problem = f"实际 ID {chip_id.hex(' ')}，预期 20 76"
        raise OSError(f"无法确认 PAJ7620U2 芯片 ID · {problem}")

    def initialize(self):
        self.probe()
        try:
            for register in INIT:
                self.transfer(register)
            # 和 Seeed 的 Arduino 驱动一样使用近距离模式的采样参数。
            self.transfer((BANK, 1))
            self.transfer((0x65, 18))
        finally:
            self.transfer((BANK, 0))
        # 实测小圆容易被识别成直划。保留连续旋转检测，将累计阈值从 32 降到 24。
        # 0x99 的 bit 0 是启用位，bit 1–6 存阈值。动作码仍由芯片输出。
        self.transfer((0x99, (24 << 1) | 1))
        time.sleep(0.1)
        self.read_flags()  # 丢弃开始本轮之前积存的手势。

    def read_flags(self):
        return int.from_bytes(self.transfer((RESULT,), 2), "little")


class GestureSequence:
    def __init__(self):
        self.step = 0
        self.deadline = 0.0
        self.ignore_until = 0.0

    def expire(self, now):
        if self.step and now >= self.deadline:
            self.step = 0
            return True
        return False

    def accept(self, flags, now):
        self.expire(now)
        # 接受一笔后留出收手时间。期间的读数不计入序列，也不延长截止时间。
        if self.step and now < self.ignore_until:
            return self.step
        # 一个寄存器结果可能同时有多个位。只能在单个标志完全匹配时前进。
        # 错误动作清空已有进度。若它正好是「左」，从这一笔开始新序列。
        if flags == UNLOCK_SEQUENCE[self.step]:
            self.step += 1
            self.deadline = now + STEP_SECONDS
            self.ignore_until = now + BETWEEN_GESTURES_SECONDS
        elif flags == UNLOCK_SEQUENCE[0]:
            self.step = 1
            self.deadline = now + STEP_SECONDS
            self.ignore_until = now + BETWEEN_GESTURES_SECONDS
        else:
            self.step = 0
        progress = self.step
        if progress == len(UNLOCK_SEQUENCE):
            self.step = 0
        return progress


def emit(event, **fields):
    # ensure_ascii 的输出在任何 Jetson 终端语言设置下都是有效 JSON。
    print(json.dumps({"event": event, **fields}), flush=True)


def listen(sensor, mode, seconds):
    sensor.initialize()
    emit("ready", mode=mode)
    sequence = GestureSequence()
    until = time.monotonic() + seconds
    while time.monotonic() < until:
        flags = sensor.read_flags()
        now = time.monotonic()
        if now >= until:
            break
        if mode == "unlock" and sequence.expire(now):
            emit("reset")
        if flags:
            progress = sequence.accept(flags, now) if mode == "unlock" else 0
            emit("gesture", name=GESTURES.get(flags, "混合或未知手势"),
                 flags=f"0x{flags:03x}", step=progress)
            if progress == len(UNLOCK_SEQUENCE):
                emit("result", unlocked=True)
                return
        time.sleep(0.03)
    emit("result", unlocked=False)


def main():
    parser = argparse.ArgumentParser(description="PAJ7620U2 I²C 手势读取")
    parser.add_argument("mode", choices=("probe", "watch", "unlock"))
    parser.add_argument("--device", default="/dev/i2c-7")
    parser.add_argument("--seconds", type=int, choices=range(1, 121), default=20,
                        metavar="1–120")
    args = parser.parse_args()
    try:
        with open(args.device, "rb+", buffering=0) as device:
            # Qt 和终端助手之间互斥，避免两边读取时抢走同一个事件。
            try:
                fcntl.flock(device, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as exc:
                raise OSError("已有手势监听，请先结束另一个助手") from exc
            sensor = Sensor(device.fileno())
            if args.mode == "probe":
                sensor.probe()
                emit("chip", id="0x7620", device=args.device)
            else:
                listen(sensor, args.mode, args.seconds)
    except OSError as exc:
        emit("error", message=str(exc))
        return 1
    except KeyboardInterrupt:
        return 130
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
