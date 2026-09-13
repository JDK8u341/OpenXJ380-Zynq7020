#!/usr/bin/env python3
"""上板验证:抓串口 -> 解析自检报告 -> 退出码即结论。

============================================================================
为什么要有它
============================================================================

在此之前,"上板验证"是靠人读串口日志判断"看起来没问题"。两个毛病:

  1. **不可自动化** —— 改一行代码就要重看一遍;
  2. **判据模糊** —— "speedup=6x 算不算通过"人心里有个数,
     但那个数没写在任何地方,换个人或换个时间判断可能不同。

现在判据写在固件里(arch/arm32/include/arch/selftest.h),板子以固定格式
回传,本脚本负责判定。所以"上板验证"是一条命令:

    python tmp-test/verify_board.py --load          # 顺带用 JTAG 重新加载

============================================================================
解析规则(与固件的输出格式一一对应)
============================================================================

    === SELF-TEST BEGIN ===
    CHECK mmu_stage             = 4            (expect == 4)        PASS
    ...
    === SELF-TEST SUMMARY: 18 passed, 0 failed ===
    === SELF-TEST END ===

**只认行首的 "CHECK "** —— 若把别的行当成检查项,脚本会静默地少检查几项,
而"少检查"和"检查通过"在输出上长得一样。所以:
  - 报告区间必须被 BEGIN/END 框住;
  - 区间内除了 CHECK 行和 SUMMARY 行,出现别的非空行会报警告;
  - 一项都没解析到 = 失败(而不是"通过")。
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# 本机路径(工具链/串口/比特流)全在仓库根目录的 config.py —— 换机器只改那一个文件。
sys.path.insert(0, str(ROOT))
import config  # noqa: E402


BEGIN = "=== SELF-TEST BEGIN ==="
END = "=== SELF-TEST END ==="
SUMMARY_RE = re.compile(r"=== SELF-TEST SUMMARY: (\d+) passed, (\d+) failed ===")
CHECK_RE = re.compile(r"^CHECK (\S+)\s*=\s*(\d+)\s+\(expect (\S+) (\d+)\)\s+(PASS|FAIL)$")

# ★ 报告之后的"整机还活着"签名(内核主循环之前打的那一行)★
#
# 为什么需要它(坑 53):自检报告是**报告那一刻**的快照。报告之后还有七组
# 破坏性对照组会把调度状态搅乱,而它们坏掉的方式通常是**日志中途断掉**
# (异常 → 停机)—— 那时报告早已打完、97 项全绿。实测踩过一次:
# M4-11.1 的第一次验证只看了报告与自己那条 A/B,而整机在 9.78 之前就
# Data Abort 了,直到逐行比对完整日志才发现。
#
# ⇒ 于是把"跑完了"变成机器可判的:**缺了这一行,即使 SUMMARY 全过也算失败**。
POST_SIGNATURE = "Boot complete:"

DEFAULT_PORT = config.SERIAL_PORT
DEFAULT_BAUD = config.SERIAL_BAUD
XSDC = str(config.XSDB)
LOAD_TCL = ROOT / "tmp-test" / "jtag" / "run_kernel_uart.tcl"


# ---------------------------------------------------------------------------
# 解析(纯逻辑,单测见 tests/test_selftest.py)
# ---------------------------------------------------------------------------


class Check:
    def __init__(self, name: str, actual: int, op: str, expect: int, passed: bool) -> None:
        self.name = name
        self.actual = actual
        self.op = op
        self.expect = expect
        self.passed = passed

    def __repr__(self) -> str:
        return f"Check({self.name}, {self.actual} {self.op} {self.expect}, {'PASS' if self.passed else 'FAIL'})"


class ReportError(Exception):
    """报告本身有问题(缺失、被截断、格式不对)—— 与"某项没通过"是两回事。"""


def parse_report(text: str) -> tuple[list[Check], int, int]:
    """从串口文本里抽出自检报告。

    返回 (检查项列表, 声明的通过数, 声明的失败数)。

    报告缺失或不完整时抛 ReportError —— **不能把"没解析到"当成"通过"**,
    那会让验证脚本在最需要它的时候给出最危险的结论。
    """
    lines = [line.rstrip("\r\n") for line in text.splitlines()]

    try:
        begin = lines.index(BEGIN)
    except ValueError:
        raise ReportError(f"串口输出里找不到 {BEGIN!r} —— 内核可能没跑到自检阶段") from None

    end = None
    for i in range(begin + 1, len(lines)):
        if lines[i] == END:
            end = i
            break
    if end is None:
        raise ReportError(f"找到 {BEGIN!r} 但没有 {END!r} —— 输出可能在自检途中被截断")

    checks: list[Check] = []
    passed = failed = None
    stray: list[str] = []

    for line in lines[begin + 1 : end]:
        if not line.strip():
            continue
        m = CHECK_RE.match(line)
        if m:
            checks.append(
                Check(
                    name=m.group(1),
                    actual=int(m.group(2)),
                    op=m.group(3),
                    expect=int(m.group(4)),
                    passed=(m.group(5) == "PASS"),
                )
            )
            continue

        s = SUMMARY_RE.match(line)
        if s:
            passed, failed = int(s.group(1)), int(s.group(2))
            continue

        stray.append(line)

    if passed is None or failed is None:
        raise ReportError("报告区间里没有 SUMMARY 行")
    if not checks:
        raise ReportError("报告区间里一项 CHECK 都没有")

    # 交叉校验:声明的数目必须与实际解析到的一致。
    # 不一致说明格式变了或输出被截断,此时**任何结论都不可信**。
    actual_passed = sum(1 for c in checks if c.passed)
    actual_failed = len(checks) - actual_passed
    if (actual_passed, actual_failed) != (passed, failed):
        raise ReportError(
            f"SUMMARY 声明 {passed} passed / {failed} failed,"
            f"但实际解析到 {actual_passed} / {actual_failed} —— 报告不完整"
        )

    if stray:
        raise ReportError("报告区间里出现了非 CHECK/SUMMARY 的行(格式被污染): " + " | ".join(stray[:3]))

    return checks, passed, failed


# ---------------------------------------------------------------------------
# 采集
# ---------------------------------------------------------------------------


def capture(port: str, baud: int, seconds: float, load: bool) -> str:
    """抓串口。load=True 时先用 JTAG 重新加载内核。"""
    buffer = bytearray()
    stop = threading.Event()

    try:
        import serial
    except ImportError:
        print("错误: 需要 pyserial(pip install pyserial)", file=sys.stderr)
        raise SystemExit(2)

    try:
        handle = serial.Serial(port, baud, timeout=0.1)
    except Exception as exc:  # noqa: BLE001
        print(f"错误: 打不开 {port}: {exc}", file=sys.stderr)
        raise SystemExit(2)

    def reader() -> None:
        while not stop.is_set():
            try:
                chunk = handle.read(512)
            except Exception:  # noqa: BLE001
                break
            if chunk:
                buffer.extend(chunk)

    thread = threading.Thread(target=reader, daemon=True)
    thread.start()
    time.sleep(0.5)

    if load:
        if not LOAD_TCL.exists():
            print(f"错误: 找不到 {LOAD_TCL}", file=sys.stderr)
            raise SystemExit(2)
        print(f"[jtag] 加载 {LOAD_TCL.name} ...")
        subprocess.run(
            [XSDC, str(LOAD_TCL)],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=300,
        )

    time.sleep(seconds)
    stop.set()
    thread.join(timeout=3.0)
    handle.close()

    return buffer.decode("utf-8", errors="replace")


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", default=DEFAULT_PORT)
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    parser.add_argument("--seconds", type=float, default=8.0, help="加载后继续抓多少秒")
    parser.add_argument("--load", action="store_true", help="先用 JTAG 重新加载内核")
    parser.add_argument("--from-file", type=Path, help="不抓串口,直接解析已有日志")
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()

    if args.from_file:
        text = args.from_file.read_text(encoding="utf-8", errors="replace")
    else:
        text = capture(args.port, args.baud, args.seconds, args.load)

    try:
        checks, passed, failed = parse_report(text)
    except ReportError as exc:
        print(f"\n验证失败: {exc}", file=sys.stderr)
        # 把原始输出留一份,便于排查
        dump = ROOT / "verify_board_raw.txt"
        dump.write_text(text, encoding="utf-8", errors="replace")
        print(f"串口原文已存到 {dump}", file=sys.stderr)
        return 1

    # ★ 报告之后的存活检查:见 POST_SIGNATURE 的说明 ★
    #
    # ⚠ 它**必须**在报告判定之后单独判,而且报错信息要写清"报告是通过的" ——
    #   否则读到 FAIL 的人会去报告里找原因,而报告里没有原因。
    if POST_SIGNATURE not in text:
        print(f"\n验证失败: 自检报告通过了({passed} passed),但串口里没有 "
              f"{POST_SIGNATURE!r} —— 说明**整机没能跑完报告之后的对照组**"
              f"(异常/停机/卡住)。", file=sys.stderr)
        print("  ⇒ 报告只覆盖它自己那一刻;报告之后还有七组破坏性对照组,"
              "它们坏掉的方式就是日志中途断掉。", file=sys.stderr)
        dump = ROOT / "verify_board_raw.txt"
        dump.write_text(text, encoding="utf-8", errors="replace")
        print(f"  串口原文已存到 {dump}", file=sys.stderr)
        return 1

    if not args.quiet:
        print(f"\n{'检查项':<26}{'实际':>12}{'判据':>10}{'结果':>8}")
        print("-" * 58)
        for c in checks:
            mark = "PASS" if c.passed else "FAIL"
            print(f"{c.name:<26}{c.actual:>12}{c.op + ' ' + str(c.expect):>10}{mark:>8}")
        print("-" * 58)

    print(f"\nSELF-TEST: {passed} passed, {failed} failed")

    if failed:
        print("\n未通过的检查项:")
        for c in checks:
            if not c.passed:
                print(f"  {c.name}: 实际 {c.actual},判据 {c.op} {c.expect}")
        return 1

    print("全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
