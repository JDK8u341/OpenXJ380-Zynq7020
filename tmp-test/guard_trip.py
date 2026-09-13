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
只在"这一页到底能不能访问"这一件事上不同,看行为是否随之改变。**

======================================================================
两个 guard 实现,各一组受控 A/B
======================================================================

guard 有两条独立实现(见 arch/kstack.h):

  - `KSTACK_GUARD_UNMAPPED` —— L2 项清零(板级大池用的就是这个)
  - `KSTACK_GUARD_AP_NONE`  —— 映射着,但 AP=0b000

它们**不是"两种写法"**,而是两种机制:`guard_kind` 是池级配置,所以板级
建了两个池,本脚本对两个池各跑一组 A/B。

### 第一次加载:B(共用对照组)+ C

  A  读自检报告:必须全过。

  B  选择器 8 —— **对照组**:把 guard 页临时映射成普通可读写页,
     再跑同一段溢出代码。
     期望:**没有任何异常**,而且写下去的内容能读回来
     ("静默损坏"就此被看见)。这一组是 C 与 D **共用**的对照 ——
     它证明"这一段代码本身不会炸"。

  C  选择器 6 —— guard 生效("不映射"实现):同一段代码、同一批地址。
     期望:Data Abort,且
       - DFAR 正好等于 `base - 4`(guard 页的最后一个字)
       - FS[4:0] == 0x07(translation fault, level 2)+ WnR = 1(写)

### 第二次加载:D

  D  选择器 7 —— guard 改用 `AP=0b000` 实现。
     这一路 guard 页在**两个阶段里都是映射着的**,唯一差别就是 AP 是不是
     0b000(阶段 B 的 kstack_guard_disable 会把 AP 改回全权限)。
     期望:Data Abort,FS[4:0] == 0x0F(permission fault, level 2)。

     ⚠ 0x0F 而不是 0x07:这是**权限**故障,不是转换故障。两者混起来就等于
       把"guard 拦住了"和"这一段根本没映射"当成同一件事。
     ⚠ 这一路顺带补上 M2-4 欠的账:当年把 DACR 从全 manager 切成全 client,
       理由是"所有区域的 AP 都是 0b011,行为应当完全不变" —— 那是**推理**,
       不是证据。一个 AP=0b000 的页才让"AP 到底有没有被硬件执行"可观测。

======================================================================
判定
======================================================================

  A/B 1 成立(对照组干净 + C 的 DFAR/FS 都对)→ guard("不映射")是承重的。
  A/B 2 成立(D 的 FS == 0x0F)→ AP 被硬件执行,AP 式 guard 也成立。

⚠ 顺序不能反:C 与 D 都会停机,一次加载里只能跑其中一个。
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
# 本机路径(工具链/串口/比特流)全在仓库根目录的 config.py —— 换机器只改那一个文件。
sys.path.insert(0, str(ROOT))
import config  # noqa: E402

XSDB = str(config.XSDB)
LOAD_TCL = ROOT / "tmp-test" / "jtag" / "run_kernel_uart.tcl"
POKE_TCL = ROOT / "tmp-test" / "out" / "_guard_trip_poke.tcl"

PORT = config.SERIAL_PORT
BAUD = config.SERIAL_BAUD

FAULT_SEL_ADDR = 0x00020080
SEL_GUARD = 6
SEL_GUARD_AP = 7
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


# ---------------------------------------------------------------------------
# 一次"加载 + 串口会话"
# ---------------------------------------------------------------------------


def open_boot(do_load: bool) -> SerialLog | None:
    """开串口(必须在加载之前)+ 可选加载。失败返回 None。"""
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
            return None
        print("  加载完成")

    return log


def stage_report(log: SerialLog) -> bool:
    """阶段 A:等自检报告,确认全过。"""
    print("\n阶段 A:读自检报告")

    text = log.wait_for("=== SELF-TEST END ===", 30.0)
    m = SUMMARY_RE.search(text)
    if not m:
        print(text[-1500:])
        print("!! 没等到自检报告 —— 内核可能没跑到那里(不带 --load 时报告早已打完)")
        return False

    passed, failed = int(m.group(1)), int(m.group(2))
    print(f"  自检 {passed} passed / {failed} failed")
    if failed != 0:
        print("!! 自检有失败项,先修它再谈 guard —— 否则下面的对比没有意义")
        return False

    m = STACK_RE.search(text)
    if m:
        b, tp, g = (int(m.group(k), 16) for k in (1, 2, 3))
        print(f"  slot0   base=0x{b:08X}  top=0x{tp:08X}  guard=0x{g:08X}")
        print("  (这行只确认栈池建起来了;溢出落点看下面注入输出里的探针地址)")
    for line in text.splitlines():
        if "Ap guard" in line:
            print(f"  {line.strip()}")

    # 排空启动尾巴,让主循环稳定下来
    log.wait_for("alive loop=", 8.0)
    return True


