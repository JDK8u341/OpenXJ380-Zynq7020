"""受控破坏性 A/B:证明 MIO_PIN_49 的 IO 标准字段是"串口能不能收"的承重位。

做法(板上实测,不重新烧写、不重新加载):
  A  当前状态(内核已由加载器修好 [11:9]=1)      -> 命令通道应当能用
  B  用 JTAG 把 [11:9] 改回 3(=XSA 的错误值)      -> 命令通道应当失效
  A' 再改回 1                                      -> 命令通道应当恢复

为什么需要这一层:前两级证据(软件自报日志、寄存器读回)都只能说明
"某个值被写进去了"。只有让行为随这个位一起改变,才能说明它是**承重的**,
而不是碰巧改好了别的东西。

注意 B 步骤必须**先确认提示符消失**,再据此判定;如果只是"没回显",
也可能是恰好丢了包 —— 所以每个阶段都连发多次取一致性。
"""

import re
import subprocess
import sys
import time
from pathlib import Path

import serial

ROOT = Path(__file__).resolve().parent.parent
# 本机路径(工具链/串口/比特流)全在仓库根目录的 config.py —— 换机器只改那一个文件。
sys.path.insert(0, str(ROOT))
import config  # noqa: E402

XSDB = str(config.XSDB)
POKE = ROOT / "tmp-test" / "jtag" / "mio49_poke.tcl"
PORT = config.SERIAL_PORT
BAUD = config.SERIAL_BAUD

# 1 = LVCMOS18(本板实际电压,正确) ; 3 = LVCMOS33(XSA 里的错误值)
GOOD = 0x000012E1
BAD = 0x000016E1

PROBES = 5


def poke(value: int) -> str:
    """通过 JTAG 写 MIO_PIN_49,返回读回值。"""
    proc = subprocess.run(
        [XSDB, str(POKE), f"0x{value:08X}"],
        cwd=str(ROOT),
        capture_output=True,
        text=True,
        timeout=180,
    )
    out = (proc.stdout or "") + (proc.stderr or "")
    m = re.search(r"MIO49_READBACK\s+(\S+)", out)
    if not m:
        print(out[-800:])
        raise RuntimeError("JTAG 写寄存器失败:看不到 MIO49_READBACK")
    return m.group(1)


def probe_channel(port: serial.Serial, tries: int = PROBES) -> int:
    """连发 tries 次 ver,返回成功应答的次数。"""
    ok = 0
    for _ in range(tries):
        port.reset_input_buffer()
        port.write(b"ver\r")
        port.flush()
        time.sleep(0.9)
        text = port.read(8192).decode("utf-8", "replace")
        if "OpenXJ380 / ARMv7-A" in text:
            ok += 1
    return ok


def main() -> int:
    port = serial.Serial(PORT, BAUD, timeout=0.2)

    print("阶段 A:当前状态(加载器已把 [11:9] 修成 1 = LVCMOS18)")
    a = probe_channel(port)
    print(f"        {a}/{PROBES} 次收到应答")
    if a == 0:
        print("!! 起点就不通,后续对比无意义 —— 请先确认板子已按 loader 正常启动")
        port.close()
        return 2

    print(f"\n阶段 B:JTAG 改回错误值 0x{BAD:08X} ([11:9]=3 = LVCMOS33)")
    print(f"        读回 = 0x{poke(BAD)}")
    b = probe_channel(port)
    print(f"        {b}/{PROBES} 次收到应答")

    print(f"\n阶段 A':再改回正确值 0x{GOOD:08X} ([11:9]=1 = LVCMOS18)")
    print(f"        读回 = 0x{poke(GOOD)}")
    a2 = probe_channel(port)
    print(f"        {a2}/{PROBES} 次收到应答")

    port.close()

    print("\n判定")
    print(f"  A  ={a}/{PROBES}   B  ={b}/{PROBES}   A' ={a2}/{PROBES}")
    if a == PROBES and b == 0 and a2 == PROBES:
        print("  ✅ 行为完全随 [11:9] 这一位翻转 —— 该字段是承重的,结论成立")
        return 0
    if b == PROBES:
        print("  ❌ 改回 LVCMOS33 之后通信照旧 —— 说明这一位不是原因,原结论不成立")
        return 1
    print("  ⚠ 结果不干净(可能有丢包),请重跑;不要据此下结论")
    return 2


if __name__ == "__main__":
    sys.exit(main())
