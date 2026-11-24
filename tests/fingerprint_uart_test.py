import json
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from fingerprint_simulator import FingerprintSimulator
from fingerprint_uart import ACK, COMMAND, packet


def run(module, helper, action, touches=(), slot=None):
    module.finger = ""
    command = [sys.executable, helper, "--port", module.port, "--wait", "1.5"]
    if slot is not None:
        command += ["--slot", str(slot)]
    events = []
    remaining = iter(touches)
    with subprocess.Popen(command + [action], stdout=subprocess.PIPE, text=True) as child:
        for line in child.stdout:
            message = json.loads(line)
            events.append(message)
            if message["event"] == "prompt":
                module.finger = next(remaining, "")
        return child.wait(timeout=4), events[-1]


def main(helper):
    # 按协议手算的固定包，避免模拟器与驱动共用编码时掩盖错误。
    assert packet(COMMAND, b"\x0f") == bytes.fromhex("EF01 FFFFFFFF 01 0003 0F 0013")
    assert packet(ACK, b"\x00") == bytes.fromhex("EF01 FFFFFFFF 07 0003 00 000A")
    with FingerprintSimulator() as module:
        assert run(module, helper, "probe") == (
            0, {"event": "result", "operation": "probe", "capacity": 200})
        assert run(module, helper, "enroll", ("A", "", "A"), slot=1) == (
            0, {"event": "result", "operation": "enroll", "slot": 1})
        assert run(module, helper, "verify", ("A",)) == (
            0, {"event": "result", "operation": "verify", "matched": True, "slot": 1, "score": 180})
        assert run(module, helper, "verify", ("B",)) == (
            0, {"event": "result", "operation": "verify", "matched": False})
        status, error = run(module, helper, "enroll", slot=1)
        assert status == 1 and "已有模板" in error["text"]
        status, error = run(module, helper, "enroll", ("A", "", "B"), slot=2)
        assert status == 1 and "无法合成" in error["text"] and module.templates == {1: "A"}
        status, error = run(module, helper, "verify")
        assert status == 1 and "等待按压超时" in error["text"]
        module.bad_checksum = True
        status, error = run(module, helper, "probe")
        assert status == 1 and "校验失败" in error["text"]
    print("UART passed: probe, two-touch enrollment, match, reject, occupied slot, mismatch, timeout, checksum")


if __name__ == "__main__":
    main(sys.argv[1])
