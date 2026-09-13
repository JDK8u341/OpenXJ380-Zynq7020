"""判定 irq_spurious 到底是"真的虚假中断"还是"内存被写坏"。

背景:`irq_spurious` 出现 1285 的那两次是间歇性的(5 次运行里 2 次),
而"间歇 + 一次固定值"既像竞态,也像内存越界写坏了一个计数器。两者
排查方向完全相反,必须先分开。

判据来自 c_irq_handler 的执行顺序 —— irq_count++ 发生在 spurious 判断
**之前**,所以如果那 1285 是真的虚假中断,CPU0 上必然满足:

    irq_count = ticks + spurious + unhandled

(unhandled 为 0;CPU0 只收自己的私有定时器 PPI,不收 SGI。)

    恒等式成立 -> 中断真的发生了 1285 次,是 GIC ack 竞态那一类
    恒等式不成立 -> 计数本身被别的写入破坏了,是内存问题

⚠ 读数有**固有偏差**:5 次 mrd 是顺序执行的,每次经 hw_server 要 10-20ms,
  1kHz 下先读的 irq_count 会比后读的 ticks 少几十。实测差值 -32 就是这个,
  **不是 bug**。判据只在 spurious 非 0 时有意义,那时 1285 远大于偏差。

用法: python tmp-test/spurious_probe.py
"""

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
# 本机路径(工具链/串口/比特流)全在仓库根目录的 config.py —— 换机器只改那一个文件。
sys.path.insert(0, str(ROOT))
import config  # noqa: E402

XSDB = str(config.XSDB)
NM = str(config.ARM_NM)
ELF = ROOT / "out" / "kernel-arm.elf"

# irq_stats_t 里各字段的偏移(见 include/arch/irq.h)
OFF_IRQ = 0x00
OFF_SPURIOUS = 0x04
OFF_UNHANDLED = 0x08

# percpu_t 里 ticks 的偏移(见 include/arch/percpu.h)
OFF_PC_TICKS = 0x10


def percpu_layout() -> tuple[int, int]:
    """从**当前头文件**算出 sizeof(percpu_t) 与 ticks 的偏移。

    ⚠ 原来这两个数是硬编码的(40 / 0x10),而 M4-6 往 percpu_t 里加了
      current_task 之后 40 就过期了 —— 本脚本会安静地去读 CPU1 结构体
      错位后的字节,读出来的数看着像模像样,其实毫无意义。
      这种"工具自己过期"的坑不值得再踩一次,所以改成每次现算。

    做法:让宿主编译器算好再用 objdump/nm 不方便,直接用 clang 编一个
    只打印这两个数的临时程序。宿主与目标在 percpu_t 上布局一致,因为
    该结构体**只含 u32 字段**(这是 arch/percpu.h 里的刻意约束)。
    """
    import subprocess as sp
    import tempfile

    src = """
int printf(const char *fmt, ...);
#include <arch/percpu.h>
int main(void)
{
    printf("%u %u\\n", (unsigned)sizeof(percpu_t),
           (unsigned)__builtin_offsetof(percpu_t, ticks));
    return 0;
}
"""
    with tempfile.TemporaryDirectory() as tmp:
        c = Path(tmp) / "p.c"
        exe = Path(tmp) / "p.exe"
        c.write_text(src, encoding="utf-8")
        for cc in ("clang", "gcc", "cc"):
            r = sp.run([cc, "-std=c11", "-w", "-I", str(ROOT / "arch/arm32/include"), str(c),
                        "-o", str(exe)], capture_output=True, text=True)
            if r.returncode == 0:
                out = sp.run([str(exe)], capture_output=True, text=True).stdout.split()
                return int(out[0]), int(out[1])
    raise RuntimeError("算不出 percpu_t 的布局:没有可用的宿主 C 编译器")


PERCPU_SIZE, OFF_PC_TICKS = percpu_layout()


def sym(name: str) -> int:
    out = subprocess.run([NM, str(ELF)], capture_output=True, text=True, timeout=60).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == name:
            return int(parts[0], 16)
    raise RuntimeError(f"符号 {name} 找不到")


def main() -> int:
    stats = sym("g_irq_stats")
    percpu = sym("g_percpu")

    tcl = ROOT / "tmp-test" / "out" / "_spurious_probe.tcl"
    tcl.parent.mkdir(parents=True, exist_ok=True)

    reads = {
        "irq_count": stats + OFF_IRQ,
        "spurious": stats + OFF_SPURIOUS,
        "unhandled": stats + OFF_UNHANDLED,
        "cpu0_ticks": percpu + OFF_PC_TICKS,
        "cpu1_ticks": percpu + PERCPU_SIZE + OFF_PC_TICKS,
    }
    body = ["connect", 'targets -set -filter {name =~ "ARM*#0"}']
    for name, addr in reads.items():
        body.append(f'puts "PROBE {name} 0x{addr:X} [mrd -force 0x{addr:X}]"')
    body.append("exit 0")
    tcl.write_text("\n".join(body) + "\n", encoding="utf-8", newline="\n")

    out = subprocess.run([XSDB, str(tcl)], cwd=str(ROOT), capture_output=True, text=True, timeout=180)
    text = (out.stdout or "") + (out.stderr or "")

    vals: dict[str, int] = {}
    for m in re.finditer(r"PROBE (\w+) 0x[0-9A-F]+ .*?:\s*([0-9A-Fa-f]{8})", text):
        vals[m.group(1)] = int(m.group(2), 16)

    if len(vals) != len(reads):
        print(text[-1500:])
        print("!! 读数不完整,本次无效")
        return 2

    print("读数")
    for k, v in vals.items():
        print(f"  {k:12s} = {v}  (0x{v:08X})")

    lhs = vals["irq_count"]
    rhs = vals["cpu0_ticks"] + vals["spurious"] + vals["unhandled"]

    print()
    print(f"  恒等式  irq_count({lhs})  =?  ticks({vals['cpu0_ticks']}) + spurious({vals['spurious']})"
          f" + unhandled({vals['unhandled']})  =  {rhs}")
    print(f"  差值    {lhs - rhs}")

    print()
    if vals["spurious"] == 0:
        print("  当前 spurious = 0 —— 这次没有复现,无法判定。")
        return 1

    if lhs == rhs:
        print("  ✅ 恒等式成立 —— 那 1285 是**真的虚假中断**,属 GIC ack 竞态一类。")
        print("     下一步:用二分法隔离(先关 SGI 段,再关 stress),统计出现率变化。")
        return 0

    print("  ❌ 恒等式不成立 —— 计数不是被中断累加出来的,而是**被别的写入破坏**了。")
    print("     这改变了排查方向:该去看 .bss 里 g_irq_stats 附近的越界写,而不是 GIC。")
    return 1


if __name__ == "__main__":
    sys.exit(main())
