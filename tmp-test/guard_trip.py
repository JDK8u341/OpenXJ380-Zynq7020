"""第三级证据:证明内核栈的 guard page 是**承重的**(M4-5)。

======================================================================
为什么不能用"读回页表"来验收
======================================================================

`vmap_lookup(guard) == 0` 只是**第二级证据**。它在下面这种情况下**照样成立**:

    TLB 里还留着拆段之前的**段表项**。

那时 guard 页在页表里看着"没有映射",但硬件仍然能访问它 ——
guard 完全失效,而所有读回检查全部通过。这恰恰是本模块最可能出的错
(改完页表忘了失效 TLB,见 arch/kstack.h 顶部)。

所以要证明 guard 承重,只有一条路:**同一个地址、同一条指令,
只在"这一页映射与否"这一件事上不同,看行为是否随之改变。**

======================================================================
三个阶段(一次加载里全部跑完)
======================================================================

  A  读自检报告:必须全过,并从串口取出探针栈的 base / guard 地址
     (后面要靠它核对 DFAR)。

  B  选择器 8 —— **对照组**:把 guard 页临时映射成普通可读写页,
     再跑同一段溢出代码。
     期望:**没有任何异常**,而且写下去的内容能读回来
     ("静默损坏"就此被看见)。

  C  选择器 6 —— guard 生效:同一段代码、同一批地址。
     期望:Data Abort,且
       - DFAR 正好等于 `base - 4`(guard 页的最后一个字)
       - FS[4:0] == 0x07(translation fault, level 2)+ WnR = 1(写)

判定:
  B 不报错 **且** C 报错并且 DFAR/DFSR 都对 → guard 是承重的,结论成立。
  只有 C 报错、B 表现不干净 → 无法排除"这段代码本来就会炸",结论不成立。
  只有 B 不报错、C 也不报错 → guard 没生效。

⚠ 顺序不能反:C 会停机,必须在它之前把 B 跑完。
⚠ 串口必须在**加载之前**就打开并在后台线程里持续读 ——
  9600 波特下自检报告比 Windows 串口缓冲区大,等加载完再打开会丢掉报告。
  这个坑 verify_board.py 已经踩过一次。
"""

import re
import subprocess
import sys
import threading
import time
from pathlib import Path

import serial

ROOT = Path(__file__).resolve().parent.parent
XSDB = r"C:\AMDDesignTools\2025.2\Vitis\bin\xsdb.bat"
LOAD_TCL = ROOT / "tmp-test" / "jtag" / "run_kernel_uart.tcl"
POKE_TCL = ROOT / "tmp-test" / "out" / "_guard_trip_poke.tcl"

PORT = "COM4"
BAUD = 9600

FAULT_SEL_ADDR = 0x00020080
SEL_GUARD = 6
SEL_GUARD_OFF = 8

STACK_RE = re.compile(r"Kernel stack: slot0 base=0x([0-9A-F]{8}) top=0x([0-9A-F]{8}) guard=0x([0-9A-F]{8})")
# 注入路径打印的是**探针栈**的地址。它才是溢出落点的判据 ——
# 报告里那行 slot0 只是用来确认栈池建起来了,不能拿来核对 DFAR。
PROBE_RE = re.compile(r"stack base=0x([0-9A-F]{8}) top=0x([0-9A-F]{8}) guard=0x([0-9A-F]{8})")
SUMMARY_RE = re.compile(r"=== SELF-TEST SUMMARY: (\d+) passed, (\d+) failed ===")
DFAR_RE = re.compile(r"DFAR\s+=\s+0x([0-9A-F]{8})")
DFSR_RE = re.compile(r"DFSR\s+=\s+0x([0-9A-F]{8})\s+\(([^)]*)\)")
DOMAIN_RE = re.compile(r"domain\s+=\s+(\d+)\s+WnR\s+=\s+(\d+)")


