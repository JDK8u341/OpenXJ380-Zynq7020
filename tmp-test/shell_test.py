#!/usr/bin/env python3
"""串口命令通道的板上测试:加载内核 -> 等提示符 -> 发命令 -> 校验回显。

与 verify_board.py 的分工:
    verify_board.py  只读,解析自检报告(不需要板子配合收发)
    本脚本           双向,验证命令通道本身(发命令、看回显、看执行结果)

用法:
    python tmp-test/shell_test.py --load
    python tmp-test/shell_test.py --port <串口>           # 不重新加载
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# 本机路径(工具链/串口/比特流)全在仓库根目录的 config.py —— 换机器只改那一个文件。
sys.path.insert(0, str(ROOT))
import config  # noqa: E402

XSDC = str(config.XSDB)
LOAD_TCL = ROOT / "tmp-test" / "jtag" / "run_kernel_uart.tcl"

PROMPT = "\n> "


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


def drain_until(handle, needles: list[str], deadline_s: float) -> str:
    """读到**该出现的片段全出现**为止,或者到点为止。

    ⚠ 为什么不是"读固定一段时间":9600 波特下一行要 60~80ms,而
    `ver`/`dump` 这类命令的回显是好几十行 —— 固定窗口必然截断。
    而且 M4-8.4 之后**串口有两个写者**(1Hz 状态行),固定窗口还会读到
    "上一条命令的尾巴",于是判据看起来像"回显不对",其实只是没读完。
    """
    end = time.time() + deadline_s
    chunks = bytearray()

    while time.time() < end:
        try:
            data = handle.read(512)
        except Exception:  # noqa: BLE001
            break
        if data:
            chunks.extend(data)
            text = chunks.decode("utf-8", errors="replace")
            if all(n in text for n in needles):
                break

    return chunks.decode("utf-8", errors="replace")


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
    # peek 读心跳区的 magic 槽。
    #
    # 这里原来读的是 0x41200008(AXI GPIO 的 LED 数据寄存器)并期待 0,
    # 前提就是错的 —— 主循环的流水灯一直在改写它,所以它几乎不可能是 0。
    # 上板实测读回 0x00000010 而判定 FAIL,是**期望值定错**,不是 peek 有问题。
    #
    # 换成 OCM 心跳的 magic:内核在 kmain 开头写死 0x4F583338("OX38"),
    # 是个常量,与 JTAG 加载器里的同名校验互为交叉验证。
    Case("peek 20000", ["peek 0x00020000", "0x4F583338"], wait=2.0),
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
    parser.add_argument("--port", default=config.SERIAL_PORT)
    parser.add_argument("--baud", type=int, default=config.SERIAL_BAUD)
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

    # 等命令通道就绪。
    #
    # ⚠ 这里原来是 `drain(handle, 8.0)` + "横幅 + 自检报告大约 3~4 秒" —— 
    #   那个数字是 **M3-7 写的**,此后每一阶段都让它更远:
    #     M4 各相的内核线程与自检项、M4-8.4 的 1Hz 状态行、
    #     M4-8.5 的无饥饿相、M4-10 的两核相与对照组……
    #   到 M4-10 时,**报告本身就有 81 行** × 60~80ms ≈ 6 秒,
    #   加上各相与对照组,加载完成到提示符要 **二十多秒**。
    #   8 秒窗口于是必然失败,而失败信息("等不到命令提示符,内核可能没跑到
    #   那一步")会把人往**错的方向**带 —— 实测就是这么误导了我一轮。
    #
    #   ⇒ 改成"一直等到提示符出现,上限 60 秒"。判据本身没变松:
    #     它仍然是"提示符必须出现",只是不再用一个过期的常数猜它什么时候出现。
    boot = drain_until(handle, [PROMPT], 60.0)
    if PROMPT not in boot:
        print("错误: 60 秒内等不到命令提示符,内核可能真的没跑到那一步", file=sys.stderr)
        print(boot[-2000:], file=sys.stderr)
        return 1

    print("已就绪,开始发送命令\n")

    failures = 0
    for case in CASES:
        label = case.line if case.line else "(空行)"
        send(handle, case.line)
        # 读到"该出现的片段都出现"为止;上限取 max(wait, 8s) ——
        # 9600 波特下几十行回显本身就要好几秒
        reply = drain_until(handle, case.must_contain, max(case.wait, 8.0))

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
