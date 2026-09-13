#!/usr/bin/env python3
"""本机相关的路径与端口 —— **换一台机器只需要改这个文件**。

============================================================================
怎么用(三步)
============================================================================

1. 改下面那块 "★ 要改的就是这一段 ★";

2. 自检(会逐项告诉你哪个路径不存在,以及该改成什么):

       python config.py

   它还会顺手生成 `tmp-test/jtag/paths.tcl` —— JTAG 的 Tcl 脚本要从那里取路径
   (Tcl 没法 import Python,所以由本文件生成一个小文件给它 source);

3. 之后照 `docs/BUILD_ARM32.md` 走,别的文件都不用动。

============================================================================
不想让自己的路径进 git?
============================================================================

本文件是**要提交**的,所以里面只放"通用/占位"的值。如果你不想让自己的机器路径
出现在版本库里,就别改这里,而是:

    cp config.example.py config.local.py     # 本文件末尾会自动读它
    # 然后只改 config.local.py

`config.local.py` 已被 .gitignore 忽略,不会被提交。两种做法都行。
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

# ============================================================================
# ★ 要改的就是这一段 ★
# ============================================================================

#: Vitis / Vivado 的安装目录。里面应当有 `Vitis/bin/xsdb.bat` 和
#: `gnu/aarch32/nt/gcc-arm-none-eabi/bin/arm-none-eabi-gcc.exe`。
#: 常见位置:
#:     C:\AMDDesignTools\2025.2          AMD 统一安装器的默认位置
#:     C:\Xilinx\Vitis\2025.2            Xilinx 安装器的默认位置
VITIS_DIR = r"C:\AMDDesignTools\2025.2"

#: 板子上的 PS UART1 接到 PC 的哪个串口(Windows 设备管理器里看)。
SERIAL_PORT = "COM4"

#: 串口波特率。板级自检/命令通道都是 9600 8N1,一般不用改。
SERIAL_BAUD = 9600

#: PL 比特流。**这个必须换成你自己的**:它在 Vitis 工程导出目录里,形状通常是
#:     <平台>\hw\sdt\System_wrapper.bit
BITSTREAM = r"C:\path\to\your\platform\hw\sdt\System_wrapper.bit"

#: 原理图 PDF(只有 `tmp-test/sch_render.py` 用)。留空表示不用。
SCHEMATIC_PDF = ""

# ============================================================================
# 本地覆盖(可选)—— 必须放在**派生之前**,否则从 BITSTREAM 派生的值会用到旧值
# ============================================================================
#: 仓库根目录(本文件所在目录)。
ROOT = Path(__file__).resolve().parent

# 如果存在 config.local.py,就用它覆盖上面那段的值 —— 这样你的机器路径不必进 git。
_LOCAL = ROOT / "config.local.py"
if _LOCAL.is_file():
    exec(compile(_LOCAL.read_text(encoding="utf-8"), str(_LOCAL), "exec"))  # noqa: S102

# ============================================================================
# 以下**一般不用改**:要么是从上面派生的,要么是跟着仓库走的
# ============================================================================

#: JTAG 的 Tcl 脚本目录。
JTAG_DIR = ROOT / "tmp-test" / "jtag"
#: 给 Tcl 用的路径文件(由本文件生成,不进版本库)。
PATHS_TCL = JTAG_DIR / "paths.tcl"

# --- 从 VITIS_DIR 派生 ------------------------------------------------------
_VITIS = Path(VITIS_DIR)
#: 下载/调试用的 xsdb(bat 在 Windows 上,无扩展名的在 Linux 上)。
XSDB = _VITIS / "Vitis" / "bin" / ("xsdb.bat" if os.name == "nt" else "xsdb")
#: hw_server(只有少数诊断脚本用得到)。
HW_SERVER = _VITIS / "Vitis" / "bin" / ("hw_server.bat" if os.name == "nt" else "hw_server")
#: ARM 交叉工具链根目录。
ARM_TOOLCHAIN_DIR = _VITIS / "gnu" / "aarch32" / "nt" / "gcc-arm-none-eabi"

_EXE = ".exe" if os.name == "nt" else ""
#: 编译器。**注意文件名是 arm-none-eabi-gcc**(`--version` 自报
#: `arm-xilinx-eabi-gcc`,在 bin/ 里找那个名字是找不到的)。
ARM_CC = ARM_TOOLCHAIN_DIR / "bin" / f"arm-none-eabi-gcc{_EXE}"
ARM_OBJCOPY = ARM_TOOLCHAIN_DIR / "bin" / f"arm-none-eabi-objcopy{_EXE}"
ARM_NM = ARM_TOOLCHAIN_DIR / "bin" / f"arm-none-eabi-nm{_EXE}"
ARM_OBJDUMP = ARM_TOOLCHAIN_DIR / "bin" / f"arm-none-eabi-objdump{_EXE}"

# --- 仓库内相对路径(跟着仓库走,换机器不用改)--------------------------------
#: 内核产物。
KERNEL_ELF = ROOT / "out" / "kernel-arm.elf"
#: PL LED 演示程序的产物。
LED_ELF = ROOT / "tmp-test" / "led" / "out" / "led.elf"
#: ★ 必须用**这一份**:平台自带的 ps7_init.tcl **不把 MIO48/49 配成 UART1**,
#: 用了它内核照常启动但串口一个字节都没有(看起来像"内核没跑起来")。
PS7_INIT = ROOT / "tmp-test" / "zynq" / "ps7_init_uart1.tcl"
#: 平台自带的那一份(不路由 UART1)。只有非串口的老脚本用得到它;它在比特流同目录。
PS7_PLATFORM = Path(BITSTREAM).with_name("ps7_init.tcl")
#: MIO bank1 电平自检。
PS7_MIO_CHECK = ROOT / "tmp-test" / "zynq" / "ps7_mio_bank1_check.tcl"

# ============================================================================
# 工具函数
# ============================================================================

#: 自检时逐项检查"存在性"的值(name -> 路径)。
REQUIRED_PATHS = {
    "jtag.xsdb": XSDB,
    "toolchain.arm_gcc": ARM_CC,
    "toolchain.arm_objcopy": ARM_OBJCOPY,
}

#: `as_dict()` 里哪些键是**路径**(要判存在性)。串口号/波特率不在其中。
PATH_KEYS = frozenset(
    {
        "vitis.dir",
        "jtag.xsdb",
        "toolchain.dir",
        "toolchain.arm_gcc",
        "board.bitstream",
        "repo.ps7_init",
        "repo.kernel_elf",
        "repo.led_elf",
        "tools.schematic_pdf",
    }
)


def check() -> list[str]:
    """返回"有问题"的清单(空列表 = 全绿)。"""
    problems: list[str] = []
    for label, path in REQUIRED_PATHS.items():
        if not Path(path).exists():
            problems.append(f"{label} 不存在: {path}")
    if not BITSTREAM or "path/to/your" in BITSTREAM:
        problems.append(f"board.bitstream 还是占位值,必须改成你的比特流: {BITSTREAM}")
    elif not Path(BITSTREAM).exists():
        problems.append(f"board.bitstream 不存在: {BITSTREAM}")
    for label, path in (("ps7_init", PS7_INIT), ("ps7_mio_check", PS7_MIO_CHECK)):
        if not Path(path).exists():
            problems.append(f"{label} 不存在: {path}")
    if not SERIAL_PORT:
        problems.append("board.serial_port 是空的 —— 它因机器而异,没有默认值")
    return problems


def write_paths_tcl() -> Path:
    """把 Tcl 需要的路径写进 `tmp-test/jtag/paths.tcl`。

    Tcl 没法 `import config`,所以由 Python 侧生成一个小文件让它 `source`。

    两点讲究:
      * 值用 `{}` 包起来 —— Tcl 的花括号里不做变量/反斜杠替换,正好适合
        Windows 路径(`C:/...` 里那几个反斜杠不会被吃掉);
      * 整个文件**保持纯 ASCII**(注释也用英文)—— tclsh 在 Windows 上按
        **系统代码页**读脚本,文件里混 UTF-8 中文会有被拆错的隐患。
    """
    PATHS_TCL.parent.mkdir(parents=True, exist_ok=True)
    PATHS_TCL.write_text(
        "# Generated by <repo>/config.py -- DO NOT EDIT.\n"
        "# Refresh with:  python config.py\n"
        "# Values are braced so Tcl does no substitution (safe for Windows paths).\n"
        f"set BIT {{{Path(BITSTREAM).as_posix()}}}\n"
        f"set PS7 {{{PS7_INIT.as_posix()}}}\n"
        f"set PS7_PLATFORM {{{PS7_PLATFORM.as_posix()}}}\n"
        f"set MIOCHK {{{PS7_MIO_CHECK.as_posix()}}}\n"
        f"set ELF {{{KERNEL_ELF.as_posix()}}}\n"
        f"set LED_ELF {{{LED_ELF.as_posix()}}}\n",
        encoding="ascii",
    )
    return PATHS_TCL


def as_dict() -> dict[str, str]:
    """给 `python config.py` 打印用。"""
    return {
        "vitis.dir": VITIS_DIR,
        "jtag.xsdb": str(XSDB),
        "toolchain.dir": str(ARM_TOOLCHAIN_DIR),
        "toolchain.arm_gcc": str(ARM_CC),
        "board.serial_port": SERIAL_PORT,
        "board.baud": str(SERIAL_BAUD),
        "board.bitstream": BITSTREAM,
        "repo.ps7_init": str(PS7_INIT),
        "repo.kernel_elf": str(KERNEL_ELF),
        "repo.led_elf": str(LED_ELF),
        "tools.schematic_pdf": SCHEMATIC_PDF or "(未设置)",
    }


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001 - 老解释器没有 reconfigure
        pass

    args = sys.argv[1:]

    # `python config.py --get NAME` —— 给不方便 import 的地方用(例如 build.ps1)
    if args and args[0] == "--get":
        if len(args) < 2:
            print("用法: python config.py --get <名字>", file=sys.stderr)
            return 2
        table = as_dict()
        key = args[1]
        if key not in table:
            print(f"没有这一项: {key}\n可选: {', '.join(table)}", file=sys.stderr)
            return 2
        print(table[key])
        return 0

    print(f"仓库根目录 : {ROOT}")
    print(f"本地覆盖   : {'有 config.local.py' if _LOCAL.is_file() else '无'}")
    print()
    table = as_dict()
    width = max(len(k) for k in table)
    for key, value in table.items():
        if key not in PATH_KEYS:
            mark = ""            # 不是路径(串口号/波特率),不判存在性
        elif not value or value == "(未设置)" or "path/to/your" in value:
            mark = "" if value == "(未设置)" else "  [★ 占位值,必须改 ★]"
        elif Path(value).exists():
            mark = "  [存在]"
        else:
            mark = "  [✗ 不存在]"
        print(f"  {key:<{width}}  {value}{mark}")

    problems = check()
    print()
    if problems:
        print(f"✗ 有 {len(problems)} 项要先解决:")
        for p in problems:
            print(f"    - {p}")
        print("\n  ⇒ 改本文件顶部那段(或建 config.local.py 覆盖),然后重跑 python config.py")
        # 自检没过时**不刷新** paths.tcl —— 但要提醒它可能是过期的:
        # 一个过期的路径表会安静地加载错比特流,比报错难查得多。
        if PATHS_TCL.is_file():
            print(f"\n  ⚠ 注意:{PATHS_TCL.relative_to(ROOT)} 还是上一次生成的,里面的值")
            print("     可能与你刚改的 config.py 不一致。修好上面几项后重跑本命令刷新它。")
        return 1

    written = write_paths_tcl()
    print(f"✓ 全部就绪;已生成 {written.relative_to(ROOT)}")
    print("  下一步:按 docs/BUILD_ARM32.md 构建并上板")
    return 0


if __name__ == "__main__":
    sys.exit(main())
