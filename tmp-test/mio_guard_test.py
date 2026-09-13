"""故障注入测试:MIO bank1 前置校验真的会拦住错误的 XSA 吗?

守卫本身也必须被测。做法是把正确的 ps7_init 机械地改坏(把 MIO16-53 的
[11:9] 从 1 改回 3,即每个值 +0x400 —— 正是旧 XSA 的样子),
跑一遍 ps7_init 再看校验函数是否报错;然后再用正确的跑一遍确认它放行。

只做 rst -system + ps7_init,不加载比特流和内核,所以很快。
"""

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
XSDB = r"C:\AMDDesignTools\2025.2\Vitis\bin\xsdb.bat"
GOOD = ROOT / "tmp-test" / "zynq" / "ps7_init_uart1.tcl"
CHECK = ROOT / "tmp-test" / "zynq" / "ps7_mio_bank1_check.tcl"
BAD = ROOT / "tmp-test" / "out" / "ps7_init_badbank1.tcl"
TCL = ROOT / "tmp-test" / "out" / "mio_guard_test.tcl"


def make_bad() -> int:
    """把 MIO16-53 的 [11:9] 从 1 改回 3(值 +0x400),模拟旧的错误 XSA。"""
    text = GOOD.read_text(encoding="utf-8")
    changed = 0

    def fix(m: re.Match) -> str:
        nonlocal changed
        addr = int(m.group(1), 16)
        # 只动 MIO16..MIO53 -> 0xF8000740..0xF80007D4
        if not (0xF8000740 <= addr <= 0xF80007D4):
            return m.group(0)
        val = int(m.group(2), 16)
        if ((val >> 9) & 0x7) != 1:
            return m.group(0)
        changed += 1
        return "mask_write 0x%X 0x00003FFF 0x%08X" % (addr, val | 0x400)

    # 注意:生成的 tcl 里地址写成 `0XF8000740`(大写 X)、值写成 `0x00001200`
    # (小写 x),所以两边都要用 [xX] 兼容 —— 第一版只写了小写,结果 0 处命中。
    out = re.sub(r"mask_write 0[xX](F80007[0-9A-Fa-f]{2}) 0x00003FFF (0x[0-9A-Fa-f]{8})", fix, text)
    BAD.parent.mkdir(parents=True, exist_ok=True)
    BAD.write_text(out, encoding="utf-8", newline="\n")
    return changed


def main() -> int:
    n = make_bad()
    print(f"注入:把 {n} 个 MIO16-53 的 [11:9] 改回 3(= 旧 XSA 的错误配置)")
    # ps7_init 里有 _1_0/_2_0/_3_0 三套硅版本变体,每套都有同样的 38 行,
    # 所以预期是 38 的整数倍。三套都要改 —— 加载器按硅版本挑其中一套执行,
    # 只改一套的话测的就不是真实行为了。
    if n < 38 or n % 38 != 0:
        print(f"!! 预期是 38 的整数倍且不少于 38,实际 {n} 个 —— 注入脚本本身有问题")
        return 2
    print(f"   (对应 {n // 38} 套硅版本变体)")

    TCL.write_text(
        "\n".join(
            [
                "connect",
                'targets -set -filter {name =~ "APU"}',
                "rst -system",
                "after 2000",
                'targets -set -filter {name =~ "APU"}',
                f'source {{{BAD.as_posix()}}}',
                "ps7_init",
                "ps7_post_config",
                f'source {{{CHECK.as_posix()}}}',
                'puts "GUARD_BAD [ps7_mio_bank1_check]"',
                "",
                f'source {{{GOOD.as_posix()}}}',
                "ps7_init",
                "ps7_post_config",
                'puts "GUARD_GOOD [ps7_mio_bank1_check]"',
                "exit 0",
            ]
        )
        + "\n",
        encoding="utf-8",
        newline="\n",
    )

    proc = subprocess.run([XSDB, str(TCL)], cwd=str(ROOT), capture_output=True, text=True, timeout=300)
    out = (proc.stdout or "") + (proc.stderr or "")
    bad = re.search(r"GUARD_BAD (\d+)", out)
    good = re.search(r"GUARD_GOOD (\d+)", out)
    if not bad or not good:
        print(out[-2000:])
        print("!! 拿不到校验结果")
        return 2

    print(f"\n错误配置下的不合规引脚数: {bad.group(1)}  (期望 38)")
    print(f"正确配置下的不合规引脚数: {good.group(1)}  (期望 0)")

    if int(bad.group(1)) == 38 and int(good.group(1)) == 0:
        print("\n✅ 守卫有效:错误 XSA 会被拦下,正确的放行")
        return 0
    print("\n❌ 守卫没起到作用")
    return 1


if __name__ == "__main__":
    sys.exit(main())
