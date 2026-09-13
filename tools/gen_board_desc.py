#!/usr/bin/env python3
"""从 xparameters.h 生成设备描述表（类 DTS 模型的落地形式）。

============================================================================
为什么要有这个生成器
============================================================================

描述模型（arch/arm32/include/arch/plat_device.h）照 DTS 的语义设计，
而落地形式是"从硬件描述生成 C 表"。这个脚本就是那个落地环节。

单一真值来源 = Vitis 从 XSA 导出的 xparameters.h。它本身就是一份**已经扁平化
的 DTS 表示**：每个 IP 实例一组 `#define XPAR_<实例>_<属性>`。
生成器把它翻成 `plat_device_t[]`，内核侧零运行期依赖（不需要 DTB 解析器，
也就不需要早期分配器）。

这与 Xilinx 自己的 `XLookupConfig` 是同一个哲学：
`xgpio_g.c` 里的 `XGpio_ConfigTable[]` 就是从同一份 xparameters.h 生成的。
我们只是换成本项目自己的模型。

============================================================================
三处必须小心的地方（都踩过或用实测值验证过）
============================================================================

1. **每个设备在 xparameters.h 里出现两次。**
   一次是外设名形式（`XPAR_AXI_GPIO_0_*`），一次是驱动规范形式
   （`XPAR_XGPIO_0_*`）。两者 compatible 与地址完全相同，属性也基本重复。
   不去重的话描述表里每个设备都会有两个节点，驱动会 probe 两次。

2. **中断号是编码过的，不是 INTID。**
   `XPAR_*_INTERRUPTS` 的格式（定义见 BSP 的 xinterrupt_wrap.h）：
       bits[11:0]  = 相对中断号
       bits[15:12] = 触发类型
       bit20       = 0 = SPI, 1 = PPI
       实际 GIC INTID = 相对号 + (SPI ? 32 : PPI ? 16 : 0)
   已用本板真实值逐个验算：
       SCUTIMER 0x13100d -> PPI, 13 + 16 = 29（Cortex-A9 私有定时器）✓
       SCUWDT   0x10400e -> PPI, 14 + 16 = 30（看门狗）✓
       QSPI     0x4013   -> SPI, 19 + 32 = 51 ✓
   差 32 的错会让驱动挂到一个完全无关的中断上，且编译期不暴露。

3. **触发类型字段在本输入里不可靠。**
   按 xinterrupt_wrap.h 的定义，1 = 电平、3 = 边沿。但本板 xparameters.h
   里实际读到的是 4 / 1 / 0 三种值，与定义对不上（多数是 4）。
   生成器会把这个不一致**报出来**而不是悄悄写进表里 ——
   驱动的中断触发类型由自己配置（gic_set_trigger），不依赖这个字段。

用法:
    python tools/gen_board_desc.py arch/arm32/board/xparameters.h \\
        --out-c arch/arm32/src/board_devices.c \\
        --out-h arch/arm32/include/arch/board_devices.h
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]

# 生成物在仓库里的规范位置。写进生成物的"重生成命令"用这两个常量而不是
# 命令行实参 —— 否则把生成物输出到临时目录(比如做"重新生成后是否一致"的
# 校验)时,产物内容会跟着变,那种校验就立不住了。
DEFAULT_OUT_C = "arch/arm32/src/board_devices.c"
DEFAULT_OUT_H = "arch/arm32/include/arch/board_devices.h"


def display_path(path: Path) -> str:
    """把路径显示成**相对仓库根**的形式。

    为什么必须做这一步:生成物里嵌了输入路径(便于知道文件是怎么来的)。
    如果直接写命令行传进来的路径,那么同一份输入用绝对路径和相对路径跑,
    生成物就会不同 —— "重新生成后与仓库一致"这种校验根本立不住,
    换个机器、换个检出目录都会 diff 出差异。
    """
    try:
        return path.resolve().relative_to(REPO_ROOT).as_posix()
    except ValueError:
        return path.as_posix()


# ---------------------------------------------------------------------------
# xparameters.h 解析
# ---------------------------------------------------------------------------

DEFINE_RE = re.compile(
    r"^#define\s+(?P<name>XPAR_\w+?)\s+(?P<value>.+?)\s*(?:/\*.*)?$"
)

# 驱动规范形式的名字: XPAR_X<DRIVER>_<INDEX>，例如 XPAR_XGPIO_0
CANONICAL_RE = re.compile(r"^XPAR_X\w+_\d+$")

# 这些后缀映射到模型的固定字段，不作为 props
FIELD_SUFFIXES = {
    "COMPATIBLE",
    "BASEADDR",
    "BASEADDRESS",
    "HIGHADDR",
    "HIGHADDRESS",
    "INTERRUPTS",
    "INTERRUPT_PARENT",
}

# XPAR_<实例>_INTERRUPTS_<N>：多中断设备的第 N 条中断线
EXTRA_IRQ_RE = re.compile(r"^INTERRUPTS_\d+$")

# PL 地址段。落在这一段里的节点属于 FPGA 侧，当前一律标记为未启用
PL_BASE_LO = 0x40000000
PL_BASE_HI = 0xBFFFFFFF


def parse_defines(text: str) -> dict[str, str]:
    """把 xparameters.h 里的 #define 读成 {名字: 原始值字符串}。"""
    out: dict[str, str] = {}
    for line in text.splitlines():
        line = line.strip()
        if not line.startswith("#define"):
            continue
        # 去掉行尾注释
        line = re.sub(r"/\*.*?\*/", "", line).strip()
        m = DEFINE_RE.match(line)
        if not m:
            continue
        out[m.group("name")] = m.group("value").strip().strip('"')
    return out


