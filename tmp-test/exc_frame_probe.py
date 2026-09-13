"""测量本板 Cortex-A9 各异常下 LR 的偏移 —— 给 M4-6 的寄存器帧定契约。

======================================================================
为什么必须**实测**而不是查文档
======================================================================

异常入口的 LR 是"首选返回地址 + 一个取决于异常类型与指令集的偏移"。
文档给的是规则,但**具体是几**要靠硬件回答,而且两条用途的答案不同:

  1. **返回**:`movs pc, lr` 要跳到哪 —— 跳错了会重复执行或跳过一条指令;
  2. **诊断**:打印出来的 `pc` 要指向**真正出错/被中断的那条指令** ——
     偏 4 字节会把人引到隔壁那条指令上。

这两件事在 x86 上由硬件压栈的 `rip` 一并解决,在 ARM 上则**每个异常类型都不同**。
M4-7 的上下文切换要在这个基础上建,所以这里先把表测出来。

======================================================================
做法
======================================================================

注入点地址从 **ELF 里读**(objdump 找 `fault_test_trigger` 范围内那两条
已知指令的编码),不写死 —— 每次重新构建后地址都会变。

  - `svc #0` 编码 `ef000000`  → 选择器 4(SVC,SVC 处理函数**会返回**)
  - UDF 编码 `e7f000f0`       → 选择器 2(Undefined Instruction,会停机)
  - 溢出写 `str r2,[r3],#-4`  → 选择器 6(Data Abort,会停机)

一次加载里先跑 4(会返回,不打断后续),再跑 2(停机)。Data Abort 的偏移
由 `guard_trip.py` 的输出与 ELF 对照得到,本脚本不重复跑(它会停机)。

输出:每个异常类型下"处理函数打印的 pc"与"ELF 里那条真指令的地址"之差。
"""

import re
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from guard_trip import (  # noqa: E402
    BAUD,
    FAULT_SEL_ADDR,
    LOAD_TCL,
    PORT,
    ROOT,
    XSDB,
    SerialLog,
    poke,
)

ELF = ROOT / "out" / "kernel-arm.elf"
OBJDUMP = (r"C:\AMDDesignTools\2025.2\gnu\aarch32\nt\gcc-arm-none-eabi"
           r"\bin\arm-none-eabi-objdump.exe")

PC_RE = re.compile(r"pc\s+=\s+0x([0-9A-F]{8})")


def find_in_function(func: str, encoding: str) -> int:
    """在某个函数的反汇编里找一条已知编码的指令,返回它的地址。"""
    out = subprocess.run([OBJDUMP, "-d", str(ELF)], capture_output=True, text=True, encoding="utf-8",
                         errors="replace", timeout=120).stdout

    start = None
    for line in out.splitlines():
        if line.endswith(f"<{func}>:"):
            start = int(line.split()[0], 16)
            break
    if start is None:
        raise RuntimeError(f"找不到函数 {func}")

    best = None
    for line in out.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\s+([0-9a-f]{8})\s", line)
        if not m:
            continue
        addr, enc = int(m.group(1), 16), m.group(2)
        if addr < start:
            continue
        # 函数之间按地址升序,遇到下一个函数符号就停
        if line.strip().endswith(">:") and addr > start:
            break
        if enc == encoding:
            # 同一个编码可能出现多次,取"属于本函数且最靠前"的那个
            if best is None or addr < best:
                best = addr

    if best is None:
        raise RuntimeError(f"在 {func} 里找不到编码 {encoding}")
    return best


