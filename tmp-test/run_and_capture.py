"""JTAG 加载内核 + 同步抓取串口原始字节。

为什么不用普通串口助手:
    波特率不对时串口上出现的是成帧错误产生的垃圾字节,普通终端会把它
    渲染成空白或方块,看上去像"什么都没发"。这里强制以 hex 打印,
    才能区分三种截然不同的情况:
        全 0x00  -> 线路被拉低 / TX 被写 0
        全 0xFF  -> 线路空闲为高,其实没在发
        有字符   -> 波特率基本正确,内容可读
    之前正是靠这一步才定位到 console_init 用 NULL 覆盖了波特率寄存器。

用法:
    python tmp-test/run_and_capture.py [COM4] [9600] [秒数]
"""

import subprocess
import sys
import threading
import time

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM4"
BAUD = int(sys.argv[2]) if len(sys.argv) > 2 else 9600
SECONDS = float(sys.argv[3]) if len(sys.argv) > 3 else 20.0

XSDC = r"C:\AMDDesignTools\2025.2\Vitis\bin\xsdb.bat"
TCL = r"C:\Users\VeryS\Documents\others\OpenXJ380\tmp-test\jtag\run_kernel_uart.tcl"

captured = bytearray()
stop_flag = threading.Event()


def reader():
    try:
        import serial
    except ImportError:
        print("[capture] 缺少 pyserial,跳过串口抓取")
        return

    try:
        # timeout 小一点,保证 stop_flag 能及时生效
        port = serial.Serial(PORT, BAUD, timeout=0.1)
    except Exception as exc:  # noqa: BLE001 - 端口占用/不存在都要能继续跑 JTAG
        print(f"[capture] 打开 {PORT} 失败: {exc}")
        return

    print(f"[capture] 已打开 {PORT} @ {BAUD}")
    while not stop_flag.is_set():
        try:
            chunk = port.read(256)
        except Exception as exc:  # noqa: BLE001
            print(f"[capture] 读取失败: {exc}")
            break
        if chunk:
            captured.extend(chunk)
    port.close()
    print(f"[capture] 关闭 {PORT},共收到 {len(captured)} 字节")


def hexdump(data):
    """按 16 字节一行输出 hex + 可打印字符,便于肉眼比对 ASCII 文本。"""
    for offset in range(0, len(data), 16):
        row = data[offset : offset + 16]
        hexpart = " ".join(f"{b:02X}" for b in row).ljust(47)
        text = "".join(chr(b) if 32 <= b < 127 else "." for b in row)
        print(f"  {offset:06X}  {hexpart}  |{text}|")


def main():
    # Windows 控制台默认 GBK,直接 print 抓到的原始字节会炸在编码上。
    # 这里强制 UTF-8 + replace:抓串口数据时"显示成问号"永远好过"抛异常中断"。
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001 - 老解释器没有 reconfigure
        pass

    thread = threading.Thread(target=reader, daemon=True)
    thread.start()

    # 给串口线程一点时间先打开端口,避免漏掉开头的横幅
    time.sleep(1.0)

    print(f"[jtag] xsdb -source {TCL}")
    proc = subprocess.run(
        [XSDC, TCL],
        capture_output=True,
        encoding="utf-8",
        errors="replace",
        timeout=300,
    )
    print(proc.stdout or "")
    if proc.stderr:
        print("[jtag stderr]", proc.stderr)

    # xsdb 退出后继续收尾,把主循环的周期输出也抓进来
    time.sleep(SECONDS)
    stop_flag.set()
    thread.join(timeout=3.0)

    print(f"\n=== 串口原始数据 ({len(captured)} 字节) ===")
    if not captured:
        print("  (空 —— 一个字节都没收到)")
    else:
        hexdump(captured)
        non_zero = sum(1 for b in captured if b != 0)
        print(f"\n  非零字节: {non_zero} / {len(captured)}")
        text = captured.decode("ascii", errors="replace")
        print("\n=== 按 ASCII 解释 ===")
        print(text)


if __name__ == "__main__":
    main()