def parse_int(value: str) -> int:
    """xparameters.h 里的数值都是十六进制字面量，但也容忍十进制。"""
    value = value.strip()
    try:
        return int(value, 0)
    except ValueError:
        return 0


def decode_irq(encoded: int) -> tuple[int, int, str]:
    """把 xparameters.h 的 _INTERRUPTS 编码解成 (GIC INTID, 触发类型, 说明)。

    返回 (PLAT_IRQ_NONE 对应的 -1, 0, ...) 表示无中断。
    """
    if encoded == 0:
        return -1, 0, "none"

    rel = encoded & 0xFFF
    trigger = (encoded >> 12) & 0xF
    is_ppi = (encoded >> 20) & 1
    is_sgi = (encoded >> 22) & 1

    if is_sgi:
        # SGI 是软件产生的，不该出现在设备描述里
        return -1, 0, f"SGI? (rel={rel})"

    if is_ppi:
        return rel + 16, trigger, f"PPI rel={rel} +16"
    return rel + 32, trigger, f"SPI rel={rel} +32"


# ---------------------------------------------------------------------------
# 设备节点
# ---------------------------------------------------------------------------


class Node:
    def __init__(self, instance: str, compatible: str) -> None:
        self.instance = instance
        self.compatible = compatible
        self.base = 0
        self.high = 0
        self.irq = -1
        self.trigger = 0
        self.irq_note = "none"
        self.props: list[tuple[str, int]] = []
        self.has_interrupts = False
        self.extra_irqs: list[str] = []

    @property
    def name(self) -> str:
        """节点名。去掉 XPAR_ 前缀并转小写 —— 前缀是 xparameters.h 的命名
        约定,不是硬件事实,放在描述表里只是噪音。"""
        return self.instance[len("XPAR_") :].lower() if self.instance.startswith("XPAR_") else self.instance.lower()

    @property
    def is_pl(self) -> bool:
        """是否落在 PL(FPGA)地址段。"""
        return PL_BASE_LO <= self.base <= PL_BASE_HI

    @property
    def size(self) -> int:
        if self.base == 0 or self.high < self.base:
            return 0
        return self.high - self.base + 1

    @property
    def vendor(self) -> str:
        """compatible 的厂商前缀，用作属性名前缀。"""
        if "," in self.compatible:
            return self.compatible.split(",", 1)[0]
        return ""

    def prop_name(self, suffix: str) -> str:
        """把 XPAR 后缀翻成类 DTS 的属性名。

        `IS_DUAL` -> `xlnx,is-dual`

        这样出来的名字与 Xilinx 上游 DTS 一致 —— `xgpio_g.c` 里的注释
        明确写了 `0x1, /* xlnx,is-dual */`、`0x8, /* xlnx,gpio-width */`。
        """
        dashed = suffix.lower().replace("_", "-")
        return f"{self.vendor},{dashed}" if self.vendor else dashed