def stage_control(log: SerialLog) -> dict:
    """阶段 B:对照组 —— 把 guard 页映射成普通可读写页,再跑同一段溢出。

    这一组是 C 与 D **共用**的对照:它证明"这一段代码本身不会炸"。
    """
    print("\n阶段 B:对照组 —— 选择器 8(临时把 guard 页映射出来,同一段溢出)")
    print(f"  写 0x{FAULT_SEL_ADDR:08X} = {SEL_GUARD_OFF}(读回 0x{poke(SEL_GUARD_OFF)})")

    text = log.wait_for_any(("!!! CONTROL GROUP", "Data Abort", "!!!"), 10.0)
    if "!!! CONTROL GROUP" not in text:
        text += log.wait_for_any(("!!! CONTROL GROUP", "Data Abort"), 6.0)

    print("  反馈文本:")
    for line in text.splitlines():
        if line.strip():
            print(f"      {line.rstrip()}")

    alive = log.wait_for("alive loop=", 8.0)

    res = {
        "control": "!!! CONTROL GROUP" in text,
        "silent": "silent corruption" in text,
        "abort": "Data Abort" in text,
        "alive": "alive loop=" in alive,
        "probe": None,
    }
    m = PROBE_RE.search(text)
    if m:
        res["probe"] = tuple(int(m.group(k), 16) for k in (1, 2, 3))

    print(f"  对照组标记        : {'出现' if res['control'] else '未出现'}")
    print(f"  静默损坏被读到    : {'是' if res['silent'] else '否'}")
    print(f"  这一步有 Data Abort: {'有' if res['abort'] else '无'}")
    print(f"  内核仍在推进      : {'是' if res['alive'] else '否'}")
    if res["probe"]:
        b, tp, g = res["probe"]
        print(f"  探针栈            : base=0x{b:08X} top=0x{tp:08X} guard=0x{g:08X}")
        print(f"  guard == base-4096: {g == b - 4096}")

    return res


def stage_trip(log: SerialLog, selector: int, expect_fs: int, label: str) -> dict:
    """阶段 C / D:让 guard 生效,跑同一段溢出。会停机,所以每次加载只能跑一个。"""
    print(f"\n阶段 {label}:guard 生效 —— 选择器 {selector}(同一段代码、同一批地址)")
    print(f"  写 0x{FAULT_SEL_ADDR:08X} = {selector}(读回 0x{poke(selector)})")

    text = log.wait_for("System halted", 12.0)
    for line in text.splitlines():
        if line.strip():
            print(f"      {line.rstrip()}")

    m = DFAR_RE.search(text)
    dfar = int(m.group(1), 16) if m else None
    m = DFSR_RE.search(text)
    dfsr = int(m.group(1), 16) if m else None
    fs = fs_of(dfsr) if dfsr is not None else -1
    m = DOMAIN_RE.search(text)
    domain = int(m.group(1)) if m else -1
    wnr = int(m.group(2)) if m else -1
    m = PROBE_RE.search(text)
    probe = tuple(int(m.group(k), 16) for k in (1, 2, 3)) if m else None

    print(f"  → Data Abort={'有' if 'Data Abort' in text else '无'}"
          f"   DFAR={f'0x{dfar:08X}' if dfar is not None else '?'}"
          f"   FS[4:0]=0x{fs:02X}(期望 0x{expect_fs:02X})"
          f"   WnR={wnr}   domain={domain}")

    return {"abort": "Data Abort" in text, "dfar": dfar, "fs": fs, "wnr": wnr, "probe": probe}


