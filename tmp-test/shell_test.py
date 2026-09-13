#!/usr/bin/env python3
"""串口命令通道的板上测试:加载内核 -> 等提示符 -> 发命令 -> 校验回显。

与 verify_board.py 的分工:
    verify_board.py  只读,解析自检报告(不需要板子配合收发)
    本脚本           双向,验证命令通道本身(发命令、看回显、看执行结果)

用法:
    python tmp-test/shell_test.py --load
    python tmp-test/shell_test.py --port COM4            # 不重新加载
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
XSDC = r"C:\AMDDesignTools\2025.2\Vitis\bin\xsdb.bat"
LOAD_TCL = ROOT / "tmp-test" / "jtag" / "run_kernel_uart.tcl"

PROMPT = "> "


def open_port(port: str, baud: int):
    try:
        import serial
    except ImportError:
        print("错误: 需要 pyserial", file=sys.stderr)
        raise SystemExit(2)

    try:
        return serial.Serial(port, baud, timeout=0.1)
    except Exception as exc:  # noqa: BLE001
        print(f"错误: 打不开 {port}: {exc}", file=sys.stderr)
        raise SystemExit(2)


def drain(handle, seconds: float) -> str:
    """读一段时间,返回解出的文本。"""
    end = time.time() + seconds
    chunks = bytearray()

    while time.time() < end:
        try:
            data = handle.read(512)
        except Exception:  # noqa: BLE001
            break
        if data:
            chunks.extend(data)

    return chunks.decode("utf-8", errors="replace")


def send(handle, text: str) -> None:
    handle.write((text + "\r").encode("ascii"))
    handle.flush()


class Case:
    def __init__(self, line: str, must_contain: list[str], wait: float = 1.5) -> None:
        self.line = line
        self.must_contain = must_contain
        self.wait = wait


# ---------------------------------------------------------------------------
# 用例
# ---------------------------------------------------------------------------
#
# 每条用例的 must_contain 都是**回显里必须出现的片段**。
# 刻意挑"只有真的执行了才会出现"的东西,而不是命令本身 ——
# 命令会被回显,拿它当证据等于没验(这正是本项目反复踩过的
# "软件自述算不算证据"的问题)。
CASES = [
    Case("help", ["commands:", "dump", "peek <hex-addr>"]),
    Case("ver", ["OpenXJ380 / ARMv7-A", "Switches  : 0x"]),
    Case("uptime", ["uptime:", " ms"]),
    Case("dump", ["Board devices:", "axi_gpio_0", "xlnx,axi-gpio-2.0"]),
    Case("probe", ["Board probe", "probed=1"]),
    Case("selftest", ["SELF-TEST BEGIN", "SELF-TEST SUMMARY"]),
    # peek 读我们自己刚写进去的 LED 寄存器,应当等于 0(自检结束后清零了)
    Case("peek 41200008", ["peek 0x41200008", "0x00000000"], wait=2.0),
    # 未知命令必须有明确报错,而不是静默什么都不做
    Case("nosuchcmd", ["unknown command: nosuchcmd"]),
    # 前缀不能匹配(与 nosuchcmd 同等重要:敲少一个字母不该执行别的命令)
    Case("pe", ["unknown command: pe"]),
    # 空行:只该再给一个提示符,不该报错
    Case("", [], wait=1.0),
]


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", default="COM4")
    parser.add_argument("--baud", type=int, default=9600)
    parser.add_argument("--load", action="store_true")
    args = parser.parse_args()

    handle = open_port(args.port, args.baud)
    drain(handle, 0.3)  # 丢掉上电残留

    if args.load:
        print(f"[jtag] 加载 {LOAD_TCL.name} ...")
        subprocess.run(
            [XSDC, str(LOAD_TCL)],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=300,
        )

    # 等命令通道就绪。横幅 + 自检报告大约 3~4 秒
    boot = drain(handle, 8.0)
    if PROMPT not in boot:
        print("错误: 等不到命令提示符,内核可能没跑到那一步", file=sys.stderr)
        print(boot[-2000:], file=sys.stderr)
        return 1

    print("已就绪,开始发送命令\n")

    failures = 0
    for case in CASES:
        label = case.line if case.line else "(空行)"
        send(handle, case.line)
        reply = drain(handle, case.wait)

        missing = [needle for needle in case.must_contain if needle not in reply]
        status = "PASS" if not missing else "FAIL"

        print(f"[{status}] {label!r}")
        if missing:
            failures += 1
            for needle in missing:
                print(f"        缺少: {needle!r}")
            print("        ---- 实际回显 ----")
            for line in reply.splitlines():
                print(f"        | {line}")
            print("        ------------------")

    handle.close()

    print(f"\n{len(CASES) - failures}/{len(CASES)} 通过")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
