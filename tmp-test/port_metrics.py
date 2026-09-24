#!/usr/bin/env python3
"""移植度量与不变式体检（P0 落地）。

来源:`docs/ZYNQ7020_PORT_CHARTER.md`。宪章定了五条不变式(I1–I5)与两个度量
(M1/M2),但**约束只有在能被一条命令复现时才算数** —— 否则每次都要人去数,
而"人去数"正是之前漂移七天的原因:没人报数。

用法(仓库根目录):

    python3 tmp-test/port_metrics.py            # 报告
    python3 tmp-test/port_metrics.py --verbose  # 附逐文件明细

设计上的三条自律:

1. **只读**。不写任何文件、不碰硬件、不改状态 —— 所以它可以随时跑、
   也可以放进 CI。
2. **不抄数**。所有事实都从源头取:`tools/gen_ninja.py` 的构建图清单、
   `git diff --numstat`、源文件本身。抄一份常量就会和现实分家,
   而那正是"量出来的数"与"真实情况"分叉的经典原因。
3. **口径写在代码里**。每个数字旁边写清它怎么算的,含糊的指标等于没有指标。

⚠ 它是**静态**体检:不做编译。上游文件"能不能编过"要用
`tmp-test/measure_arm_cxx.py`(它用 `-c`,不是 `-fsyntax-only`)。
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# ---------------------------------------------------------------------------
# 口径定义(改这些就等于改指标,必须同步改宪章)
# ---------------------------------------------------------------------------

# I1:哪六个目录算"上游"。宪章 §二 写死了这六个 —— 不含 kmod/third_party
# (前者是可加载模块、后者是 vendored 代码,它们不是"源 OS 的核心"）。
UPSTREAM_DIRS = ("kernel", "driver", "lib", "include", "boot", "user")

# vendored FatFs:它在 driver/ 下,但**不是源 OS 自有代码**(第三方库)。
# M1 的分母按宪章口径 = 上游全部 − vendored FatFs。
VENDORED = ("driver/fs/fatfs/ff.cpp", "driver/fs/fatfs/ffunicode.cpp",
            "driver/fs/fatfs/ffsystem.cpp", "driver/fs/fatfs/diskio.cpp",
            "driver/fs/fatfs/fatfs.cpp")

# N-A(不适用):**这块板上没有这个硬件**,原文永远不可能跑。
#
# ⚠ 判据是"硬件不存在",不是"还没做"。所以:
#   - `kernel/pctable/`(IDT/GDT)、`kernel/intr/apic.cpp`、`cpuid.cpp`、`fsgsbase.cpp`、
#     `boot/*.c`(UEFI)、`driver/power.cpp` **不算 N-A** —— 它们的**角色**在 ARM 上
#     存在(中断控制器=GIC、每 CPU 基址=TPIDR、引导器=stage0、复位=SLCR),
#     属于 P-B「同名角色重写」。
#   - `driver/ps2/`、`driver/hda/`、`sb16`、`dma`、`ahci/nvme/ide/pci` 才算 N-A:
#     板上没有 PS/2 控制器、没有 HD Audio、没有 ISA DMA、没有 PCI 总线。
#     (PS/2 的**输入角色**由源 OS 自己的 `kmod/xhci` USB HID 路径承担。)
NOT_APPLICABLE = (
    "driver/ahci/", "driver/nvme/", "driver/ide/", "driver/pci/",
    "driver/ps2/", "driver/hda/", "driver/sb16.cpp", "driver/dma.cpp",
)

# I2:源 OS 里**没有对应角色**的移植侧文件(I2 违例),按宪章 §八 待撤销。
# 值 = 撤销后的替代物,写进报告是为了让人一眼看到"要回到哪里去"。
INVENTED = {
    "arch/arm32/include/arch/plat_device.h": "删：改生成常量头",
    "arch/arm32/src/plat_device.c": "删：匹配框架不存在于源 OS",
    "arch/arm32/src/board.c": "删：改上游风格 xxx_setup() 序列",
    "arch/arm32/src/board_devices.c": "删：改生成常量头",
    "arch/arm32/include/arch/board_devices.h": "删：改生成常量头",
    "arch/arm32/include/arch/board.h": "删：改生成常量头",
    "arch/arm32/src/axi_gpio.c": "改写成上游风格驱动(device_t 或纯 setup)",
    "arch/arm32/include/arch/axi_gpio.h": "同上",
    "arch/arm32/src/led.c": "同上",
    "arch/arm32/include/arch/led.h": "同上",
    "arch/arm32/src/fault_test.c": "移到 tmp-test/jtag 工具侧",
    "arch/arm32/include/arch/fault_test.h": "同上",
    "arch/arm32/src/shell.c": "删：源 OS 的 shell 是用户态 user/cli_shell.cpp",
    "arch/arm32/include/arch/shell.h": "同上",
    "arch/arm32/src/selftest.c": "搬出内核：判定交主机脚本",
    "arch/arm32/include/arch/selftest.h": "同上",
    "arch/arm32/src/vfs_check.c": "并入主机侧断言",
    "arch/arm32/src/fatfs_check.c": "并入主机侧断言",
    "arch/arm32/src/pipe_check.c": "并入主机侧断言",
    "arch/arm32/src/pty_check.c": "并入主机侧断言",
    "arch/arm32/include/arch/vfs_check.h": "同上",
    "arch/arm32/include/arch/fatfs_check.h": "同上",
    "arch/arm32/include/arch/pipe_check.h": "同上",
    "arch/arm32/include/arch/pty_check.h": "同上",
}

# I3:移植侧自造、应当改成上游同名服务的 API（键 = 现在的名字，值 = 上游名字）
API_ALIASES = {
    "vmap_": "page_map_range / unmap_page_range / translate_address",
    "palloc_": "alloc_frames / free_frames",
    "kstack_": "(上游 kernel/task/pcb.cpp 内的栈管理)",
    "console_": "write_serial_* / sprintf / snprintf（上游 driver/serial/serial_port.cpp）",
}

# 落地层里"拒绝型桩"的判定词。命中即计入 I5 待办。
REFUSE_WORDS = ("拒绝", "REFUSE")
UNLOCK_WORD = "解开条件"


# ---------------------------------------------------------------------------
# 基础工具
# ---------------------------------------------------------------------------

def run(*args: str) -> str:
    return subprocess.run(args, cwd=ROOT, capture_output=True, text=True,
                          encoding="utf-8", errors="replace").stdout


def code_lines_text(text: str) -> int:
    """非空、非注释的代码行数。

    与宪章口径一致:注释与空行不算 —— 移植侧注释占 45%,把它算进去会让
    体量虚高 2.4 倍,而虚高的数字没法用来判断"是不是写多了"。
    """
    code = 0
    in_block = False
    for raw in text.splitlines():
        if not raw.strip():
            continue
        had = False
        i, n = 0, len(raw)
        while i < n:
            if in_block:
                j = raw.find("*/", i)
                if j < 0:
                    i = n
                else:
                    in_block = False
                    i = j + 2
                continue
            if raw.startswith("//", i):
                break
            if raw.startswith("/*", i):
                in_block = True
                i += 2
                continue
            if not raw[i].isspace():
                had = True
            i += 1
        if had:
            code += 1
    return code


def code_lines(path: Path) -> int:
    if not path.is_file():
        return 0
    return code_lines_text(path.read_text(encoding="utf-8", errors="replace"))


def upstream_sources() -> list[Path]:
    out = []
    for d in UPSTREAM_DIRS:
        for p in (ROOT / d).rglob("*"):
            if p.suffix in (".c", ".cpp") and p.is_file():
                out.append(p)
    return sorted(out)


def is_not_applicable(rel: str) -> bool:
    return any(rel.startswith(pref) for pref in NOT_APPLICABLE)


def build_graph_upstream() -> list[str]:
    """从构建图里读"正在编的上游 C++ 文件"。

    不抄清单:抄了就会和构建图分家,而"哪些上游文件真的在跑"是整个 M1 的
    分子 —— 它一旦失真,指标就变成自欺。
    """
    src = (ROOT / "tools" / "gen_ninja.py").read_text(encoding="utf-8", errors="replace")
    start = src.index("ARM32_UPSTREAM_CXX = (")
    end = src.index("\n)\n", start)
    block = src[start:end]
    names = re.findall(r'"([^"]+\.cpp)"', block)
    return [n for n in names if "/" in n and not n.startswith("arch/")]


# ---------------------------------------------------------------------------
# 各项体检
# ---------------------------------------------------------------------------

def metric_m1():
    """M1:上游原文在 ARM 上运行的代码行 / 上游适用代码行。"""
    in_graph = build_graph_upstream()
    num_all = sum(code_lines(ROOT / f) for f in in_graph)
    num_vend = sum(code_lines(ROOT / f) for f in in_graph if f in VENDORED)
    num_own = num_all - num_vend

    all_src = upstream_sources()
    den_all = sum(code_lines(p) for p in all_src)
    den_na = sum(code_lines(p) for p in all_src
                 if is_not_applicable(p.relative_to(ROOT).as_posix()))
    den_own = den_all - sum(code_lines(ROOT / f) for f in VENDORED)
    den_own_na = den_own - den_na

    return {
        "files": len(in_graph), "files_total": len(all_src),
        "num_all": num_all, "num_own": num_own, "num_vend": num_vend,
        "den_all": den_all, "den_own": den_own, "den_na": den_na,
        "den_own_na": den_own_na,
    }


def invariant_i1():
    """I1:上游零修改。返回 (总行数, 逐文件表)。"""
    text = run("git", "diff", "--numstat", "main", "HEAD")
    rows = []
    for line in text.splitlines():
        parts = line.split("\t")
        if len(parts) != 3:
            continue
        add, dele, path = parts
        if not path.startswith(UPSTREAM_DIRS):
            continue
        try:
            rows.append((path, int(add), int(dele)))
        except ValueError:
            continue
    rows.sort(key=lambda r: -(r[1] + r[2]))
    return sum(a + d for _, a, d in rows), rows


def invariant_i2():
    """I2:内核里的发明物(按宪章 §八 的清单点名统计)。

    两类都算:
      ① 点名文件(整份文件在源 OS 里没有对应角色);
      ② `kmain.c` 内部的自检段 —— 它不是独立文件,但那 600 多行判定
         同样是发明物,漏掉它会把 I2 报小 30%。
    """
    rows = []
    for rel, alt in INVENTED.items():
        n = code_lines(ROOT / rel)
        if n:
            rows.append((rel, n, alt))

    # ② kmain.c 的自检段:从 selftest_begin() 到 selftest_summary() 之间的代码行
    kmain = ROOT / "arch" / "arm32" / "src" / "kmain.c"
    seg = 0
    if kmain.is_file():
        lines = kmain.read_text(encoding="utf-8", errors="replace").splitlines()
        start = end = None
        for i, line in enumerate(lines):
            if start is None and "selftest_begin()" in line:
                start = i
            if "selftest_summary(" in line:
                end = i
        if start is not None and end is not None and end > start:
            seg = code_lines_text("\n".join(lines[start:end + 1]))
            rows.append(("arch/arm32/src/kmain.c[自检段]",
                         seg, "搬出内核：判定交主机脚本"))

    rows.sort(key=lambda r: -r[1])
    return sum(r[1] for r in rows), rows


def invariant_i3():
    """I3:自造 API(启发式:在移植侧头文件里找已知的自造前缀)。

    为什么是启发式:判断"这个名字算不算新造"需要语义,机器只能按约定找。
    所以它报的是**待人工确认的清单**,不是判决。
    """
    hits = {}
    for hdr in (ROOT / "arch" / "arm32" / "include").rglob("*.h"):
        text = hdr.read_text(encoding="utf-8", errors="replace")
        for pref, upstream in API_ALIASES.items():
            n = len(re.findall(rf"\b{re.escape(pref)}\w+\s*\(", text))
            if n:
                hits.setdefault(pref, [0, upstream, []])
                hits[pref][0] += n
                hits[pref][2].append(hdr.relative_to(ROOT).as_posix())
    return hits


def invariant_i4():
    """I4:内核里的判定(自检)。内核只该打印事实。"""
    kmain = ROOT / "arch" / "arm32" / "src" / "kmain.c"
    text = kmain.read_text(encoding="utf-8", errors="replace")
    judges = len(re.findall(r"selftest_report\s*\(", text))
    refs = len(re.findall(r"\bselftest_\w+", text))
    return judges, refs


def is_pure_refusal(body: str) -> bool:
    """函数体是不是"除了常量失败返回什么都没做"。

    为什么需要这一步:第一版只看"有小常量返回 + 附近提到 x86",于是把
    `write_serial_fmt()` 也报了 —— 它是真实现(格式化 + 写串口),只是
    与源 OS 一样恒返回 0。指标一旦误报就会被忽略,所以这里收紧:

      去掉注释、`(void)参数;`、计数器自增、大括号之后,**只允许剩一条**
      `return <常量>;`。剩下别的东西 = 它做了实事 = 不是拒绝型桩。
    """
    lines = []
    in_block = False
    for raw in body.splitlines():
        s = raw.strip()
        if not s:
            continue
        out, i, n = [], 0, len(raw)
        while i < n:
            if in_block:
                j = raw.find("*/", i)
                if j < 0:
                    i = n
                else:
                    in_block = False
                    i = j + 2
                continue
            if raw.startswith("//", i):
                break
            if raw.startswith("/*", i):
                in_block = True
                i += 2
                continue
            out.append(raw[i])
            i += 1
        s = "".join(out).strip()
        if not s or s in ("{", "}"):
            continue
        if re.fullmatch(r"\(void\)\s*\w+\s*;", s):          # (void)count;
            continue
        if re.fullmatch(r"\w+\s*\+\+\s*;", s):             # counter++;
            continue
        if re.fullmatch(r"return\s+(0|NULL|false|\(size_t\)-1|\(uint64_t\)-1)\s*;", s):
            lines.append("RET")
            continue
        lines.append(s)
    return lines == ["RET"]


def find_refusal_stubs(text: str):
    """找落地层里的"拒绝型桩"。

    判定规则**结构性**,不靠某个中文词 —— 靠词就会因为换一种写法而漏报
    (第一版就是这样,报了 0 个,而实际上至少有三个)。

      ① 行首是函数定义,且下一行是 `{`;
      ② 函数体小(≤ 15 代码行)—— 真实现不会只有常量返回;
      ③ 体内有**常量失败返回**(`return 0/NULL/false/(size_t)-1`);
      ④ 附近(前置注释或函数体)提到 x86 / 拒绝 / 退化 / 如实 / ARM 没有 ——
         说明它是"如实报告这个平台没有这个能力",而不是真的实现。

    返回 [(名字, 文本块, 是否写了『解开条件』)]
    """
    lines = text.splitlines()
    sig = re.compile(r"^(?:extern\s+\"C\"\s+)?(?:static\s+)?[\w:<>]+[\w \*&]*\s+\**(\w+)\s*\(")
    out = []
    for i, line in enumerate(lines):
        if line.startswith((" ", "\t")):
            continue
        m = sig.match(line)
        if not m:
            continue
        if i + 1 >= len(lines) or lines[i + 1].strip() != "{":
            continue
        depth, end = 0, None
        for j in range(i + 1, min(i + 80, len(lines))):
            depth += lines[j].count("{") - lines[j].count("}")
            if depth <= 0:
                end = j
                break
        if end is None:
            continue
        # 只把**花括号里面**的部分交给判定:签名行不是语句,
        # 带上它会让"只剩一条 return"这个判据永远不成立(第一版就是这么坏的)。
        body = "\n".join(lines[i + 1:end + 1])
        if not re.search(r"return\s+(0|NULL|false|\(size_t\)-1|\(uint64_t\)-1)\s*;", body):
            continue
        if not is_pure_refusal(body):
            continue
        head = "\n".join(lines[max(0, i - 30):i])
        blob = head + "\n" + body
        if not any(w in blob for w in ("x86", "拒绝", "退化", "如实", "占位", "ARM 没有")):
            continue
        out.append((m.group(1), blob, UNLOCK_WORD in blob))
    return out


def invariant_i5():
    """I5:不隐藏缺口。三件事:链接宽容、拒绝型桩、排除表。"""
    ninja = ROOT / "tools" / "gen_ninja.py"
    text = ninja.read_text(encoding="utf-8", errors="replace")
    muldefs = "muldefs" in text

    api = ROOT / "arch" / "arm32" / "src" / "upstream_api.cpp"
    atext = api.read_text(encoding="utf-8", errors="replace")
    found = find_refusal_stubs(atext)
    stubs = [n for n, _, _ in found]
    unlocked = sum(1 for _, _, ok in found if ok)

    # 排除表条目数
    ex = 0
    if "ARM32_UPSTREAM_EXCLUDED = (" in text:
        blk = text[text.index("ARM32_UPSTREAM_EXCLUDED = ("):]
        blk = blk[:blk.index("\n)\n")] if "\n)\n" in blk else blk
        ex = len(re.findall(r'^\s{4}\(\s*$', blk, re.M)) or len(re.findall(r'"([\w/\.]+\.cpp)"', blk))
    return muldefs, stubs, unlocked, ex, [n for n, _, ok in found if not ok]


# ---------------------------------------------------------------------------
# 报告
# ---------------------------------------------------------------------------

def main() -> int:
    # Windows 控制台默认 GBK,直接 print ✓/中文会 UnicodeEncodeError。
    # 报告是给人看的,所以这里强制 UTF-8 输出(拿不到 reconfigure 就算了)。
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")

    ap = argparse.ArgumentParser()
    ap.add_argument("--verbose", action="store_true", help="附逐文件明细")
    args = ap.parse_args()

    m1 = metric_m1()
    i1_total, i1_rows = invariant_i1()
    i2_total, i2_rows = invariant_i2()
    i3 = invariant_i3()
    i4_judges, i4_refs = invariant_i4()
    muldefs, stubs, unlocked, ex, stub_missing = invariant_i5()

    OK, BAD, WARN = "[ OK ]", "[FAIL]", "[WARN]"

    print("=" * 78)
    print("移植度量与不变式体检  (docs/ZYNQ7020_PORT_CHARTER.md)")
    print("=" * 78)

    print("\n【M1】上游原文在 ARM 上运行的代码行 / 上游适用代码行")
    print(f"  源 OS 自有(不含 vendored FatFs): {m1['num_own']:>6} / {m1['den_own']:>6}"
          f"  = {100.0 * m1['num_own'] / max(1, m1['den_own']):5.1f}%   ← 宪章主指标")
    print(f"  计入 vendored FatFs:            {m1['num_all']:>6} / {m1['den_all']:>6}"
          f"  = {100.0 * m1['num_all'] / max(1, m1['den_all']):5.1f}%")
    print(f"  扣除 N-A(硬件不存在)后:        {m1['num_own']:>6} / {m1['den_own_na']:>6}"
          f"  = {100.0 * m1['num_own'] / max(1, m1['den_own_na']):5.1f}%  (参考)")
    print(f"  在跑的上游文件: {m1['files']} / {m1['files_total']}")

    print("\n【M2】移植侧自有代码 / 内核内发明物")
    a32 = sum(code_lines(p) for p in (ROOT / "arch" / "arm32").rglob("*")
              if p.suffix in (".c", ".cpp", ".h", ".S") and p.is_file())
    print(f"  arch/arm32 自有代码: {a32:>6} 行")
    print(f"  内核内发明物:        {i2_total:>6} 行  (宪章 §八 点名的 {len(i2_rows)} 个文件)")

    print("\n【I1】上游零修改" + ("  " + OK + " 0 行" if i1_total == 0 else f"  {BAD} {i1_total} 行 / {len(i1_rows)} 文件"))
    if args.verbose:
        for path, add, dele in i1_rows:
            print(f"      {path:<44} +{add:<4} -{dele}")

    print("\n【I2】不做源 OS 没有的东西"
          + ("  " + OK + " 无" if i2_total == 0 else f"  {BAD} {i2_total} 行（待撤销）"))
    if args.verbose:
        for rel, n, alt in i2_rows:
            print(f"      {rel:<44} {n:>5} 行  → {alt}")

    print("\n【I3】不新造 API  " + WARN + " 启发式（待人工确认）")
    for pref, (n, upstream, files) in sorted(i3.items()):
        print(f"      {pref + '*':<14} {n:>4} 处 → 上游应为 {upstream}")

    print("\n【I4】验证脚手架不进内核"
          + ("  " + OK + " 内核无判定" if i4_judges == 0 else f"  {BAD} 内核里有 {i4_judges} 个判定"))
    print(f"      内核里 selftest_* 引用 {i4_refs} 处")

    print("\n【I5】不隐藏缺口")
    print(f"      链接宽容 -z muldefs: {'在用（须列期望重复符号清单）' if muldefs else '未用'}")
    print(f"      落地层拒绝型桩: {len(stubs)} 个，其中写了『{UNLOCK_WORD}』的 {unlocked} 个"
          + ("  " + OK if stubs and unlocked == len(stubs) else "  " + BAD))
    if args.verbose and stubs:
        for name in stubs:
            mark = OK if name not in stub_missing else BAD
            print(f"        {mark} {name}")
    print(f"      上游排除表条目: {ex} 条（每条须有理由 + 解开条件，由宿主测试钉住）")

    bad = (i1_total > 0) + (i2_total > 0) + (i4_judges > 0) + (len(stubs) > unlocked)
    print("\n" + "-" * 78)
    print(f"待处理违例项: {bad} 类   "
          f"(I1={i1_total} 行 · I2={i2_total} 行 · I4={i4_judges} 个判定 · "
          f"I5={len(stubs) - unlocked} 个桩缺『{UNLOCK_WORD}』)")
    print("-" * 78)
    return 0


if __name__ == "__main__":
    sys.exit(main())