def main() -> int:
    udf_addr = find_in_function("fault_test_trigger", "e7f000f0")
    svc_addr = find_in_function("fault_test_trigger", "ef000000")
    # 选择器 5 是 `ldr r0,=0xE0001000; blx r0`,blx 的编码是 e12fff30
    blx_addr = find_in_function("fault_test_trigger", "e12fff30")

    # 会停机的那个只能挑一个(停了就跑不了第二个),默认取 prefetch
    halting = int(sys.argv[1]) if len(sys.argv) > 1 else 5
    halting_name = {2: "Undefined", 5: "Prefetch(XN)"}.get(halting, f"sel{halting}")

    print("ELF 里的已知指令")
    print(f"  svc #0            @ 0x{svc_addr:08X}   (选择器 4,会返回)")
    print(f"  .word 0xe7f000f0  @ 0x{udf_addr:08X}   (选择器 2,会停机)")
    print(f"  blx r0            @ 0x{blx_addr:08X}   (选择器 5,会停机)")
    print(f"  本次会停机的用例:选择器 {halting}({halting_name})")
    print()

    log = SerialLog(PORT, BAUD)
    time.sleep(0.4)

    print(f"加载内核({LOAD_TCL.name})...")
    proc = subprocess.run([XSDB, str(LOAD_TCL)], cwd=str(ROOT), capture_output=True, text=True,
                          encoding="utf-8", errors="replace", timeout=600)
    out = (proc.stdout or "") + (proc.stderr or "")
    if proc.returncode != 0 or "FAIL" in out:
        print(out[-2000:])
        log.close()
        return 2
    log.wait_for("alive loop=", 30.0)

    results: list[tuple[str, int, int]] = []

    # ---- 选择器 4:SVC,处理函数会返回,所以不会打断后续 ----
    print("\n--- 选择器 4:SVC(#13 目前是诊断占位,会返回)---")
    poke(4)
    text = log.wait_for("Returning to the instruction after SVC", 8.0)
    printed = None
    for line in text.splitlines():
        if line.strip():
            print(f"      {line.rstrip()}")
        m = PC_RE.search(line)
        if m:
            printed = int(m.group(1), 16)
    if printed is not None:
        results.append(("SVC", printed, svc_addr))

    # ---- 会停机的那个用例 ----
    print(f"\n--- 选择器 {halting}:{halting_name}(停机)---")
    poke(halting)
    text = log.wait_for("System halted", 10.0)
    printed = None
    for line in text.splitlines():
        if line.strip():
            print(f"      {line.rstrip()}")
        m = PC_RE.search(line)
        if m:
            printed = int(m.group(1), 16)
    if printed is not None:
        real = udf_addr if halting == 2 else blx_addr
        results.append((halting_name, printed, real))

    log.close()

    # ---- 结论 ----
    print("\n" + "=" * 66)
    print("测量结果")
    print("=" * 66)
    print(f"  {'异常':<12} {'打印的 pc':<14} {'真指令地址':<14} {'差':<6} 含义")
    print(f"  {'-' * 12} {'-' * 14} {'-' * 14} {'-' * 6} {'-' * 40}")
    for name, printed, real in results:
        delta = printed - real
        if delta == 0:
            what = "打印的 pc == 出错的那条指令(正确)"
        elif delta > 0:
            what = f"打印的 pc 指向出错指令之后第 {delta // 4} 条"
        else:
            what = f"打印的 pc 指向出错指令之前第 {-delta // 4} 条"
        print(f"  {name:<12} 0x{printed:08X}     0x{real:08X}     {delta:<+6} {what}")

    print("\n  Data Abort 由 guard_trip.py 测量(它会停机,本脚本不重复跑),")
    print("  两者共用同一份 ELF,所以地址可以直接对照:")
    print("      修复前:打印 pc = 0x00105C38,而 objdump 显示出错指令是")
    print("              `str r2,[r3],#-4` @ 0x00105C34 → 偏 +4(指向了下一条 bne)")
    print("      修复后:打印 pc = 0x00105F24,objdump 显示")
    print("              `str r2,[r3],#-4` @ 0x00105F24 → 正好是那条指令")

    if not results:
        print("\n  ❌ 一个 pc 都没读到 —— 本次无效")
        return 2

    bad = [(n, pr, re_) for n, pr, re_ in results if pr != re_]
    print()
    if bad:
        print("  ❌ 仍有异常的 pc 与真指令对不上:")
        for n, pr, re_ in bad:
            print(f"     {n}: 打印 0x{pr:08X} vs 真指令 0x{re_:08X}")
        print("     ⇒ 该异常在 vectors.S 的 EXC_FRAME_ENTER 里减错了")
        return 1

    print("  ✅ 本次测到的异常,打印的 pc 都**正好**是出错的那条指令")
    print("     (判据是地址相等,不是「看起来差不多」—— 偏 4 字节在这个尺度上")
    print("      就是指向了隔壁那条指令,而那正是修复前的状态)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
