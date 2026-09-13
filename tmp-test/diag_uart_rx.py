#!/usr/bin/env python3
"""诊断串口 RX:先发一批字符,再用 JTAG 读 UART 寄存器判断收到了没有。

区分两种情况:
    RX FIFO 里有数据 -> 收到了,问题在软件的轮询/处理
    RXEMPTY 仍为 1   -> 根本没收到,问题在硬件通路(MIO / 线序 / 对端)

这一步是必要的:光看"板上没回显"分不出这两类,而它们的排查方向完全不同。
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
XSDC = r"C:\AMDDesignTools\2025.2\Vitis\bin\xsdb.bat"

UART_BASE = 0xE0001000

TCL = f"""
proc rd {{a}} {{
    if {{[catch {{mrd -force $a}} v]}} {{ return "ERR" }}
    set s [string trim $v]
    set i [string first ":" $s]
    if {{$i >= 0}} {{ return [string trim [string range $s [expr {{$i+1}} end]]] }}
    return $s
}}

connect
targets -set -filter {{name =~ "ARM*#0"}}

puts "CR   (0x00) = [rd 0x{UART_BASE + 0x00:08X}]"
puts "MR   (0x04) = [rd 0x{UART_BASE + 0x04:08X}]"
puts "SR   (0x2C) = [rd 0x{UART_BASE + 0x2C:08X}]   <== bit1=1 表示 RX 空"
puts "BAUDGEN(34) = [rd 0x{UART_BASE + 0x34:08X}]"
puts "BAUDDIV(38) = [rd 0x{UART_BASE + 0x38:08X}]"
puts "MIO48(0xF80007C0) = [rd 0xF80007C0]   <== UART1 TX"
puts "MIO49(0xF80007C4) = [rd 0xF80007C4]   <== UART1 RX,期望 0x000016E1"
exit 0
"""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM4")
    parser.add_argument("--baud", type=int, default=9600)
    parser.add_argument("--count", type=int, default=200, help="先发多少个字符")
    args = parser.parse_args()

    try:
        import serial
    except ImportError:
        print("需要 pyserial", file=sys.stderr)
        return 2

    # 1. 先灌字符。**保持端口打开**直到读完寄存器,否则对端可能被挂起
    handle = serial.Serial(args.port, args.baud, timeout=0.1)
    time.sleep(0.3)
    handle.reset_input_buffer()
    handle.write(b"A" * args.count)
    handle.flush()
    print(f"已发送 {args.count} 个 'A',等待其抵达 UART...")
    time.sleep(0.5)  # 9600 波特下 200 字符要 ~208ms

    # 2. 用 JTAG 读寄存器。此时 RX FIFO 里应当有东西
    with tempfile.TemporaryDirectory() as tmp:
        script = Path(tmp) / "uart_rx_state.tcl"
        script.write_text(TCL, encoding="utf-8")
        result = subprocess.run(
            [XSDC, str(script)],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=180,
        )
        for line in (result.stdout or "").splitlines():
            if "=" in line and "INFO" not in line:
                print(line)

    handle.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