def fs_of(dfsr: int) -> int:
    """按短描述符格式取 FS[4:0]。

    ★ 不能用 `dfsr & 0x1F` ★ —— DFSR 的 bits[7:4] 是 **Domain**,
    正好压在状态位上面。本内核 domain=15,Domain[0]=1,
    于是真值 0x07 会被 `& 0x1F` 读成 0x17(看起来像"保留")。
    这个坑就是本脚本第一次上板时暴露出来的(实测 DFSR=0x000008F7)。
    """
    return (dfsr & 0xF) | (0x10 if (dfsr >> 10) & 1 else 0)


class SerialLog:
    """后台线程持续读串口,正文只增不减;wait_for 返回**上次读到之后**的新内容。"""

    def __init__(self, port: str, baud: int) -> None:
        self.handle = serial.Serial(port, baud, timeout=0.1)
        self.buf = bytearray()
        self.pos = 0
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self._reader, daemon=True)
        self.thread.start()

    def _reader(self) -> None:
        while not self.stop.is_set():
            try:
                chunk = self.handle.read(512)
            except Exception:  # noqa: BLE001
                break
            if chunk:
                self.buf.extend(chunk)

    def since(self) -> str:
        """取上次取走之后新到的文本,并把游标推到现在。"""
        text = bytes(self.buf[self.pos :]).decode("utf-8", "replace")
        self.pos = len(self.buf)
        return text

    def wait_for(self, needle: str, budget_s: float) -> str:
        """等到正文里(从游标处起)出现 needle,或超时。返回新读到的文本。"""
        got = ""
        deadline = time.time() + budget_s

        while time.time() < deadline:
            got += self.since()
            if needle in got:
                break
            time.sleep(0.05)

        got += self.since()
        return got

    def wait_for_any(self, needles: tuple[str, ...], budget_s: float) -> str:
        """等到任一 needle 出现 —— 用于"要么报错要么继续跑"这种二选一。"""
        got = ""
        deadline = time.time() + budget_s

        while time.time() < deadline:
            got += self.since()
            if any(n in got for n in needles):
                break
            time.sleep(0.05)

        got += self.since()
        return got

    def close(self) -> None:
        self.stop.set()
        self.thread.join(timeout=3.0)
        self.handle.close()