def collect_nodes(defines: dict[str, str]) -> tuple[list[Node], list[str]]:
    """扫出所有带 _COMPATIBLE 的实例，去掉规范形式的重复。

    返回 (节点列表, 警告列表)。
    """
    warnings: list[str] = []

    # 1. 找出所有 (实例名 -> compatible)
    instances: dict[str, str] = {}
    for name, value in defines.items():
        if not name.endswith("_COMPATIBLE"):
            continue
        instance = name[: -len("_COMPATIBLE")]
        instances[instance] = value

    # 2. 丢掉驱动规范形式（XPAR_X<DRIVER>_<N>）
    #
    #    这些是同一个硬件的第二份定义，compatible 与地址完全相同。
    #    保留它们的后果是每个设备在表里出现两次、驱动 probe 两次。
    canonical = [i for i in instances if CANONICAL_RE.match(i)]
    for instance in canonical:
        del instances[instance]

    # 3. 构造节点
    nodes: list[Node] = []
    seen: dict[tuple[str, int], str] = {}

    for instance in sorted(instances):
        node = Node(instance, instances[instance])
        prefix = instance + "_"

        for name, value in defines.items():
            if not name.startswith(prefix):
                continue
            suffix = name[len(prefix) :]
            if suffix in FIELD_SUFFIXES:
                continue

            # 多中断设备的第 N 条中断线。
            #
            # 模型里只有一个 irq 字段,装不下它们;把它们当成不透明的原始
            # 编码值塞进 props 更糟 —— 驱动读到的是个没有意义的数字。
            # 所以这里**丢掉并记下来**,由调用方在警告里看到。
            if EXTRA_IRQ_RE.match(suffix):
                node.extra_irqs.append(suffix)
                continue

            # INTERRUPT_PRESENT == 0 时不应出现中断号，但仍保留这个属性：
            # 驱动可能需要知道"这个 IP 支持中断但本设计没连"
            node.props.append((node.prop_name(suffix), parse_int(value)))

        node.base = parse_int(defines.get(prefix + "BASEADDR", defines.get(prefix + "BASEADDRESS", "0")))
        node.high = parse_int(defines.get(prefix + "HIGHADDR", defines.get(prefix + "HIGHADDRESS", "0")))

        raw_irq = defines.get(prefix + "INTERRUPTS")
        if raw_irq is not None:
            node.has_interrupts = True
            node.irq, node.trigger, node.irq_note = decode_irq(parse_int(raw_irq))

        # 4. 同 (compatible, base) 再去一次重 —— 兜住命名形式之外的重复
        key = (node.compatible, node.base)
        if key in seen:
            warnings.append(
                f"重复节点已跳过: {instance} 与 {seen[key]} 的 compatible+base 相同"
            )
            continue
        seen[key] = instance
        nodes.append(node)

    if canonical:
        warnings.append(
            f"已折叠 {len(canonical)} 个驱动规范形式(xpar_x*_<n>)的重复定义: "
            + ", ".join(sorted(canonical)[:4])
            + (" ..." if len(canonical) > 4 else "")
        )

    return nodes, warnings


# ---------------------------------------------------------------------------
# 输出
# ---------------------------------------------------------------------------