def probe_of(dst: dict, fallback: dict) -> tuple | None:
    """取探针地址:优先本阶段打印的,退而求其次用对照组打印的。"""
    for d in (dst, fallback):
        if d and d.get("probe"):
            return d["probe"]
    return None


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

    do_load = "--load" in sys.argv

    # ==================================================================
    # 第一次加载:B(共用对照组)+ C(guard 用"不映射"实现)
    # ==================================================================
    log = open_boot(do_load)
    if log is None:
        return 2

    if not stage_report(log):
        log.close()
        return 2

    b = stage_control(log)
    c = stage_trip(log, SEL_GUARD, 0x07, "C")
    log.close()

    probe = probe_of(c, b)
    ok_guard = ok_dfar = ok_fs = False
    if probe:
        pbase, _ptop, pguard = probe
        ok_guard = pguard == pbase - 4096
        ok_dfar = c["dfar"] == pbase - 4
        print(f"\n  探针 guard == base-4096 : {ok_guard}")
        print(f"  C 的 DFAR == base-4     : {ok_dfar}   "
              f"(DFAR={f'0x{c[chr(100) + chr(102) + chr(97) + chr(114)]:08X}' if c['dfar'] is not None else '?'}"
              f" 期望=0x{pbase - 4:08X})")
    ok_fs = c["fs"] == 0x07
    print(f"  C 的 FS[4:0] == 0x07    : {ok_fs}")
    print(f"  C 的 WnR == 1(写)       : {c['wnr'] == 1}")

    clean_control = b["control"] and b["silent"] and not b["abort"] and b["alive"]
    unmapped_ok = clean_control and c["abort"] and ok_guard and ok_dfar and ok_fs and c["wnr"] == 1

    if unmapped_ok:
        print("\n  ✅ A/B 1(guard = 不映射):同一地址、同一段代码,只差这一页能不能访问 ——")
        print("     行为从『静默损坏』变成『当场 Data Abort,DFAR 落在 guard 页里』。")
    else:
        print("\n  ❌ A/B 1(guard = 不映射)不成立:")
        print(f"     对照组干净(无异常+静默损坏可见+内核仍推进) = {clean_control}")
        print(f"     C 报错={c['abort']}  guard 几何={ok_guard}  DFAR 对={ok_dfar}  FS 对={ok_fs}")

    # ==================================================================
    # 第二次加载:D(guard 用 AP=0b000 实现)—— 两组里更紧的一组
    # ==================================================================
    ap_ran = do_load
    ap_ok = False

    if do_load:
        print("\n" + "=" * 68)
        print("第二次加载:阶段 D —— guard 改用 AP=0b000")
        print("=" * 68)

        log = open_boot(True)
        if log is None:
            return 2
        if not stage_report(log):
            log.close()
            return 2

        d = stage_trip(log, SEL_GUARD_AP, 0x0F, "D")
        log.close()

        probe = probe_of(d, None)
        ok_guard = ok_dfar = False
        if probe:
            pbase, _ptop, pguard = probe
            ok_guard = pguard == pbase - 4096
            ok_dfar = d["dfar"] == pbase - 4
            print(f"\n  探针 guard == base-4096 : {ok_guard}")
            print(f"  D 的 DFAR == base-4     : {ok_dfar}   "
                  f"(DFAR={f'0x{d[chr(100) + chr(102) + chr(97) + chr(114)]:08X}' if d['dfar'] is not None else '?'}"
                  f" 期望=0x{pbase - 4:08X})")
        print(f"  D 的 FS[4:0] == 0x0F    : {d['fs'] == 0x0F}"
              "   (permission fault, level 2 —— 不是 0x07)")
        print(f"  D 的 WnR == 1(写)       : {d['wnr'] == 1}")

        ap_ok = (bool(probe) and d["abort"] and ok_guard and ok_dfar and d["fs"] == 0x0F
                 and d["wnr"] == 1)
        if ap_ok:
            print("\n  ✅ A/B 2(guard = AP=0b000):两阶段里 guard 页**都是映射着的**,")
            print("     唯一差别就是 AP 是不是 0b000 —— 行为随之从『无异常』变成")
            print("     『permission fault, level 2』。")
            print("     这同时证明:**DACR = client 模式下 AP 确实被硬件执行** ——")
            print("     那是 M2-4 当年只做过推理、没做过验证的一条。")
        else:
            print("\n  ❌ A/B 2(guard = AP=0b000)不成立")

    # ==================================================================
    print("\n" + "=" * 68)
    print("判定")
    print("=" * 68)
    print(f"  A/B 1  guard = 不映射(选择器 6 vs 8)  : {'成立' if unmapped_ok else '不成立'}")
    if ap_ran:
        print(f"  A/B 2  guard = AP=0b000(选择器 7 vs 8): {'成立' if ap_ok else '不成立'}")
    else:
        print("  A/B 2  guard = AP=0b000                : 未跑(需要 --load,它要第二次加载)")

    if unmapped_ok and ap_ok:
        print("\n  ✅ guard page 是承重的 —— 第三级证据成立")
        print("     (两条独立实现路径各有一组对照,而且第二组比第一组更紧:")
        print("      两阶段里那一页都映射着,只差 AP)")
        return 0

    if unmapped_ok and not ap_ran:
        print("\n  ⚠ 只验了 A/B 1。加 --load 才能把 A/B 2 也跑掉。")
        return 0

    if not unmapped_ok:
        print("\n  ❌ A/B 1 不成立 —— guard 没生效,或异常来源不是 guard。")
        return 1

    print("\n  ❌ A/B 2 不成立 —— AP=0b000 没有拦住访问。")
    print("     最可能的原因:DACR 不在 client 模式(manager 模式下 AP 被完全忽略)。")
    return 1


if __name__ == "__main__":
    sys.exit(main())