def poke(selector: int) -> str:
    """通过 JTAG 往故障注入选择器写一个码,返回读回值。"""
    POKE_TCL.parent.mkdir(parents=True, exist_ok=True)
    POKE_TCL.write_text(
        "\n".join(
            [
                "connect",
                'targets -set -filter {name =~ "ARM*#0"}',
                f"mwr -force 0x{FAULT_SEL_ADDR:08X} {selector}",
                f'puts "SEL_READBACK [mrd -force 0x{FAULT_SEL_ADDR:08X}]"',
                "exit 0",
            ]
        )
        + "\n",
        encoding="utf-8",
        newline="\n",
    )

    proc = subprocess.run([XSDB, str(POKE_TCL)], cwd=str(ROOT), capture_output=True, text=True,
                          encoding="utf-8", errors="replace", timeout=180)
    out = (proc.stdout or "") + (proc.stderr or "")
    m = re.search(r"SEL_READBACK\s+.*?:\s*([0-9A-Fa-f]{8})", out)
    if not m:
        print(out[-1200:])
        raise RuntimeError("JTAG 写选择器失败")
    return m.group(1)


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

    do_load = "--load" in sys.argv

    # ⚠ 必须先开串口再加载:见文件顶部的说明
    log = SerialLog(PORT, BAUD)
    time.sleep(0.4)

    if do_load:
        print(f"加载内核({LOAD_TCL.name})...")
        proc = subprocess.run([XSDB, str(LOAD_TCL)], cwd=str(ROOT), capture_output=True, text=True,
                              encoding="utf-8", errors="replace", timeout=600)
        out = (proc.stdout or "") + (proc.stderr or "")
        if proc.returncode != 0 or "FAIL" in out:
            print(out[-2000:])
            print("!! 加载失败")
            log.close()
            return 2
        print("  加载完成")

    # ------------------------------------------------------------------
    print("\n阶段 A:读自检报告,取出探针栈的地址")
    # ------------------------------------------------------------------
    text = log.wait_for("=== SELF-TEST END ===", 30.0)
    m = SUMMARY_RE.search(text)
    if not m:
        print(text[-1500:])
        print("!! 没等到自检报告 —— 内核可能没跑到那里(不带 --load 时报告早已打完)")
        log.close()
        return 2

    passed, failed = int(m.group(1)), int(m.group(2))
    print(f"  自检 {passed} passed / {failed} failed")
    if failed != 0:
        print("!! 自检有失败项,先修它再谈 guard —— 否则下面的对比没有意义")
        log.close()
        return 2

    m = STACK_RE.search(text)
    if not m:
        print(text[-1500:])
        print("!! 串口里没有 'Kernel stack: slot0 ...' 行 —— 栈池没建起来")
        log.close()
        return 2

    slot0_base, slot0_top, slot0_guard = (int(m.group(i), 16) for i in (1, 2, 3))
    print(f"  slot0   base=0x{slot0_base:08X}  top=0x{slot0_top:08X}  guard=0x{slot0_guard:08X}")
    print("  (这一行只用来确认栈池建起来了;溢出落点要看下面注入输出里的探针地址)")

    # 排空启动尾巴,让主循环稳定下来
    log.wait_for("alive loop=", 8.0)

    # ------------------------------------------------------------------
    print("\n阶段 B:对照组 —— 选择器 8(临时关掉 guard,同一段溢出)")
    # ------------------------------------------------------------------
    print(f"  写 0x{FAULT_SEL_ADDR:08X} = {SEL_GUARD_OFF}(读回 0x{poke(SEL_GUARD_OFF)})")

    # 二选一:要么打出对照组标记(正常),要么直接 Data Abort(说明"无 guard 也炸")
    btext = log.wait_for_any(("!!! CONTROL GROUP", "Data Abort", "!!!"), 10.0)
    if "!!! CONTROL GROUP" not in btext:
        btext += log.wait_for_any(("!!! CONTROL GROUP", "Data Abort"), 6.0)

    b_control = "!!! CONTROL GROUP" in btext
    b_silent = "silent corruption" in btext
    b_abort = "Data Abort" in btext
    b_written = re.search(r"overflow wrote (\d+) words", btext)
    b_guard_dis = re.search(r"guard_disable=(\d+)", btext)

    print(f"  反馈文本          :")
    for line in btext.splitlines():
        if line.strip():
            print(f"      {line.rstrip()}")

    # 对照组之后内核必须还活着 —— 否则"没有异常"这个结论不成立
    alive = log.wait_for("alive loop=", 8.0)
    b_alive = "alive loop=" in alive

    m = PROBE_RE.search(btext)
    if not m:
        print("!! 注入输出里没有探针地址 —— 无法核对落点")
        log.close()
        return 2
    base, top, guard = (int(m.group(i), 16) for i in (1, 2, 3))
    print(f"  探针栈            : base=0x{base:08X} top=0x{top:08X} guard=0x{guard:08X}")
    print(f"  溢出第一个字应当落在  0x{base - 4:08X}(guard 页的最后一个字)")
    print(f"  guard == base-4096: {guard == base - 4096}")
    print(f"  探针不是 slot0    : {base != slot0_base}(相邻槽的栈顶就是它的 guard)")
    print(f"  对照组标记        : {'出现' if b_control else '未出现'}")
    print(f"  guard_disable 返回: {b_guard_dis.group(1) if b_guard_dis else '（没报）'}")
    print(f"  溢出写入字数      : {b_written.group(1) if b_written else '（没报）'}")
    print(f"  静默损坏被读到    : {'是' if b_silent else '否'}")
    print(f"  这一步有 Data Abort: {'有' if b_abort else '无'}")
    print(f"  内核仍在推进      : {'是' if b_alive else '否'}")

    # ------------------------------------------------------------------
    print("\n阶段 C:guard 生效 —— 选择器 6(同一段代码、同一批地址)")
    # ------------------------------------------------------------------
    print(f"  写 0x{FAULT_SEL_ADDR:08X} = {SEL_GUARD}(读回 0x{poke(SEL_GUARD)})")
    ctext = log.wait_for("System halted", 12.0)

    for line in ctext.splitlines():
        if line.strip():
            print(f"      {line.rstrip()}")

    c_abort = "Data Abort" in ctext
    m = DFAR_RE.search(ctext)
    c_dfar = int(m.group(1), 16) if m else None
    m = DFSR_RE.search(ctext)
    c_dfsr = int(m.group(1), 16) if m else None
    c_dfsr_text = m.group(2) if m else ""
    c_fs = fs_of(c_dfsr) if c_dfsr is not None else -1
    m = DOMAIN_RE.search(ctext)
    c_domain = int(m.group(1)) if m else -1
    c_wnr = int(m.group(2)) if m else -1

    log.close()

    print("\n  解析")
    print(f"      Data Abort        : {'有' if c_abort else '无'}")
    print(f"      DFAR              : {f'0x{c_dfar:08X}' if c_dfar is not None else '（没读到）'}")
    print(f"      DFSR              : {f'0x{c_dfsr:08X}' if c_dfsr is not None else '（没读到）'}"
          f"  ({c_dfsr_text})")
    if c_dfsr is not None:
        print(f"      Domain / WnR      : {c_domain} / {c_wnr}")
    print(f"      FS[4:0]           : 0x{c_fs:02X}"
          f"(guard 用'不映射'实现时期望 0x07 = translation fault, level 2)")

    # ------------------------------------------------------------------
    print("\n判定")
    # ------------------------------------------------------------------
    ok_dfar = c_dfar == base - 4
    ok_guard = guard == base - 4096
    ok_dfsr = c_fs == 0x07

    print(f"  B(无 guard)无异常且看到静默损坏 : {b_control and b_silent and not b_abort}")
    print(f"  B 内核仍在推进                  : {b_alive}")
    print(f"  C(有 guard)报 Data Abort       : {c_abort}")
    print(f"  探针 guard == base-4096         : {ok_guard}")
    print(f"  C 的 DFAR == base-4             : {ok_dfar}   "
          f"(DFAR=0x{c_dfar:08X} 期望=0x{base - 4:08X})" if c_dfar is not None else "")
    print(f"  C 的 WnR == 1(写)              : {c_wnr == 1}")
    print(f"  C 的 FS[4:0] == 0x07            : {ok_dfsr}")

    if (b_control and b_silent and not b_abort and b_alive and c_abort and ok_guard and ok_dfar
            and ok_dfsr and c_wnr == 1):
        print("\n  ✅ 同一个地址、同一段代码,只在 guard 页映射与否上不同 ——")
        print("     行为随之从『静默损坏』变成『当场 Data Abort,DFAR 落在 guard 页里』。")
        print("     guard page 是承重的,第三级证据成立。")
        return 0

    if c_abort and not b_control:
        print("\n  ⚠ C 报了异常但 B 的表现不干净 —— 无法排除『这一段代码本来就会炸』。")
        print("     重跑;若重现,说明异常来源不是 guard。")
        return 2

    if not c_abort:
        print("\n  ❌ guard 没有生效:关掉它会静默损坏,打开它也不报错。")
        return 1

    print("\n  ❌ 异常发生了,但 DFAR / DFSR 与 guard 页对不上 —— 不是 guard 拦下来的。")
    return 1


if __name__ == "__main__":
    sys.exit(main())
