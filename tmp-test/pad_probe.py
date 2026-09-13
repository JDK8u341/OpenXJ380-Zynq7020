"""判定 PS UART 的 PC->板 方向是线路问题还是控制器问题。

做法:PC 端每 4 秒在「连发 0x00」和「连发 0xFF」之间切换,同时板子通过 JTAG
采样 MIO49 焊盘电平(GPIO DATA_RO,与引脚复用功能无关)。

判读:
  bit17 跟着翻转  -> 线是通的,故障在 MIO 复用/控制器一侧
  bit17 始终不变  -> 线没接上或接错脚,软件侧无解
  一直读到 ERR    -> GPIO 时钟被门控或总线不可访问,本次实验无效
"""

import re
import subprocess
import sys
import threading
import time
from pathlib import Path

import serial

ROOT = Path(__file__).resolve().parent.parent
# 本机路径(工具链/串口/比特流)全在仓库根目录的 config.py —— 换机器只改那一个文件。
sys.path.insert(0, str(ROOT))
import config  # noqa: E402

XSDB = str(config.XSDB)
TCL = ROOT / "tmp-test" / "jtag" / "sample_mio49.tcl"
PORT = config.SERIAL_PORT
BAUD = config.SERIAL_BAUD
FLIP_S = 4.0
TOTAL_S = 90.0

MIO48_BIT = 16
MIO49_BIT = 17


def send_worker(stop: threading.Event, log: list):
    """交替连发 0x00 / 0xFF。0x00 会把线压低约 90% 的时间,0xFF 反之。"""
    try:
        port = serial.Serial(PORT, BAUD, timeout=0.1)
    except Exception as exc:  # noqa: BLE001 - 串口打不开就要如实报告
        log.append(("ERROR", f"无法打开 {PORT}: {exc}"))
        return

    t0 = time.monotonic()
    state = 0x00
    chunk = bytes([state]) * 64
    next_flip = t0
    try:
        while not stop.is_set() and time.monotonic() - t0 < TOTAL_S:
            now = time.monotonic()
            if now >= next_flip:
                state = 0xFF if state == 0x00 else 0x00
                chunk = bytes([state]) * 64
                next_flip = now + FLIP_S
                log.append(("FLIP", f"{state:02X}"))
            port.write(chunk)
            port.flush()
    finally:
        port.close()


def main() -> int:
    log: list = []
    stop = threading.Event()

    print(f"启动 xsdb 采样:{TCL.name}")
    proc = subprocess.Popen(
        [XSDB, str(TCL)],
        cwd=str(ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )

    sender = threading.Thread(target=send_worker, args=(stop, log), daemon=True)
    samples: list = []
    t0 = time.monotonic()
    ready_seen = False
    sender_started = False

    def kick_sender(reason: str) -> None:
        nonlocal sender_started
        if sender_started:
            return
        sender_started = True
        sender.start()
        print(f"开始交替发送 0x00/0xFF({reason})")

    try:
        for line in proc.stdout:  # type: ignore[union-attr]
            line = line.strip()
            if not line:
                continue
            if line.startswith("PADPROBE_") and not line.startswith("PADPROBE_READY"):
                print(f"  [xsdb] {line}")
            if line.startswith("PADPROBE_READY"):
                ready_seen = True
                kick_sender("板子就绪")
                continue
            m = re.match(r"SAMPLE (\d+) (\S+) (\S+)", line)
            if m:
                ts = int(m.group(1))
                samples.append((time.monotonic() - t0, ts, m.group(2), m.group(3)))
                if len(samples) == 1:
                    # 兜底:万一 READY 那行的解析出意外,看到采样点就开送。
                    kick_sender("收到首个采样点")
            if line.startswith("PADPROBE_DONE"):
                break
    finally:
        stop.set()
        sender.join(timeout=5)
        try:
            proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            proc.kill()

    print(f"\n采样点 {len(samples)} 个,发送端事件 {len(log)} 条")
    if not ready_seen:
        print("!! 没有看到 PADPROBE_READY —— xsdb 阶段失败,本次实验无效")
        return 2
    if not samples:
        print("!! 一个采样点都没解析到,本次实验无效")
        return 2

    errs = sum(1 for _, _, v, _ in samples if v == "ERR")
    if errs == len(samples):
        print("!! 全部读回 ERR —— GPIO 时钟可能被门控,本次实验无效")
        return 2

    def bit(val: str, n: int):
        if val == "ERR":
            return None
        try:
            return (int(val, 16) >> n) & 1
        except ValueError:
            return None

    # 发送端每 4 秒翻转一次,采样每 0.25 秒一次 => 每 16 个采样点对应一段。
    # 不用绝对时间对齐(两个线程的启动时刻本来就有偏差),按序号分块更稳。
    print("\n按 16 点(约 4 秒)分段统计 MIO49 电平:")
    print("  段   采样数   bit17=1  bit17=0  未知   判定")
    seg_len = 16
    segs = []
    for s in range(0, len(samples), seg_len):
        chunk_samples = samples[s : s + seg_len]
        ones = sum(1 for _, _, v, _ in chunk_samples if bit(v, MIO49_BIT) == 1)
        zeros = sum(1 for _, _, v, _ in chunk_samples if bit(v, MIO49_BIT) == 0)
        unknown = len(chunk_samples) - ones - zeros
        if ones and not zeros:
            verdict = "恒高"
        elif zeros and not ones:
            verdict = "恒低"
        elif unknown == len(chunk_samples):
            verdict = "读不到"
        else:
            verdict = "有跳变"
        segs.append(verdict)
        print(f"  {s // seg_len:3d}  {len(chunk_samples):6d}  {ones:7d}  {zeros:7d}  {unknown:5d}   {verdict}")

    all_ones = all(v == "恒高" for v in segs)
    all_zeros = all(v == "恒低" for v in segs)
    has_flip = any(v == "有跳变" for v in segs)

    print("\nMIO48(TX)电平分布参考:", end=" ")
    tx_ones = sum(1 for _, _, v, _ in samples if bit(v, MIO48_BIT) == 1)
    print(f"1={tx_ones} 0={len(samples) - tx_ones}(空闲时应绝大多数为 1)")

    print("\n结论:")
    if has_flip:
        print("  MIO49 焊盘电平跟着 PC 的发送翻转 —— 物理线路是通的。")
        print("  故故障在 SoC 内部:MIO 复用选择或 UART 控制器侧,可以继续查软件。")
        return 1
    if all_ones:
        print("  MIO49 恒为高,PC 无论发 0x00 还是 0xFF 都没变化。")
        print("  => PC 的 TX 没有到达 MIO49 焊盘。软件侧无法解决。")
        return 0
    if all_zeros:
        print("  MIO49 恒为低 —— 该引脚被持续拉低(短路或对端一直在驱动低电平)。")
        return 0
    print("  电平分布不明确,请人工看上面的分段表。")
    return 2


if __name__ == "__main__":
    sys.exit(main())