BANNER = """/*
 * 设备描述表 —— **本文件由 tools/gen_board_desc.py 生成,不要手改**
 *
 * 重新生成:
 *     python tools/gen_board_desc.py {input} \\
 *         --out-c {out_c} --out-h {out_h}
 *
 * 输入是 Vitis 从 XSA 导出的 xparameters.h —— 它本身就是一份已经扁平化的
 * DTS 表示。改硬件之后重新导出并重跑生成器,不要手改本文件。
 *
 * 模型定义与设计理由见 arch/arm32/include/arch/plat_device.h。
 */"""


def c_string(value: str) -> str:
    escaped = value.replace("\\", "\\\\").replace('"', '\\"')
    return f'"{escaped}"'


def sanitize(identifier: str) -> str:
    return re.sub(r"[^A-Za-z0-9_]", "_", identifier).lower()


def emit_header(nodes: list[Node], input_name: str) -> str:
    lines = [
        "#pragma once",
        "",
        BANNER.format(input=input_name, out_c=DEFAULT_OUT_C, out_h=DEFAULT_OUT_H),
        "",
        "#include <arch/plat_device.h>",
        "",
        "/* 全部设备节点,按 xparameters.h 的实例名排序 */",
        "/*",
        " * ⚠ 非 const:enabled 字段允许运行时修正。",
        " *",
        " * 生成器描述的是**设计里有什么**(xparameters.h 的来源),",
        " * 而\"比特流是否真的已加载\"是运行时事实 —— 两者不一定一致。",
        " * 所以 PL 节点默认 enabled = false,由 board.c 按实际加载的",
        " * 比特流在启动时修正。见 arch/arm32/src/board.c。",
        " */",
        f"extern plat_device_t g_board_devices[{len(nodes)}];",
        "extern const u32     g_board_device_count;",
        "",
        "/*",
        " * 驱动汇总入口 —— **指针数组**。",
        " *",
        " * 为什么不是结构体数组:C 不允许用结构体变量初始化结构体数组,",
        " * 而驱动必须能各自住在自己的翻译单元里。见 plat_device.h。",
        " *",
        " * 定义在 arch/arm32/src/board.c(手写),因为驱动是代码不是数据 ——",
        " * 生成器只该产出\"硬件是什么\",不该产出\"谁去驱动它\"。",
        " */",
        "extern const plat_driver_t *const g_board_drivers[];",
        "extern const u32                  g_board_driver_count;",
        "",
    ]
    return "\n".join(lines)


def emit_source(nodes: list[Node], input_name: str) -> str:
    lines: list[str] = [
        BANNER.format(input=input_name, out_c=DEFAULT_OUT_C, out_h=DEFAULT_OUT_H),
        "",
        "#include <arch/board_devices.h>",
        "",
    ]

    # --- props ---
    any_props = False
    for node in nodes:
        if not node.props:
            continue
        any_props = True
        lines.append(f"static const plat_prop_t {sanitize(node.instance)}_props[] = {{")
        for name, value in node.props:
            lines.append(f"    {{{c_string(name)}, {value}u}},")
        lines.append("};")
        lines.append("")

    if not any_props:
        lines.append("/* 本输入里没有任何实例带自定义属性 */")
        lines.append("")

    # --- devices ---
    lines.append(f"plat_device_t g_board_devices[{len(nodes)}] = {{")
    for node in nodes:
        lines.append("    {")
        lines.append(f"        .name       = {c_string(node.name)},")
        lines.append(f"        .compatible = {c_string(node.compatible)},")
        lines.append(f"        .reg_base   = 0x{node.base:08X}u,")
        lines.append(f"        .reg_size   = 0x{node.size:08X}u,")

        if node.irq >= 0:
            lines.append(f"        .irq        = {node.irq},  /* {node.irq_note} */")
            lines.append(f"        .irq_flags  = {node.trigger}u,")
        else:
            lines.append("        .irq        = PLAT_IRQ_NONE,")
            lines.append("        .irq_flags  = 0u,")

        # clocks 是符号名(对应 DTS 的 phandle + index),
        # 而 xparameters.h 里根本没有"时钟名"这个概念 —— 它只有频率数值,
        # 而那些频率已经作为属性(如 xlnx,clock-freq)进了 props。
        # 所以生成器填不了这个字段。
        lines.append("        .clocks     = NULL,  /* xparameters.h 无对应概念,见文件头 */")

        if node.props:
            lines.append(f"        .props      = {sanitize(node.instance)}_props,")
            lines.append(f"        .prop_count = {len(node.props)}u,")
        else:
            lines.append("        .props      = NULL,")
            lines.append("        .prop_count = 0u,")

        lines.append("        .parent     = NULL,")
        lines.append(f"        .bus        = PLAT_BUS_{'AXI' if node.is_pl else 'APB'},")
        # PL 节点默认未启用:xparameters.h 只说明"设计里有这个 IP",
        # 不说明"比特流已加载"。是否真的在,由 board.c 在启动时按实际加载的
        # 比特流修正 —— 生成器不该替运行时的决定做假设。
        lines.append(f"        .enabled    = {'false' if node.is_pl else 'true'},")
        lines.append("    },")
    lines.append("};")
    lines.append("")
    lines.append(f"const u32 g_board_device_count = {len(nodes)}u;")
    lines.append("")

    return "\n".join(lines)


