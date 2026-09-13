"""回归:FSR 译码改过之后,以前验证过的几条故障路径必须仍然译对。

======================================================================
为什么必须单独跑这一遍
======================================================================

guard page(M4-5)第一次上板时暴露了 DFSR 译码的一个真 bug:
DFSR 的 **bits[7:4] 是 Domain**,正好压在状态位 FS[3:0] 上面,
所以 `dfsr & 0x1F` 会把 Domain[0] 混进 FS[4]。本内核 domain=15,
实测 `DFSR = 0x000008F7` 被 `& 0x1F` 读成 0x17("保留/未知"),
而按位域读是 0x07(translation fault, level 2)。

修法动的是**所有**故障诊断路径共用的那段译码 —— 包括 M1 阶段就验证过的
Data Abort / Prefetch Abort / XN 取指三条。所以按本项目"改了就要重验"的
规矩,这三条必须再看一遍,而不是假设"只是换了个查表方式"。

判据刻意宽松但**不空**:不比对具体文字(那种检查会在改措辞时误报),
而是要求
  1. 确实报了对应的异常类型;
  2. 状态**译得出来** —— 不能是 "reserved / unknown";
  3. Fault address 与注入时选的地址一致。
第 2 条是关键:改动之前,域非 0 的故障正好会译成"保留/未知",
而那种文本在串口上和"另一个保留码"长得一样,不写进判据就查不出来。
"""

import re
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from guard_trip import FAULT_SEL_ADDR, LOAD_TCL, PORT, BAUD, XSDB, ROOT, SerialLog, poke  # noqa: E402

# (选择器, 期望的异常标题, 期望的地址或 None, 说明)
CASES = [
    (1, "Data Abort", 0x50000000, "读没有从设备的地址"),
    (3, "Prefetch Abort", 0x50000000, "跳到没有从设备的地址"),
    (5, "Prefetch Abort", 0xE0001000, "从 XN 区域取指"),
]

STATUS_RE = re.compile(r"(DFSR|IFSR)\s+=\s+0x([0-9A-F]{8})\s+\(([^)]*)\)")
FAR_RE = re.compile(r"(DFAR|IFAR)\s+=\s+0x([0-9A-F]{8})")


def one_case(log: SerialLog, selector: int, title: str, expect_addr, note: str) -> bool:
    print(f"\n--- 选择器 {selector}:{note} ---")
    poke(selector)

    text = log.wait_for("System halted", 12.0)

    ok = True
    if title not in text:
        print(f"    ❌ 没有出现 {title!r}")
        ok = False
    else:
        print(f"    ✅ 异常类型正确:{title}")

    m = STATUS_RE.search(text)
    if not m:
        print("    ❌ 没读到状态寄存器")
        return False

    reg, value, decoded = m.group(1), int(m.group(2), 16), m.group(3)
    print(f"    {reg} = 0x{value:08X}  ->  {decoded}")

    if "reserved" in decoded or "unknown" in decoded:
        print("    ❌ 状态码译成了『保留/未知』—— 译码表或位域仍然不对")
        ok = False
    else:
        print("    ✅ 状态码译得出来")

    m = FAR_RE.search(text)
    if m and expect_addr is not None:
        far = int(m.group(2), 16)
        if far != expect_addr:
            print(f"    ❌ {m.group(1)} = 0x{far:08X},期望 0x{expect_addr:08X}")
            ok = False
        else:
            print(f"    ✅ {m.group(1)} = 0x{far:08X} 与注入地址一致")

    # 附加信息:WnR / domain 打出来了没有(guard page 那次就是靠它定位的)。
    # 只有 Data Abort 有这两个字段 —— IFSR 里 bits[7:4] 是 RES0、也没有 WnR,
    # 所以判据按异常类型分开,不然会稳定地打出一条没有信息量的警告。
    if title == "Data Abort":
        if "WnR" in text and "domain" in text:
            print("    ✅ WnR / domain 已打印")
        else:
            print("    ❌ Data Abort 输出里缺 WnR/domain —— 那条线索又没了")
            ok = False
    elif "WnR" in text:
        print("    ❌ Prefetch Abort 的输出里出现了 WnR/domain —— 那是 DFSR 才有的字段")
        ok = False

    return ok


def main() -> int:
    results: list[tuple[int, bool]] = []

    for selector, title, addr, note in CASES:
        log = SerialLog(PORT, BAUD)
        time.sleep(0.4)

        print(f"\n[加载] {LOAD_TCL.name} (为选择器 {selector})")
        proc = subprocess.run([XSDB, str(LOAD_TCL)], cwd=str(ROOT), capture_output=True, text=True,
                              encoding="utf-8", errors="replace", timeout=600)
        out = (proc.stdout or "") + (proc.stderr or "")
        if proc.returncode != 0 or "FAIL" in out:
            print(out[-1500:])
            log.close()
            return 2

        # 等到主循环跑起来(自检报告已经打完了)
        log.wait_for("alive loop=", 30.0)
        ok = one_case(log, selector, title, addr, note)
        results.append((selector, ok))
        log.close()

    print("\n=== 汇总 ===")
    all_ok = True
    for selector, ok in results:
        print(f"  选择器 {selector}: {'PASS' if ok else 'FAIL'}")
        all_ok = all_ok and ok

    if all_ok:
        print("\n  ✅ 三条既有故障路径在译码改动之后仍然全部译对")
        return 0

    print("\n  ❌ 有路径译错了 —— 译码改动不能就这么留下")
    return 1


if __name__ == "__main__":
    sys.exit(main())
