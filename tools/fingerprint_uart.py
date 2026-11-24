#!/usr/bin/env python3
"""R503 UART client. JSON lines carry progress and the final result to Qt."""

import argparse
import fcntl
import json
import os
import select
import struct
import termios
import time


HEADER = bytes.fromhex("EF01 FFFFFFFF")
COMMAND = 0x01
ACK = 0x07
NO_FINGER = 0x02
NOT_FOUND = 0x09


class FingerprintError(Exception):
    """Operation failed without a successful fingerprint decision."""


def packet(kind, payload):
    # EF01 和地址不参与校验。长度包含末尾两字节的校验和。
    body = struct.pack(">BH", kind, len(payload) + 2) + payload
    return HEADER + body + struct.pack(">H", sum(body) & 0xFFFF)


class SerialPort:
    def __init__(self, path):
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK | os.O_CLOEXEC)
        try:
            fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            settings = termios.tcgetattr(self.fd)
            settings[:6] = [0, 0, termios.CLOCAL | termios.CREAD | termios.CS8, 0,
                            termios.B57600, termios.B57600]
            settings[6][termios.VMIN] = 0
            settings[6][termios.VTIME] = 0
            termios.tcsetattr(self.fd, termios.TCSANOW, settings)
            termios.tcflush(self.fd, termios.TCIFLUSH)
        except (OSError, termios.error):
            os.close(self.fd)
            raise

    def __enter__(self):
        return self

    def __exit__(self, *_):
        os.close(self.fd)

    @staticmethod
    def remaining(deadline):
        left = deadline - time.monotonic()
        if left <= 0:
            raise FingerprintError("等待串口应答超时，请检查端口、供电和 TX/RX")
        return left

    def write_all(self, data, deadline):
        while data:
            if not select.select([], [self.fd], [], self.remaining(deadline))[1]:
                raise FingerprintError("串口发送超时")
            try:
                data = data[os.write(self.fd, data):]
            except BlockingIOError:
                continue

    def read_exactly(self, size, deadline):
        data = bytearray()
        while len(data) < size:
            if not select.select([self.fd], [], [], self.remaining(deadline))[0]:
                raise FingerprintError("等待串口应答超时，请检查端口、供电和 TX/RX")
            try:
                chunk = os.read(self.fd, size - len(data))
            except BlockingIOError:
                continue
            if not chunk:
                raise FingerprintError("串口已断开")
            data.extend(chunk)
        return bytes(data)

    def exchange(self, payload):
        deadline = time.monotonic() + 2.0
        self.write_all(packet(COMMAND, payload), deadline)
        # 串口 read() 可能只返回半包。先找包头，再按声明长度读完并核对地址和校验和。
        prefix = b""
        for _ in range(64):
            prefix = (prefix + self.read_exactly(1, deadline))[-2:]
            if prefix == HEADER[:2]:
                break
        else:
            raise FingerprintError("串口应答没有 EF01 包头")
        if self.read_exactly(4, deadline) != HEADER[2:]:
            raise FingerprintError("模块地址与出厂地址不一致")
        framing = self.read_exactly(3, deadline)
        kind, length = struct.unpack(">BH", framing)
        if kind != ACK or not 3 <= length <= 256:
            raise FingerprintError("串口应答类型或长度错误")
        body = self.read_exactly(length, deadline)
        expected = (sum(framing) + sum(body[:-2])) & 0xFFFF
        if struct.unpack(">H", body[-2:])[0] != expected:
            raise FingerprintError("串口应答校验失败")
        return body[0], body[1:-2]


class Sensor:
    def __init__(self, serial):
        self.serial = serial
        self.command(0x13, bytes(4))  # VerifyPassword，使用未修改过的出厂密码。
        _, params = self.command(0x0F, size=16)
        self.capacity = struct.unpack_from(">H", params, 4)[0]
        if not 1 <= self.capacity <= 1024:
            raise FingerprintError("模块返回了不支持的指纹容量")

    def command(self, opcode, arguments=b"", size=0, allowed=()):
        code, data = self.serial.exchange(bytes((opcode,)) + arguments)
        if code and code not in allowed:
            if opcode == 0x05 and code == 0x0A:
                raise FingerprintError("两次采集的指纹无法合成，请用同一根手指重试")
            raise FingerprintError(f"模块命令 0x{opcode:02X} 失败，应答码 0x{code:02X}")
        if not code and len(data) != size:
            raise FingerprintError(f"模块命令 0x{opcode:02X} 的应答数据长度错误")
        return code, data

    def occupied(self, slot):
        if not 0 <= slot < self.capacity:
            raise FingerprintError(f"槽位必须在 0 至 {self.capacity - 1} 之间")
        page, bit = divmod(slot, 256)
        _, table = self.command(0x1F, bytes((page,)), size=32)
        return bool(table[bit // 8] & (1 << (bit % 8)))

    def wait_for_finger(self, present, timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            code, _ = self.command(0x01, allowed=(NO_FINGER,))
            if (code == 0) == present:
                return
            time.sleep(0.15)
        raise FingerprintError("等待按压超时" if present else "等待松开手指超时")

    def enroll(self, slot, timeout):
        if self.occupied(slot):
            raise FingerprintError(f"槽位 {slot} 已有模板，请选择空槽位")
        emit("prompt", text="第一次：请按压手指")
        self.wait_for_finger(True, timeout)
        self.command(0x02, b"\x01")
        emit("prompt", text="请松开手指")
        self.wait_for_finger(False, timeout)
        emit("prompt", text="第二次：请按压同一根手指")
        self.wait_for_finger(True, timeout)
        self.command(0x02, b"\x02")
        # 两个特征缓冲区在模块上合成，模板由模块写入选定空槽；Jetson 不读出指纹图像。
        self.command(0x05)
        self.command(0x06, struct.pack(">BH", 1, slot))

    def verify(self, timeout):
        emit("prompt", text="请按压要验证的手指")
        self.wait_for_finger(True, timeout)
        self.command(0x02, b"\x01")
        code, data = self.command(0x04, struct.pack(">BHH", 1, 0, self.capacity),
                                  size=4, allowed=(NOT_FOUND,))
        if code == NOT_FOUND:
            return {"matched": False}
        slot, score = struct.unpack(">HH", data)
        if slot >= self.capacity:
            raise FingerprintError("模块返回了超出容量的匹配槽位")
        return {"matched": True, "slot": slot, "score": score}


def emit(event, **data):
    print(json.dumps({"event": event, **data}, ensure_ascii=False), flush=True)


def main():
    parser = argparse.ArgumentParser(description="R503 3.3V UART, 57600 8N1")
    parser.add_argument("--port", required=True)
    parser.add_argument("--slot", type=int)
    parser.add_argument("--wait", type=float, default=20.0)
    parser.add_argument("operation", choices=("probe", "enroll", "verify"))
    args = parser.parse_args()
    if args.operation == "enroll" and args.slot is None:
        parser.error("enroll requires --slot")
    if not 0.5 <= args.wait <= 60:
        parser.error("--wait must be between 0.5 and 60 seconds")
    try:
        with SerialPort(args.port) as serial:
            sensor = Sensor(serial)
            if args.operation == "probe":
                result = {"capacity": sensor.capacity}
            elif args.operation == "enroll":
                sensor.enroll(args.slot, args.wait)
                result = {"slot": args.slot}
            else:
                result = sensor.verify(args.wait)
        emit("result", operation=args.operation, **result)
    except (FingerprintError, OSError, termios.error) as error:
        emit("error", text=str(error))
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