# ---------------------------------------------------------------------------


def write_text_lf(path: Path, text: str) -> None:
    """写出文件,**强制 LF 行尾**。

    不能用 Path.write_text():它在 Windows 上会把 \\n 翻成 \\r\\n,
    于是同一份输入在 Windows 与 Linux 上生成出不同字节的产物 ——
    与"输出路径不影响内容"是同一类问题,都会让"重新生成后无差异"
    这种校验变得不可靠。
    """
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("xparameters", type=Path, help="Vitis 导出的 xparameters.h")
    parser.add_argument("--out-c", type=Path, required=True)
    parser.add_argument("--out-h", type=Path, required=True)
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()

    if not args.xparameters.exists():
        print(f"错误: 找不到输入 {args.xparameters}", file=sys.stderr)
        return 1

    defines = parse_defines(args.xparameters.read_text(encoding="utf-8", errors="replace"))
    nodes, warnings = collect_nodes(defines)

    if not nodes:
        print("错误: 输入里没有解析出任何设备节点", file=sys.stderr)
        return 1

    rel_in = display_path(args.xparameters)

    write_text_lf(args.out_c, emit_source(nodes, rel_in))
    write_text_lf(args.out_h, emit_header(nodes, rel_in))

    if not args.quiet:
        print(f"已生成 {len(nodes)} 个设备节点 -> {args.out_c}")
        print(f"{'':>12}{'实例':<24}{'compatible':<28}{'base':<12}{'INTID'}")
        for node in nodes:
            irq = str(node.irq) if node.irq >= 0 else "-"
            print(f"{'':>12}{node.instance:<24}{node.compatible:<28}0x{node.base:08X}  {irq:<6}({node.irq_note})")

        # 触发类型与文档定义不符时**报出来**，而不是悄悄写进表里
        odd = [n for n in nodes if n.has_interrupts and n.trigger not in (0, 1, 3)]
        if odd:
            print("\n警告: 下列节点的触发类型字段与 xinterrupt_wrap.h 的定义对不上")
            print("      (文档: 1 = 电平, 3 = 边沿)。这些值不可靠,驱动的触发类型应自行配置。")
            for node in odd:
                print(f"        {node.name}: trigger={node.trigger}")

        # 多中断设备:模型里只有一个 irq 字段
        multi = [n for n in nodes if n.extra_irqs]
        if multi:
            print("\n警告: 下列节点有多条中断线,而 plat_device_t 只有一个 irq 字段。")
            print("      额外的中断线**没有**进描述表(不能把编码值当数据塞进 props)。")
            print("      等真有驱动需要它们时再扩模型,见 plat_device.h。")
            for node in multi:
                print(f"        {node.name}: {1 + len(node.extra_irqs)} 条中断线")

        if warnings:
            print()
            for warning in warnings:
                print(f"注意: {warning}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
