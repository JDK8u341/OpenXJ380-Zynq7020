#!/usr/bin/env python3
"""Generate the Python-backed Ninja graph for XJ380.

This file is intentionally more explicit than a normal packaged build project:
XJ380 is freestanding, mixes kernel/user/kmod/Rust outputs, and needs
stable staged artifact names under out/.  Keep build behavior in this generator
instead of hand-editing build.ninja, because build.ninja is regenerated.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build.ninja"

# 本机路径(工具链装在哪)集中在仓库根目录的 config.py —— 换机器只改那一个文件。
# 仓库根不在 sys.path 里(本文件在 tools/ 下),所以先把根插进去。
sys.path.insert(0, str(ROOT))
import config  # noqa: E402

# Keep generated Ninja output easy to scan with colored progress labels.
LOG_RESET = "\033[0m"
LOG_COLORS = {
    "ASM": "\033[1;35m",
    "BOOT": "\033[1;35m",
    "CC": "\033[1;32m",
    "CHECK": "\033[1;34m",
    "CLEAN": "\033[1;31m",
    "CP": "\033[1;34m",
    "CXX": "\033[1;36m",
    "FETCH": "\033[1;34m",
    "FIND": "\033[1;34m",
    "FMT": "\033[1;34m",
    "GEN": "\033[1;35m",
    "GRAPH": "\033[1;34m",
    "HOSTLD": "\033[1;33m",
    "ISO": "\033[1;35m",
    "LD": "\033[1;33m",
    "OBJCOPY": "\033[1;35m",
    "PREP": "\033[1;34m",
    "RUN": "\033[1;32m",
    "RUST": "\033[1;32m",
    "SIZE": "\033[1;34m",
    "STAGE": "\033[1;34m",
    "VDISK": "\033[1;35m",
    "VMDK": "\033[1;35m",
}


def log_label(tag: str) -> str:
    """Return the colored `[TAG]` prefix used in Ninja descriptions."""
    return f"{LOG_COLORS[tag]}[{tag}]{LOG_RESET}"


def log_desc(tag: str, text: str = "") -> str:
    """Keep generated Ninja descriptions short but visually grouped."""
    return log_label(tag) if not text else f"{log_label(tag)} {text}"



def rel(path: Path | str) -> str:
    """Return a repository-relative path when the path lives under ROOT.

    Generated build files should be portable between Windows paths, WSL paths,
    and checked-out copies, so normal project inputs are emitted relative to the
    repository.  Absolute paths are kept only for external toolchain artifacts
    such as Rust sysroot libraries.
    """
    p = Path(path)
    if p.is_absolute():
        try:
            p = p.relative_to(ROOT)
        except ValueError:
            return p.as_posix()
    return p.as_posix()


def esc(path: str) -> str:
    """Escape characters that Ninja treats as syntax inside paths.

    Ninja uses `:` after output lists and `$` for continuations/variables, so
    Windows drive letters, spaces, and literal dollars must be escaped before a
    path can appear in a build edge.
    """
    return path.replace("$", "$$").replace(":", "$:").replace(" ", "$ ")


def paths(items: list[Path | str]) -> str:
    """Format a path list for a Ninja build edge.

    Callers pass raw Path/string objects; this helper centralizes the relpath
    conversion and Ninja escaping so every edge follows the same output style.
    """
    return " ".join(esc(rel(item)) for item in items)


def cmd_output(command: list[str]) -> str:
    """Run a small discovery command; missing tools produce an empty value.

    The generator should still be inspectable when optional host tools are not
    installed.  Hard requirements, such as the Rust target libraries, validate
    their own result after calling this helper.
    """
    try:
        return subprocess.check_output(command, cwd=str(ROOT), text=True, stderr=subprocess.DEVNULL).strip()
    except (FileNotFoundError, subprocess.CalledProcessError):
        return ""


def find_executable(names: list[str], extra_paths: list[Path] | None = None) -> str:
    """Find a tool while honoring common non-PATH locations used on WSL/Linux.

    Environment variables still win before this function is called.  The extra
    paths cover common Rust installations where `rustc` exists but the shell
    PATH inside the build environment has not been fully initialized.
    """
    extra_paths = extra_paths or []
    for name in names:
        found = shutil.which(name)
        if found:
            return found
        for base in extra_paths:
            candidate = base / name
            if candidate.exists() and os.access(candidate, os.X_OK):
                return str(candidate)
    return names[0] if names else ""


def find_rustc() -> str:
    """Resolve the Rust compiler used by no_std userland Rust apps."""
    override = os.environ.get("RUSTC")
    if override:
        return override
    return find_executable(
        ["rustc"],
        [
            Path.home() / ".cargo/bin",
            Path("/snap/bin"),
            Path("/home/leon/.rustup/toolchains/stable-x86_64-unknown-linux-gnu/bin"),
        ],
    )


def rust_target_libs(rustc: str, target: str) -> list[Path]:
    """Resolve libcore/compiler_builtins for no_std Rust user apps at gen time."""
    libdir = os.environ.get("RUST_TARGET_LIBDIR", "")
    if not libdir:
        if not rustc:
            raise SystemExit("缺少 Rust 编译器：请安装 rustup/rustc，或设置 RUSTC=/path/to/rustc")
        libdir = cmd_output([rustc, "--print", "target-libdir", "--target", target])
        if not libdir:
            raise SystemExit(
                f"无法查询 Rust 目标库目录：{target}\n"
                f"请先运行：rustup target add {target}\n"
                "如果已经安装，请确认 RUSTC 指向同一个 rustup 工具链，然后运行：ninja reconfigure"
            )
    libs: list[Path] = []
    missing: list[str] = []
    for pattern in ("libcore-*.rlib", "libcompiler_builtins-*.rlib"):
        matches = sorted(Path(libdir).glob(pattern))
        if matches:
            libs.append(matches[-1])
        else:
            missing.append(pattern)
    if missing:
        raise SystemExit(
            f"Rust 目标库不完整：{target}\n"
            f"缺少：{', '.join(missing)}\n"
            f"目标库目录：{libdir}\n"
            f"请运行：rustup target add {target}\n"
            "然后运行：ninja reconfigure"
        )
    return libs


def opt_flag() -> str:
    """Translate OPT_LEVEL into the C/C++ optimization flag family."""
    return "-O" + os.environ.get("OPT_LEVEL", "2")


def rust_opt_flag() -> str:
    """Translate OPT_LEVEL into the rustc optimization flag family."""
    return "-C opt-level=" + os.environ.get("OPT_LEVEL", "2")


def c_diagnostic_color_flag() -> str:
    """Keep compiler color enabled unless the environment overrides it."""
    return os.environ.get("DIAGNOSTICS_COLOR") or "-fdiagnostics-color=always"


def rust_diagnostic_color_flag() -> str:
    """Keep rustc color enabled unless the environment overrides it."""
    return os.environ.get("RUST_DIAGNOSTICS_COLOR") or "--color always"


def find_files(base: str, suffixes: tuple[str, ...], *, max_depth: int | None = None) -> list[Path]:
    """Collect files with deterministic sorted output.

    The optional depth cap is used for vendored libraries where only the first
    layer belongs to the existing build contract.  Sorting keeps regenerated
    build.ninja diffs stable when filesystems enumerate entries differently.
    """
    root = ROOT / base
    if not root.exists():
        return []
    out: list[Path] = []
    for path in root.rglob("*"):
        if not path.is_file() or path.suffix not in suffixes:
            continue
        if max_depth is not None:
            try:
                depth = len(path.relative_to(root).parts)
            except ValueError:
                continue
            if depth > max_depth:
                continue
        out.append(path)
    return sorted(out)


def headers(*bases: str) -> list[Path]:
    """Collect headers for implicit dependencies on generated object edges.

    These broad lists make initial dependency tracking conservative before
    compiler depfiles take over for subsequent builds.
    """
    out: list[Path] = []
    for base in bases:
        out.extend(find_files(base, (".h", ".hpp")))
    return sorted(out)


# Tiny writer for the subset of Ninja syntax this repository needs.
# It deliberately keeps commands as shell strings because the OS build still has
# many freestanding/linker steps that are clearer as explicit command lines.
class Ninja:
    def __init__(self) -> None:
        self.lines: list[str] = []
        self.default_order_only: list[Path | str] = []

    def line(self, text: str = "") -> None:
        self.lines.append(text)

    def comment(self, text: str) -> None:
        self.line(f"# {text}")

    def var(self, name: str, value: str) -> None:
        self.line(f"{name} = {value}")

    def var_list(self, name: str, values: list[Path | str]) -> None:
        """Emit long source lists over multiple Ninja lines."""
        escaped_values = [esc(rel(value)) for value in values]
        if not escaped_values:
            self.line(f"{name} =")
            return
        if len(escaped_values) == 1:
            self.var(name, escaped_values[0])
            return
        self.line(f"{name} = {escaped_values[0]} $")
        for value in escaped_values[1:-1]:
            self.line(f"  {value} $")
        self.line(f"  {escaped_values[-1]}")

    def rule(
        self,
        name: str,
        command: str,
        description: str | None = None,
        *,
        depfile: str | None = None,
        generator: bool = False,
        restat: bool = False,
    ) -> None:
        """Emit one Ninja rule and optional GCC-style depfile metadata."""
        self.line(f"rule {name}")
        self.line(f"  command = {command}")
        if description:
            self.line(f"  description = {description}")
        if depfile:
            self.line(f"  depfile = {depfile}")
            self.line("  deps = gcc")
        if generator:
            self.line("  generator = 1")
        if restat:
            self.line("  restat = 1")
        self.line()

    def build(
        self,
        outputs: list[Path | str] | Path | str,
        rule: str,
        inputs: list[Path | str] | Path | str | None = None,
        *,
        implicit: list[Path | str] | None = None,
        order_only: list[Path | str] | None = None,
        variables: dict[str, str] | None = None,
        use_default_order_only: bool = True,
    ) -> None:
        """Emit one Ninja build edge.

        `implicit` inputs appear after `|` and affect rebuild decisions without
        being passed to the command.  `order_only` inputs appear after `||` and
        enforce sequencing without making the output dirty when they change.
        Single values and lists are both accepted so call sites stay readable.
        """
        outs = [outputs] if isinstance(outputs, (str, Path)) else outputs
        ins: list[Path | str] = []
        if inputs is not None:
            ins = [inputs] if isinstance(inputs, (str, Path)) else inputs
        line = f"build {paths(list(outs))}: {rule}"
        if ins:
            line += f" {paths(ins)}"
        if implicit:
            line += f" | {paths(implicit)}"
        effective_order_only: list[Path | str] = []
        if use_default_order_only:
            effective_order_only.extend(self.default_order_only)
        if order_only:
            effective_order_only.extend(order_only)
        if effective_order_only:
            line += f" || {paths(effective_order_only)}"
        self.line(line)
        if variables:
            for key, value in variables.items():
                self.line(f"  {key} = {value}")
        self.line()


def root_objects(
    n: Ninja,
    nasm_files: list[Path],
    asm_files: list[Path],
    c_files: list[Path],
    cpp_files: list[Path],
) -> list[Path]:
    """Emit kernel/root object build edges and return their object paths."""
    # Root objects use the same source lists emitted in the generated FILES
    # section, so the readable list and the real build graph cannot drift.
    n.comment("ROOT OBJECTS - kernel, built-in drivers, console font, lib, optional built-in xhci")
    config = Path("kernel/build_config.h")
    settings = Path("kernel/build_settings.h")
    objs: list[Path] = []
    for src in nasm_files:
        obj = Path("out") / src.relative_to(ROOT).with_suffix(".o")
        n.build(obj, "nasm", src, implicit=[settings, config])
        objs.append(obj)
    for src in asm_files:
        obj = Path("out") / src.relative_to(ROOT).with_suffix(".o")
        n.build(obj, "root_as", src, implicit=[settings, config])
        objs.append(obj)
    for src in c_files:
        obj = Path("out") / src.relative_to(ROOT).with_suffix(".o")
        n.build(obj, "root_cc", src, implicit=[settings, config])
        objs.append(obj)
    for src in cpp_files:
        obj = Path("out") / src.relative_to(ROOT).with_suffix(".o")
        n.build(obj, "root_cxx", src, implicit=[settings, config])
        objs.append(obj)
    return objs


def xapi(n: Ninja) -> tuple[list[Path], Path, Path]:
    """Emit the minimal syscall and process-entry runtime."""
    n.comment("XAPI RUNTIME - declarations remain public; only syscall and entry shims are built")
    xapi_headers = headers("user/xapi/include")
    srcs = [
        ROOT / "user/xapi/arch/x86_64/crt0.S",
        ROOT / "user/xapi/libsys.cpp",
        ROOT / "user/xapi/xgui_stubs.cpp",
    ]
    core_objs: list[Path] = []
    for src in srcs:
        rel_src = src.relative_to(ROOT / "user/xapi")
        obj = Path("out/xapi") / Path(str(rel_src) + ".o")
        rule = "xapi_as" if src.suffix == ".S" else "xapi_cxx"
        n.build(obj, rule, src, implicit=xapi_headers)
        core_objs.append(obj)
    constart_obj = Path("out/xapi/constart.cpp.o")
    n.build(constart_obj, "xapi_headcon", "user/xapi/constart.cpp", implicit=xapi_headers)
    liballoc = Path("out/xapi/liballoc-x86_64.a")
    n.build(liballoc, "copy", "liballoc-x86_64.a")
    return core_objs, constart_obj, liballoc


def user_compile(n: Ninja, out: Path, src: str, flags: str, implicit: list[Path]) -> Path:
    # Most user apps compile one or more C++ objects and then link manually
    # against XAPI objects; using a custom cflags variable keeps rules reusable.
    n.build(out, "user_cxx_custom", src, implicit=implicit, variables={"cflags": flags})
    return out


def user_apps(n: Ninja, core_objs: list[Path], constart_obj: Path) -> list[Path]:
    """Emit the sole user-space example program."""
    n.comment("USER APPS - command-line example")
    shell_obj = Path("out/cli_shell.o")
    user_compile(n, shell_obj, "user/cli_shell.cpp", "$user_cflags", headers("user/xapi/include"))
    target = Path("out/shell.elf")
    n.build(target, "user_ld", core_objs + [constart_obj, shell_obj], variables={"message": log_desc("LD", target.as_posix())})
    return [target]


def kmods(n: Ninja) -> list[Path]:
    """Emit loadable kernel module artifacts."""
    # Kmods are shared objects with dlmain entrypoints.  Keep module compile
    # flags separate from root kernel flags because visibility/PIC differ.
    n.comment("KERNEL MODULES - loadable .sys outputs with dlmain entrypoints")
    targets: list[Path] = []
    # e1000 is first-party C++, so a shallow directory scan is enough and keeps
    # nested helper experiments from silently becoming part of the module ABI.
    n.comment("KMOD: e1000 PCI network driver")
    e1000_objs: list[Path] = []
    for src in find_files("kmod/e1000", (".cpp",), max_depth=1):
        obj = Path("out/kmod/e1000") / src.name.replace(".cpp", ".o")
        n.build(obj, "kmod_e1000_cxx", src)
        e1000_objs.append(obj)
    n.build("out/e1000.sys", "kmod_link", e1000_objs, variables={"ldflags": "-nostdlib -Wl,-e,dlmain"})
    targets.append(Path("out/e1000.sys"))

    # Netserver vendors lwIP, but this OS glue supports only the subset below.
    # Avoid enabling extra lwIP files accidentally because some depend on PPP or
    # platform hooks that are intentionally not wired in this tree.
    n.comment("KMOD: netserver lwIP subset and XJ380 arch glue")
    lwip_c = [
        "lwip/core/def.c",
        "lwip/core/dns.c",
        "lwip/core/init.c",
        "lwip/core/inet_chksum.c",
        "lwip/core/ip.c",
        "lwip/core/mem.c",
        "lwip/core/memp.c",
        "lwip/core/netif.c",
        "lwip/core/pbuf.c",
        "lwip/core/stats.c",
        "lwip/core/sys.c",
        "lwip/core/tcp.c",
        "lwip/core/tcp_in.c",
        "lwip/core/tcp_out.c",
        "lwip/core/udp.c",
        "lwip/core/timeouts.c",
        "lwip/core/ipv4/dhcp.c",
        "lwip/core/ipv4/etharp.c",
        "lwip/core/ipv4/icmp.c",
        "lwip/core/ipv4/ip4.c",
        "lwip/core/ipv4/ip4_addr.c",
        "lwip/netif/ethernet.c",
        "lwip/api/api_lib.c",
        "lwip/api/api_msg.c",
        "lwip/api/err.c",
        "lwip/api/netbuf.c",
        "lwip/api/netifapi.c",
        "lwip/api/tcpip.c",
    ]
    net_objs: list[Path] = []
    for item in lwip_c:
        src = Path("kmod/netserver") / item
        obj = Path("out/kmod/netserver") / item.replace(".c", ".o")
        n.build(obj, "netserver_cc", src)
        net_objs.append(obj)
    for item in ("netserver.cpp", "arch/sys_arch.cpp"):
        src = Path("kmod/netserver") / item
        obj = Path("out/kmod/netserver") / item.replace(".cpp", ".o")
        n.build(obj, "netserver_cxx", src)
        net_objs.append(obj)
    n.build("out/netserver.sys", "kmod_link", net_objs, variables={"ldflags": "-nostdlib -Wl,-e,dlmain -Wl,-z,muldefs"})
    targets.append(Path("out/netserver.sys"))

    # xhci can be linked into the kernel when BUILTIN_XHCI=1, but the module
    # artifact is still generated for staging/testing parity.
    n.comment("KMOD: xhci USB stack module artifact")
    xhci_objs: list[Path] = []
    for name in ("usb_core.cpp", "usb_hub.cpp", "usb_msc.cpp", "xhci.cpp"):
        src = Path("kmod/xhci") / name
        obj = Path("out/kmod_module/xhci") / name.replace(".cpp", ".o")
        n.build(obj, "kmod_xhci_cxx", src)
        xhci_objs.append(obj)
    n.build("out/xhci.sys", "kmod_link", xhci_objs, variables={"ldflags": "-nostdlib -Wl,-e,dlmain -Wl,-Bsymbolic -Wl,-z,muldefs"})
    targets.append(Path("out/xhci.sys"))
    return targets


def phony(n: Ninja, name: str, deps: list[Path | str]) -> None:
    """Emit compatibility targets such as `all`, `kmods`, and `build_xapi`."""
    n.build(name, "phony", deps)


def write_if_changed(path: Path, content: str) -> bool:
    """Write generated files only when bytes actually change.

    build.ninja is intentionally checked before every normal Ninja run.  If the
    generator rewrote identical content each time, Ninja would treat its own
    manifest as changed and restart repeatedly.
    """
    if path.exists() and path.read_text(encoding="utf-8") == content:
        return False
    path.write_text(content, encoding="utf-8")
    return True


# ---------------------------------------------------------------------------
# ARM32 (Zynq-7020 / Cortex-A9) build graph
#
# Kept separate from the x86_64 graph on purpose: the ARM port replaces the
# boot path, the arch layer and the platform drivers wholesale, so the two
# graphs share no compile rules.  Only the generator, the staging helper and
# the phony target names are common.
# ---------------------------------------------------------------------------

ARM32_SOURCE_ROOT = "arch/arm32"
ARM32_OBJ_ROOT = "out/arm32"
ARM32_KERNEL = "out/kernel-arm.elf"

# Zynq-7020 = dual Cortex-A9 / ARMv7-A / VFPv3.
# hard-float is deliberate: it matches AMD's own standalone BSP
# (cortexa9_toolchain.cmake uses -mfpu=vfpv3 -mfloat-abi=hard), so vendored
# Xilinx sources and any prebuilt BSP archive stay ABI-compatible.
ARM32_ARCH_FLAGS = (
    "-mcpu=cortex-a9",
    "-marm",
    "-mfpu=vfpv3",
    "-mfloat-abi=hard",
    "-mno-unaligned-access",
)

ARM32_COMMON_FLAGS = (
    "-ffreestanding",
    "-nostdlib",
    "-nostdinc",
    "-fno-builtin",
    "-fno-stack-protector",
    "-fno-exceptions",
    # AMD's BSP uses this too: stops the compiler from turning byte loops into
    # memcpy/memset calls that a freestanding kernel does not provide.
    "-fno-tree-loop-distribute-patterns",
    "-Wall",
    "-Wextra",
    # Warnings in a kernel are bugs waiting for the right hardware state.
    # This one is not theoretical: the ARM build shipped a `return;` inside
    # a function declared to return a stack pointer (c_irq_handler's spurious
    # path). With -Werror absent it compiled to a warning, and the caller's
    # `mov sp, r0` would have loaded a garbage stack pointer.
    "-Werror",
    "-g",
    "-I./arch/arm32/include",
    "-MMD",
    "-MP",
)

# ⚠ **不要**把 `-I./include` 加进 ARM32_COMMON_FLAGS。
#
# 上游的头文件树是 **C++ 专属**的,拿给 C 用会当场炸(实测,逐条):
#   include/stdint.h:20   typedef char int8_t     与 ARM 的 signed char 冲突
#                         (ARM 上 char 默认无符号 ⇒ 上游那条本身在 ARM 上就是错的)
#   include/stdint.h:38   #define NULL 0          与 ARM 的 ((void *)0) 冲突
#   include/krlibc.h:22   typedef typeof(nullptr) nullptr_t   —— C++ 专属语法
#   include/krlibc.h:217  static memmove          与 ARM 的非 static 声明冲突
# ⇒ 规矩:**移植侧的 C 文件看不到上游头文件;上游的 C++ 文件看不到移植侧头文件**
#   (后者指不被它们 include;`-I` 顺序仍保证它们能找到自己的那一份)。
#   两边唯一的交界是"上游 C++ 导出 C 链接符号 + 移植侧给出 C 声明",
#   由 tests/test_arm32_device.py 之类的契约测试钉住。
#
# C++(上游)文件的 `-I` 顺序 —— 三层,顺序本身是**契约**:
#
#   1. arch/arm32/include/upstream   覆盖层:架构相关的上游头(如 cpu/lock.h,
#                                    那份是 x86 内联汇编)在这里换成 ARM 版。
#                                    **只有架构相关的头可以放这里**。
#   2. include                       上游头文件树(默认那一层)。
#   3. arch/arm32/include            移植侧自己的头(`<arch/...>`),给覆盖层用。
#
# ⚠ 顺序不能反:把 (3) 提到最前会让上游文件拿到移植侧的 <krlibc.h>,
#   而它与上游的 <stdint.h> 在 int8_t/NULL 上直接冲突(实测)。
ARM32_UPSTREAM_INCLUDE = (
    "-I./arch/arm32/include/upstream -I./include -I./arch/arm32/include"
)

# 上游(.cpp)里被编进 ARM 内核的文件。
#
# 这是"合流"的落点,所以**显式列举、不做目录扫描**:上游 kernel/ 与 driver/
# 里有大量 x86 专有文件,扫进来只会得到满屏编译错误,而"到底哪个上游文件
# 被编进来了"必须是一份可评审的清单。
#
# 每加一项的门槛不是"能不能编过",而是"它依赖的东西到位了没有" ——
# 反例见 docs/ZYNQ7020_PORT_PLAN.md §4.6:M4A-1.1b 时 driver/device.cpp
# 编不过**与 C++ 无关**,它卡在 VFS/分区层与进程层上。
ARM32_UPSTREAM_CXX = (
    # 57 行,只依赖 malloc/calloc/free。它原先在移植侧有一份 C 副本
    # (arch/arm32/src/id_alloc.c)—— 那份是"上游是 .cpp 而当时没有 C++ 规则"
    # 的临时品;C++ 规则到位后就该删掉、改回共用这一份(源 OS 优先)。
    "kernel/id_alloc.cpp",
    #
    # ✅ `driver/fs/vfs/vfs.cpp`(1433 行)与 `driver/fs/vfs/tmpfs.cpp`(323 行)
    #
    #    **链接缺口已经清零**(2026-09-19)。缺口是一步步关掉的:
    #      18 → 11(`a8de6d2`:落地层补进程层最小切片与一批 getter)
    #      11 →  2(同批:id_alloc/lock_queue 到位后重测)
    #       2 →  0(本次:`sprintf` / `write_serial_fmt` —— 定义在 C++ 落地层,
    #              转发到 console.c 里重构后的格式化核心)
    #    清单与理由见 `docs/ZYNQ7020_PORT_PLAN.md` §4.6 的 M4A-1.2b 行。
    #
    #    它们能编过的前提都已经落地:平台中立的头重构(vfs.h / list.h /
    #    device.h 去掉 proto.hpp)、架构覆盖层(cpu/lock.h、cpu/regio.h)、
    #    以及一批平台中立修复(见各自提交)。
    #
    # ✅ 已经落进来的一批(纯复用,不是重写):
    #    `kernel/lock_queue.cpp` —— 提供 VFS 缺的 `queue_get`/`queue_dequeue`/
    #    `queue_destroy`,实测在 ARM 上零错误编过。
    #
    # ⚠ 仍然**没有调用者**的是:落地层里的 `page_map_range_to_random` /
    #   `scheduler_wake_task` / `get_current_directory`(都是"响亮拒绝"式实现),
    #   以及 VFS 本体 —— 后者要等 heap 与 `vfs_init()` 接上(M4A-1.2 的验收项)。
    #   这是**过渡态**,不是完成态 —— 记在这里以免被当成"没人用的代码"删掉。
    "kernel/lock_queue.cpp",
    "driver/fs/vfs/vfs.cpp",
    "driver/fs/vfs/tmpfs.cpp",
    #
    # ✅ **FATFS(M4A-1.4)**。5 个文件全部**零修改**编过(`-c`),链接缺口
    #    **正好 8 个符号** —— 两个来源:
    #      fatfs.cpp   mutex_create/lock/unlock、mktime、realtime_ns、
    #                  ahci_is_qemu_environment
    #      diskio.cpp  ahci_is_qemu_environment、alloc_frames、phys_to_virt
    #    全部是 **C++ 链接**(`_Z12mutex_createP5mutexb` 实测),所以实现在
    #    C++ 落地层、**不能**写 `extern "C"`(与 VFS 那组正好相反,见
    #    tests/test_arm32_fatfs.py 的开头)。落地层的取舍见
    #    `arch/arm32/src/upstream_api.cpp` 的 FATFS 一节。
    #
    #    ⚠ `ffunicode.cpp` 是 2MB 的表(Unicode 大小写/码页),`ff.cpp` 322KB。
    #      它们只在配置打开 LFN 时才会被真正用到,但**必须链进来**
    #      (`FF_USE_LFN 3`)。编译时间因此明显变长,这是它的正常代价。
    "driver/fs/fatfs/ff.cpp",
    "driver/fs/fatfs/ffunicode.cpp",
    "driver/fs/fatfs/ffsystem.cpp",
    "driver/fs/fatfs/diskio.cpp",
    "driver/fs/fatfs/fatfs.cpp",
    #
    # ✅ **M4A-1.5:`dev.cpp`(上游 devfs)+ `pipefs.cpp`(管道)**。
    #    两个都**零修改**编过;加上它们之后的链接缺口只有 4 个符号,
    #    而且其中 3 个是"同名不同符号"那一类(坑表 55):
    #      dev.cpp    disk_size(int)   → 移植侧有 C 版,但上游要**修饰名**
    #                 write_serial_string / blk_device_read / blk_device_write
    #    ⚠ ⚠ 这一笔**同时**把移植侧那份 devfs **骨架**送走了:
    #      骨架与上游 dev.cpp **都定义** `devfs_register` / `devfs_delete`
    #      (C 链接、同名),而内核链接带 `-z muldefs` ⇒ ld 会**静默挑一个**,
    #      谁生效取决于命令行顺序。那种"看起来能跑"的重复定义必须显式二选一;
    #      选上游(源 OS 优先)顺带把"devfs 骨架"那条退化项消掉。
    "driver/fs/vfs/dev.cpp",
    "driver/fs/vfs/pipefs.cpp",
    #
    # ✅ **pty(M4A-1.5)**。链接缺口只有 **1 个符号**:`snprintf(char*, size_t, …)`
    #    —— 上游那个**有边界**的格式化函数(`serial_port.cpp:761`),声明在
    #    `proto.hpp:31`(**不在** `extern "C"` 里 ⇒ 要的是修饰名
    #    `_Z8snprintfPcjPKcz`,所以定义在 C++ 落地层,转发布移植侧
    #    `console_vsnprintf()` 那个有边界的缓冲区汇)。
    #
    # ⚠ 这一笔**只到起搏**:`pty_init()` 已被调用(板上判据 `pty_init`/`pty_ptmx_node`),
    #   但**配对的读写往返还没有判据** —— 形状已查清(见 upstream_api.cpp 的
    #   pty 一节:从设备号不能假定是 0),留作下一步。
    "driver/fs/vfs/pty.cpp",
)

# 上游文件里**刻意没有**进 ARM 图的那些 —— 每条都要写清"为什么"与"什么能解开它"。
#
# 为什么要有这张表:一张"哪些文件没编进来"的清单如果只存在于人的记忆里,
# 它就分不清两种情况 —— **"决定不编"** 与 **"忘了编"**。
# 这两种情况的后续动作完全相反(前者要写理由并等条件,后者要立刻补上),
# 而它们在构建图里长得一模一样。所以把它写成数据,并由
# `tests/test_arm32_upstream_excluded.py` 钉住两条不变量:
#   ① 每条都有非空理由;② 同一路径**不能**同时出现在 ARM32_UPSTREAM_CXX 里。
#
# ⚠ 这里只放**有实测证据**的条目。写"大概不行"会把这表变成猜测集,
#   而猜测集的下场是没人再信它。
ARM32_UPSTREAM_EXCLUDED = (
    (
        "driver/fs/vfs/procfs.cpp",
        "GCC 的 **C++** 前端直接拒绝:procfs.cpp:595 的稀疏数组指定初始化器"
        "(`[0]=\"fpu\", [1]=\"vme\", …`)报 "
        "'sorry, unimplemented: non-trivial designated initializers not supported'"
        "(上游用 clang,它把这个当扩展收下了)。"
        "★ 但就算语法能过**也不该搬**:那张表是 **CPUID 的 EDX/ECX 位名**"
        "(`tsc`/`sse`/`avx`/`rdrand`…),ARM 上这些位没有意义 —— "
        "ARM 的 `/proc/cpuinfo` 必须由 **MIDR/MPIDR/CTR/CCSIDR** 拼出来。"
        "⇒ 这是\"要另写一份 ARM 的\",不是\"差一点就能编\"。"
        "解开条件:有东西真的读 `/proc`(现实里是 M7 的用户态;今天 ARM 上没有读者)。",
    ),
    (
        "driver/serial/serial_port.cpp",
        "x86 的 8250/16550 串口驱动:直接用 `inb`/`outb` 操作 0x3F8 一族端口,"
        "而 Zynq 的串口是 MMIO(PS UART1 @ 0xE0001000)。"
        "移植侧的输出通道是 `arch/arm32/src/console.c` + `uart_ps.c`;"
        "★ 它需要的两个符号(`sprintf`/`write_serial_fmt`)已经由落地层给出,"
        "所以上游**代码**能调它们,但这**份实现**搬不过来。"
        "解开条件:不需要 —— 这是架构差异,不是欠账。",
    ),
    (
        "driver/rtc.cpp",
        "PC 的 CMOS 时钟(0x70/0x71 端口)。Zynq 上**没有这个器件** ⇒ "
        "那份实现不是\"还没搬\",是\"搬过来也没有硬件\"(已记进 README 退化清单 D24)。"
        "★ 其中**纯算术**的部分(`mktime`)已经逐行照搬到落地层,"
        "并有宿主逐日期比对(`tests/test_arm32_fatfs.py`)。"
        "解开条件:Zynq PS RTC(`0xF8006000`)驱动,或由控制台设一次时间。",
    ),
    (
        "driver/device.cpp",
        "块层(`device_read`/`blk_device_*`/`device_mmap`/bounce 缓冲)。"
        "它依赖**分区层**与 x86 的帧分配器(`alloc_frames`/`phys_to_virt`)"
        "⇒ 属 M4A-3/B5 的工作面。今天 ARM 侧那两个符号是\"响亮拒绝 + 计数\""
        "(README 退化清单 D25,`arm_blk_device_calls()` 必须恒为 0)。"
        "解开条件:M4A-1.3(真实块设备)+ M4A-3/B5。",
    ),
)

# 移植侧的 C++ 源文件(**只有落地层**)。
#
# `arch/arm32/src/` 下除它之外全是 `.c` —— 这是刻意的:落地层是唯一需要
# 同时站在"上游名字"与"移植侧原语"两个世界里的地方,而它必须用 C++ 的
# 理由写在那个文件头上(上游声明没有 extern "C")。
ARM32_PORT_CXX = (
    "arch/arm32/src/upstream_api.cpp",
)

# Candidate install roots for the Vitis GNU toolchain, used only when the
# compiler is not already on PATH.  The path itself lives in the repository-root
# `config.py` (that is the one file to edit on a new machine); a couple of stock
# install locations are kept as a courtesy fallback.
_VITIS_ARM_ROOTS = (
    Path(config.ARM_TOOLCHAIN_DIR),
    Path(r"C:\Xilinx\Vitis\2025.2\gnu\aarch32\nt\gcc-arm-none-eabi"),
)


def resolve_arm_toolchain() -> tuple[str, str, str]:
    """Return (cc, objcopy, libgcc_arg) for the ARM cross build.

    Resolution order:
      1. explicit ARM_CC / ARM_OBJCOPY / ARM_LIBGCC environment variables
      2. a toolchain already on PATH (arm-none-eabi-gcc)
      3. the toolchain named by config.py (and a stock Xilinx location)

    About libgcc: ARM has no hardware integer division, so 32/64-bit division
    emits __aeabi_uidiv / __aeabi_uldivmod calls that live in libgcc.  With
    -nostdlib they are not pulled in automatically, so the link must add them.

    The default is a plain `-lgcc`: the GCC driver resolves its own multilib
    tree, and that matters here.  Xilinx's aarch32 toolchain has *no* ARM-state
    hard-float multilib at all -- the only non-Thumb variants are armv5te -- so
    for `-mcpu=cortex-a9 -marm -mfpu=vfpv3 -mfloat-abi=hard` the driver selects
    `thumb/v7-a+fp/hard`.  Hardcoding the obvious-looking
    `.../usr/lib/arm-xilinx-eabi/*/libgcc.a` instead picks a soft-float archive
    and the link fails with:
        "uses VFP register arguments, libgcc.a(_udivmoddi4.o) does not"
    Set ARM_LIBGCC only when an explicit archive path is genuinely required.
    """
    cc = os.environ.get("ARM_CC", "")
    objcopy = os.environ.get("ARM_OBJCOPY", "")
    libgcc = os.environ.get("ARM_LIBGCC", "")

    # 2. PATH lookup.  shutil.which returns None when absent, unlike
    #    find_executable() which falls back to returning the bare name.
    if not cc:
        for name in ("arm-none-eabi-gcc", "arm-none-eabi-gcc.exe", "arm-xilinx-eabi-gcc"):
            found = shutil.which(name)
            if found:
                cc = found
                break

    # 3. The toolchain named by config.py, then a stock Xilinx location.
    if not cc:
        for root in _VITIS_ARM_ROOTS:
            for name in ("arm-none-eabi-gcc.exe", "arm-none-eabi-gcc"):
                candidate = root / "bin" / name
                if candidate.exists():
                    cc = str(candidate)
                    break
            if cc:
                break

    if not cc:
        raise SystemExit(
            "ARM toolchain not found.\n"
            f"  config.py says it should be at:\n    {config.ARM_CC}\n"
            "  Fix VITIS_DIR in the repository-root config.py (one file), or set\n"
            "  ARM_CC to the compiler directly, e.g.\n"
            '    set ARM_CC=<vitis>\\gnu\\aarch32\\nt\\gcc-arm-none-eabi\\bin\\arm-none-eabi-gcc.exe\n'
            "  Self-check: python config.py\n"
            "  See docs/BUILD_ARM32.md."
        )

    cc_path = Path(cc)
    # <root>/bin/<cc>; a PATH lookup always yields an absolute path, so
    # parent is meaningful here.
    bin_dir = cc_path.parent

    if not objcopy:
        objcopy = "arm-none-eabi-objcopy"
        for name in ("arm-none-eabi-objcopy.exe", "arm-none-eabi-objcopy"):
            candidate = bin_dir / name
            if candidate.exists():
                objcopy = str(candidate)
                break

    # Default to driver-resolved libgcc rather than a hardcoded archive.
    libgcc_arg = libgcc if libgcc else "-lgcc"

    return cc, objcopy, libgcc_arg


def arm32_graph(n: Ninja, out_path: Path) -> list[Path]:
    """Emit the Zynq-7020 build graph and return its output targets."""
    cc, _objcopy, libgcc_arg = resolve_arm_toolchain()

    n.comment("ARM32 TOOLCHAIN - Vitis GNU toolchain (see docs/ZYNQ7020_PORT_PLAN.md 6.7)")
    n.var("arm_cc", cc)
    n.var("arm_libgcc", libgcc_arg)
    n.var("arm_cflags", " ".join(ARM32_COMMON_FLAGS))
    # C++(上游)文件用的那套:去掉移植侧的 -I。
    # 理由与"不要给 C 文件加 -I./include"是同一条规矩的另一半 ——
    # 两边的头文件树在同一个 TU 里是会打架的(实测:上游 stdint.h 的
    # `typedef char int8_t` 与移植侧的 `signed char` 冲突,因为在 ARM 上
    # `char` 默认无符号)。上游文件只该看见上游的世界。
    n.var("arm_cxx_cflags", " ".join(f for f in ARM32_COMMON_FLAGS if f != "-I./arch/arm32/include"))
    n.var("arm_arch_flags", " ".join(ARM32_ARCH_FLAGS))
    # ⚠ `-Wl,-z,muldefs`(= --allow-multiple-definition)**不是移植侧发明的**:
    # 源 OS 自己的内核链接就带这个标志(见本文件 x86 内核的链接命令:
    # `ld -z muldefs -T linker.ld --static ...`)。
    # 为什么非它不可:`include/fs/vfs/list.h` 在**头文件里直接定义**了
    # `list_delete` 一族(不是 inline、不是 static)。上游 x86 侧只有一个
    # TU(vfs.cpp)包含它,所以没暴露;ARM 侧一旦有两个 TU 包含
    # (vfs.cpp 与落地层 upstream_api.cpp),ld 就报 multiple definition。
    # ⇒ 与源 OS 保持同一条链接语义,而不是去改上游头(list.h 是公共 ABI 头)。
    n.var("arm_ldflags",
          f"-nostdlib -T {ARM32_SOURCE_ROOT}/boot/kernel.ld -Wl,-z,muldefs -Wl,-Map=out/kernel-arm.map")
    n.line()

    # Source discovery stays explicit: the ARM tree is small and every file in
    # it is first-party, unlike the x86 side which sweeps kernel/driver/lib.
    arm_c_sources = find_files(f"{ARM32_SOURCE_ROOT}/src", (".c",))
    arm_s_sources = find_files(f"{ARM32_SOURCE_ROOT}/boot", (".S",))
    arm_headers = find_files(f"{ARM32_SOURCE_ROOT}/include", (".h",))

    n.comment("ARM32 FILES")
    n.var_list("ARM32_C_SOURCES", arm_c_sources)
    n.var_list("ARM32_S_SOURCES", arm_s_sources)
    n.var_list("ARM32_HEADERS", arm_headers)
    n.line()

    n.comment("ARM32 RULES")
    # NOTE: these rules deliberately avoid `mkdir -p $$(dirname $out)`.
    # Native Windows Ninja calls CreateProcess directly, so shell builtins like
    # `mkdir` and `dirname` are unavailable and every edge fails with
    # "CreateProcess failed".  The object directories are therefore created by
    # this generator instead (see below), keeping the rules shell-independent.
    # `ninja clean` only deletes declared output files, not directories, so the
    # tree survives a clean; a manual `rm -rf out` just needs a regeneration.
    n.rule(
        "arm32_cc",
        "$arm_cc $arm_arch_flags $arm_cflags -std=gnu11 -O2 -MF $out.d -c $in -o $out",
        log_desc("CC", "$in -> $out"),
        depfile="$out.d",
    )
    # 上游的 .cpp。与 arm32_cc 的差别只有三处:-std=gnu++17、-fno-rtti
    # (异常已在 ARM32_COMMON_FLAGS 里关掉),以及它照样能用 -I./include。
    #
    # 为什么 C++ 值得单独一条规则(而不是把上游文件改写成 C):
    # 上游 kernel/、driver/ 全是 .cpp,改写成 C 等于**分叉**;而合流的目标
    # 恰恰是让它们成为两个架构的共同资产。实测代价很小 —— 只要架构头对
    # C++ 友好(见 arch/arm32/include/arch/types.h 的 bool 守卫与
    # krlibc.h 的 extern "C")。
    n.rule(
        "arm32_cxx",
        # `-Wno-error=attributes`:上游 `include/efi/efi.h` 的 `EFIAPI`/`ms_abi`
        # 是 **MSVC/x86 的调用约定属性**,ARM 的 GCC 只能忽略它并报
        #   "'ms_abi' attribute directive ignored [-Wattributes]"
        # 而凡是通过 `proto.hpp` 走的翻译单元都会碰到 efi.h
        # (proto.hpp 是 x86 的"万能头",里面还塞着 serial/printf/kernel 的声明)。
        #
        # ⚠ 这一条**降级为警告而不是关掉**:它照样会打出来,只是不再让构建失败。
        #   彻底的做法是把 proto.hpp 拆开(那是一件独立的上游重构,不在本步),
        #   在那之前,让"上游头文件里的 x86 属性"挡住整个 VFS 是不划算的。
        #   —— 移植侧自己的 C 文件仍然全程 -Werror,那条线没有被放松。
        f"$arm_cc $arm_arch_flags $arm_cxx_cflags {ARM32_UPSTREAM_INCLUDE} -std=gnu++17 -fno-rtti "
        # ★ 警告策略(2026-09-18 定):
        #   **移植侧自己的代码全程 `-Werror`;上游代码在 ARM 上只把警告降级,不关掉。**
        #
        #   理由不是"图省事",是两边编译器不同:上游是用 **clang** 开发的,
        #   而 ARM 图用 **GCC 13**。同一份上游代码在两者下的警告面不一样,
        #   实测碰到的就有五类:
        #     - 宏重定义(`include/mm/page.h` 的 PROT_* vs `include/syscall/syscall.h`)
        #     - 函数指针强转(`(vfs_ioctl_t)pipefs_ioctl` —— 回调表本就是一组签名)
        #     - 缺失字段初始化(`vt_mode` 的 acqsig/frsig/…)
        #     - 悬空声明(`proto.hpp` 的 `static inline uint64_t rdtsc();`,已单独删掉)
        #     - ★ 假阳性:`-Wsequence-point`,只有一处,已逐行分析过(见下)
        #
        #   ★ 第五类(`driver/fs/vfs/vfs.cpp:907`)单独说明,因为它的**形状像真 bug**,
        #     下一步做 tmpfs 的建/读/写时一定会路过它,不该被当成嫌疑犯去查:
        #
        #         node->parent->child = list_delete(node->parent->child, node);
        #
        #     GCC 报 "operation on 'node->parent->child' may be undefined"。
        #     逐条对下来它是**良定义**的:
        #       ① 实参 `node->parent->child` 的读取**先于**调用完成,赋值**后于**
        #          调用返回 —— 这正是 C 里 `x = f(x)` 这个惯用法的次序保证;
        #       ② `list_delete(list_t list, void *data)`(`list.h:283`)的链表头是
        #          **传值**的,它内部对参数 `list` 的写**不可能**指向调用方的
        #          `child` 字段;它 `free()` 掉的是**链表包装结点**,与 `data`
        #          (那个 vfs_node)不是同一个对象。
        #     ⇒ 之所以还是报,是 GCC 把 `list.h` 里那个**头文件内定义**的
        #       `list_delete` 内联之后,别名分析无法排除"同一个对象被改两次"。
        #       clang(上游的编译器)不报。
        #     ⇒ **决定:不动上游代码。** 这是警告级、且改动会碰公共 ABI 头
        #       (`list.h` 的签名/行为);记录在这里,为了让将来查
        #       "结点从 child 链表里消失/泄漏"的人先排除它,而不是先怀疑它。
        #
        #   要"对 GCC 也零警告"只有两条路:改上游(越权)或关掉整类警告(掩盖真问题)。
        #   降级为警告保留了**全部可见性**,而"能不能跑"由链接与板上判据决定。
        "-Wno-error -O2 -MF $out.d -c $in -o $out",
        log_desc("CXX", "$in -> $out"),
        depfile="$out.d",
    )
    # Assembly goes through the GNU driver so it sees the same -mcpu/-mfpu set.
    #
    # -I is required, not cosmetic: boot/vectors.S includes <arch/taskctx.h> so
    # that the exception-frame offsets have ONE definition shared with C.  The
    # static asserts in that header therefore also protect the assembly side --
    # see docs/ZYNQ7020_PORT_PLAN.md 4.7.6 (x86 hard-codes the same kind of
    # offsets in kernel/intr/handler.S with no compile-time check at all).
    n.rule(
        "arm32_as",
        f"$arm_cc $arm_arch_flags -I{ARM32_SOURCE_ROOT}/include -MMD -MP -MF $out.d -c $in -o $out",
        log_desc("ASM", "$in -> $out"),
        depfile="$out.d",
    )
    # libgcc is passed as an explicit path: the Xilinx tree is a multilib
    # layout that -L/-lgcc does not resolve correctly.
    n.rule(
        "arm32_link",
        "$arm_cc $arm_arch_flags $arm_ldflags -o $out $in $arm_libgcc",
        log_desc("LD", "$out"),
    )

    n.comment("ARM32 ARTIFACTS - Zynq-7020 kernel ELF")
    # find_files() yields absolute paths; strip the arch root so objects land in
    # out/arm32/<subdir>/<name>.o mirroring the source tree.
    arm_source_root_abs = ROOT / ARM32_SOURCE_ROOT
    arm_objs: list[Path] = []
    for src in arm_s_sources:
        obj = Path(ARM32_OBJ_ROOT) / src.relative_to(arm_source_root_abs).with_suffix(".o")
        # implicit=arm_headers:vectors.S 现在 include <arch/taskctx.h>,
        # 改帧布局必须触发汇编重编,否则跑的还是旧偏移。
        n.build(obj, "arm32_as", src, implicit=arm_headers)
        arm_objs.append(obj)
    for src in arm_c_sources:
        obj = Path(ARM32_OBJ_ROOT) / src.relative_to(arm_source_root_abs).with_suffix(".o")
        n.build(obj, "arm32_cc", src, implicit=arm_headers)
        arm_objs.append(obj)

    # 上游的 .cpp:对象放在 out/arm32/upstream/ 下,与移植侧分开放 ——
    # 这样 `ls out/arm32/upstream` 就是一份"哪些上游文件已经编进来了"的清单。
    for rel in ARM32_UPSTREAM_CXX:
        src = ROOT / rel
        obj = Path(ARM32_OBJ_ROOT) / "upstream" / Path(rel).with_suffix(".o")
        n.build(obj, "arm32_cxx", src)
        arm_objs.append(obj)

    # 移植侧的 C++ **落地层**(arch/arm32/src 下唯一不是 .c 的东西)。
    # 它必须用 C++ 的理由写在文件头上:上游那三个调度器函数的声明没有
    # `extern "C"`,C 实现给不出对应的符号。
    for rel in ARM32_PORT_CXX:
        src = ROOT / rel
        obj = Path(ARM32_OBJ_ROOT) / Path(rel).relative_to(ARM32_SOURCE_ROOT).with_suffix(".o")
        n.build(obj, "arm32_cxx", src)
        arm_objs.append(obj)

    # Create the object directories up front.  Doing it here rather than in the
    # rule commands is what keeps the rules shell-independent (see ARM32 RULES).
    for obj in arm_objs:
        (ROOT / obj).parent.mkdir(parents=True, exist_ok=True)

    kernel = Path(ARM32_KERNEL)
    n.build(
        kernel,
        "arm32_link",
        arm_objs,
        implicit=arm_headers + [Path(f"{ARM32_SOURCE_ROOT}/boot/kernel.ld")],
    )
    n.line()

    n.comment("ARM32 PHONY TARGETS")
    phony(n, "arm32", [kernel])
    phony(n, "kernel.arm", [kernel])
    n.line("default arm32")

    return [kernel]


def main() -> None:
    # Generation is deterministic for a given checkout/environment.  Toolchain
    # probes happen here so generated flags and Rust library paths are visible
    # at the top of build.ninja.
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", default=str(OUT))
    parser.add_argument(
        "--arch",
        default=os.environ.get("ARCH", "x86_64"),
        choices=("x86_64", "arm32"),
        help=(
            "Target architecture.  x86_64 keeps the historical UEFI/PC build graph; "
            "arm32 emits the Zynq-7020 (Cortex-A9) graph.  The value is baked into the "
            "generated regen rule so `ninja` re-runs the generator with the same arch."
        ),
    )
    parser.add_argument(
        "--fail-if-changed",
        action="store_true",
        help="write the manifest, then fail if it changed so Ninja can be rerun with the fresh graph",
    )
    args = parser.parse_args()

    # Environment knobs keep the same defaults as the historical build.
    builtin_xhci = os.environ.get("BUILTIN_XHCI", "1") == "1"
    cpp_extra = " -DXHCI_BUILTIN" if builtin_xhci else ""
    host_gcc = os.environ.get("HOST_GCC", "gcc")
    host_gcc_triple = cmd_output([host_gcc, "-dumpmachine"])
    rust_target = os.environ.get("RUST_TARGET", "x86_64-unknown-none")
    rustc = find_rustc()
    # No Rust user targets are currently emitted. Do not make C/C++ build graph
    # generation depend on an otherwise unused Rust target installation.
    rust_libs: list[Path] = []

    n = Ninja()
    n.comment("Generated by tools/gen_ninja.py. Run `ninja reconfigure` after changing the build graph.")
    n.comment("Edit tools/gen_ninja.py, not this generated build.ninja file.")
    n.var("ninja_required_version", "1.10")
    # Python interpreter used by generation/preflight/staging rules.  Windows has
    # no `python3` on PATH, so allow an explicit override instead of hardcoding.
    n.var("python", os.environ.get("PYTHON", "python3"))

    # ------------------------------------------------------------------
    # ARM32 short-circuit.
    #
    # The Zynq port replaces boot/, the arch layer and every platform driver,
    # so there is nothing to share with the x86_64 graph beyond the generator
    # itself.  Emitting it separately keeps the historical x86 graph byte-for-
    # byte unchanged and avoids inventing arch conditionals inside every rule.
    # ------------------------------------------------------------------
    if args.arch == "arm32":
        arm32_graph(n, Path(args.out))
        changed = write_if_changed(Path(args.out), "\n".join(n.lines) + "\n")
        if changed and args.fail_if_changed:
            raise SystemExit("build.ninja was refreshed; rerun ninja so it can load the updated graph")
        return

    n.var("cc", os.environ.get("CC", os.environ.get("COMPILER_PREFIX", "") + "clang"))
    n.var("cxx", os.environ.get("CPP", os.environ.get("COMPILER_PREFIX", "") + "clang++"))
    n.var("nasm", os.environ.get("NASM", os.environ.get("COMPILER_PREFIX", "") + "nasm"))
    n.var("ld", os.environ.get("LD", os.environ.get("COMPILER_PREFIX", "") + "ld"))
    n.var("objc", os.environ.get("OBJC", os.environ.get("COMPILER_PREFIX", "") + "objcopy"))
    n.var("host_cc", os.environ.get("HOST_CC", "cc"))
    n.var("user_cc", os.environ.get("CC", os.environ.get("COMPILER_PREFIX", "") + "clang"))
    n.var("user_cxx", os.environ.get("CXX", "g++"))
    n.var("user_ld", os.environ.get("LD", "ld"))
    n.var("rustc", rustc)
    n.var("rust_target", rust_target)
    n.line()
    # The generated FILES section is intentionally verbose: it lets a developer
    # inspect exactly what the generator discovered without reading Python code.
    n.comment("FILES - generated source and header discovery lists")
    # These root lists intentionally feed both the readable FILES section and
    # root_objects(); one scan owns both display and build behavior.
    root_source_roots = ["kernel", "driver", "lib"]
    nasm_files: list[Path] = []
    asm_files: list[Path] = []
    c_files: list[Path] = []
    cpp_files: list[Path] = []
    for base in root_source_roots:
        nasm_files.extend(find_files(base, (".asm",)))
        asm_files.extend(find_files(base, (".S",)))
        c_files.extend(find_files(base, (".c",)))
        cpp_files.extend(find_files(base, (".cpp",)))
    cpp_files = [p for p in cpp_files if p != ROOT / "kernel/stress.cpp"]
    if builtin_xhci:
        cpp_files.extend(find_files("kmod/xhci", (".cpp",)))
    xapi_sources = [ROOT / "user/xapi/arch/x86_64/crt0.S", ROOT / "user/xapi/libsys.cpp"]
    n.var("BUILTIN_XHCI", "1" if builtin_xhci else "0")
    n.var("ROOT_SOURCE_DIRS", " ".join(root_source_roots))
    n.var_list("NASM_FILES", nasm_files)
    n.var_list("ASM_FILES", asm_files)
    n.var_list("C_FILES", c_files)
    n.var_list("CPP_FILES", cpp_files)
    n.var_list("HEADER_FILES", headers("include"))
    n.var_list("XAPI_SOURCES", xapi_sources)
    n.var_list("XAPI_HEADERS", headers("user/xapi/include"))
    n.var_list("BOOT_HEADERS", find_files("boot/include", (".h", ".hpp")))
    n.var_list("KMOD_E1000_SOURCES", find_files("kmod/e1000", (".cpp",), max_depth=1))
    n.line()
    # Emit uppercase canonical flag variables first, then lowercase aliases for
    # older generated rule bodies.  That makes the build options readable while
    # preserving compatibility with existing variable names.
    n.comment("FLAGS - canonical compile and link options")
    n.var("OPT_LEVEL", os.environ.get("OPT_LEVEL", "2"))
    n.var("OPT_FLAG", "-O$OPT_LEVEL")
    n.var("RUST_OPT_FLAG", "-C opt-level=$OPT_LEVEL")
    n.var("DIAGNOSTIC_COLOR", c_diagnostic_color_flag())
    n.var("RUST_DIAGNOSTIC_COLOR", rust_diagnostic_color_flag())
    n.var("NASM_FLAGS", "-f elf64 -DX86_64_TARGET -DUEFI")
    n.var("ASM_FLAGS", "$DIAGNOSTIC_COLOR $OPT_FLAG -g -mno-red-zone -mstackrealign -nostdlib -ffreestanding -fno-builtin -m64 -fno-stack-protector -fno-exceptions -fno-strict-aliasing -mno-80387 -MMD -MP")
    n.var("C_FLAGS", "$DIAGNOSTIC_COLOR $OPT_FLAG -g -mno-red-zone -mstackrealign -nostdlib -ffreestanding -fno-builtin -m64 -fno-stack-protector -fno-exceptions -fno-strict-aliasing -std=c11 -fshort-wchar -nostdinc -mno-80387 -I./include -MMD -MP")
    n.var("CPP_FLAGS", "$DIAGNOSTIC_COLOR $OPT_FLAG -g -mno-red-zone -mstackrealign -nostdlib -ffreestanding -fno-builtin -m64 -fno-stack-protector -fno-exceptions -fno-strict-aliasing -fno-rtti -std=gnu++17 -fshort-wchar -nostdinc -fno-use-cxa-atexit -fno-threadsafe-statics -mno-80387 -Wno-int-to-pointer-cast -Wno-macro-redefined -Wno-c11-extensions -Wno-c99-extensions -Wno-gnu-statement-expression-from-macro-expansion -I./include -Wno-writable-strings -Wno-c++11-narrowing -MMD -MP" + cpp_extra)
    n.var("BOOT_C_FLAGS", "$DIAGNOSTIC_COLOR -g -I ./boot/include -Wextra -e efi_main -nostdinc -nostdlib -fno-builtin -Wl,--subsystem,10 -fshort-wchar")
    n.var("USER_CFLAGS", "$DIAGNOSTIC_COLOR $OPT_FLAG -Wall -g -ffreestanding -fno-builtin -m64 -mstackrealign -std=c++11 -fno-stack-protector -fno-exceptions -fno-strict-aliasing -fshort-wchar -nostdinc -I ./user/xapi/include -I ./include -Wno-write-strings -MMD -MP")
    n.var("RUST_FLAGS", "$RUST_DIAGNOSTIC_COLOR --edition=2024 --target $rust_target --emit=obj --crate-type lib -C panic=abort -C no-redzone=yes $RUST_OPT_FLAG -C debuginfo=2 -C overflow-checks=off")
    n.var("KMOD_CXXFLAGS", "$DIAGNOSTIC_COLOR $OPT_FLAG -g -mno-red-zone -mstackrealign -nostdlib -ffreestanding -fno-builtin -m64 -fno-stack-protector -fno-exceptions -fno-strict-aliasing -fno-rtti -std=gnu++17 -fshort-wchar -nostdinc -fno-use-cxa-atexit -fno-threadsafe-statics -mno-80387 -Wno-int-to-pointer-cast -Wno-macro-redefined -Wno-c11-extensions -Wno-c99-extensions -Wno-gnu-statement-expression-from-macro-expansion -Wno-writable-strings -Wno-c++11-narrowing -fPIC -fvisibility=hidden -MMD -MP")
    n.var("NETSERVER_CFLAGS", "$DIAGNOSTIC_COLOR $OPT_FLAG -g -mno-red-zone -mstackrealign -nostdlib -ffreestanding -fno-builtin -m64 -fno-stack-protector -fno-exceptions -fno-strict-aliasing -std=gnu11 -fshort-wchar -nostdinc -mno-80387 -fPIC -fvisibility=hidden -MMD -MP -I./include -I./kmod/netserver/lwip/include -I./kmod/netserver")
    n.var("NETSERVER_CXXFLAGS", "$KMOD_CXXFLAGS -MMD -MP -I./include -I./kmod/netserver/lwip/include -I./kmod/netserver")
    n.var("opt", "$OPT_FLAG")
    n.var("rust_opt", "$RUST_OPT_FLAG")
    n.var("diagnostic_color", "$DIAGNOSTIC_COLOR")
    n.var("rust_diagnostic_color", "$RUST_DIAGNOSTIC_COLOR")
    n.var("nasm_flags", "$NASM_FLAGS")
    n.var("asm_flags", "$ASM_FLAGS")
    n.var("c_flags", "$C_FLAGS")
    n.var("cpp_flags", "$CPP_FLAGS")
    n.var("boot_c_flags", "$BOOT_C_FLAGS")
    n.var("user_cflags", "$USER_CFLAGS")
    n.var("rust_user_flags", "$RUST_FLAGS")
    n.var("kmod_cxxflags", "$KMOD_CXXFLAGS")
    n.var("netserver_cflags", "$NETSERVER_CFLAGS")
    n.var("netserver_cxxflags", "$NETSERVER_CXXFLAGS")
    n.comment("Lowercase aliases above keep rule bodies compatible with older generated graphs.")
    n.var("host_gcc_triple", host_gcc_triple)
    n.line()

    n.comment("RULES - reusable shell commands used by build edges")
    # Rules stay generic; per-target differences are passed as edge variables so
    # command strings do not need to be duplicated for every app/module.
    n.rule(
        "regen",
        f"$python tools/gen_ninja.py --out build.ninja --arch {args.arch}",
        log_desc("GEN", "build.ninja"),
        generator=True,
        restat=True,
    )
    n.rule(
        "refresh_manifest",
        f"$python tools/gen_ninja.py --out build.ninja --arch {args.arch} --fail-if-changed && "
        "mkdir -p $$(dirname $out) && touch $out",
        log_desc("GEN", "refresh build.ninja"),
        generator=True,
        restat=True,
    )
    n.rule("nasm", "mkdir -p $$(dirname $out) && $nasm $nasm_flags $in -o $out", log_desc("ASM", "$in -> $out"))
    n.rule("root_as", "mkdir -p $$(dirname $out) && $cc -mcmodel=large $asm_flags -MF $out.d -c $in -o $out", log_desc("ASM", "$in -> $out"), depfile="$out.d")
    n.rule("root_cc", "mkdir -p $$(dirname $out) && $cc -fno-pic -fno-pie -mcmodel=large $c_flags -MF $out.d -c $in -o $out", log_desc("CC", "$in -> $out"), depfile="$out.d")
    n.rule("root_cxx", "mkdir -p $$(dirname $out) && $cxx -fno-pic -fno-pie -mcmodel=large $cpp_flags -MF $out.d -c $in -o $out", log_desc("CXX", "$in -> $out"), depfile="$out.d")
    n.rule("gen_config", "tmp=$out.tmp; { echo '#pragma once'; echo '#define CONFIG_KERNEL_BUSYBOX_ALIASES '$${KERNEL_BUSYBOX_ALIASES:-1}; echo '#define CONFIG_KERNEL_BUILTIN_XHCI '$${BUILTIN_XHCI:-1}; } > $$tmp; if ! cmp -s $$tmp $out; then mv $$tmp $out; else rm -f $$tmp; fi", log_desc("GEN", "$out"))
    n.rule("boot", "mkdir -p $$(dirname $out) && x86_64-w64-mingw32-gcc $boot_c_flags -o $out boot/bootx64.c boot/bootlib.c", log_desc("BOOT", "$out"))
    n.rule("objcopy_bin", "mkdir -p $$(dirname $out) && $objc -I binary -O elf64-x86-64 $bin_input $out", log_desc("OBJCOPY", "$bin_input"))
    n.rule(
        "kernel_link",
        "$ld -z muldefs -T linker.ld --static --wrap=malloc --wrap=calloc --wrap=realloc --wrap=aligned_alloc "
        "-o $out $in",
        log_desc("LD", "$out"),
    )
    n.rule("copy", "mkdir -p $$(dirname $out) && cp -a $in $out", log_desc("CP", "$in -> $out"))
    n.rule("user_cxx_custom", "mkdir -p $$(dirname $out) && $user_cxx $cflags -MF $out.d -c $in -o $out", log_desc("CXX", "$in -> $out"), depfile="$out.d")
    n.rule("user_cc_custom", "mkdir -p $$(dirname $out) && $user_cc $cflags -MF $out.d -c $in -o $out", log_desc("CC", "$in -> $out"), depfile="$out.d")
    n.rule("rust_user", "mkdir -p $$(dirname $out) && $rustc $rust_user_flags $in -o $out", log_desc("RUST", "$in -> $out"))
    n.rule("xapi_cxx", "mkdir -p $$(dirname $out) && $user_cxx $diagnostic_color $opt -Wall -g -ffreestanding -fno-builtin -m64 -mstackrealign -std=c++11 -fno-stack-protector -fno-strict-aliasing -fshort-wchar -nostdinc -I ./user/xapi/include -Wno-write-strings -MMD -MP -MF $out.d -c $in -o $out", log_desc("CXX", "$in"), depfile="$out.d")
    n.rule("xapi_as", "mkdir -p $$(dirname $out) && $user_cc $diagnostic_color $opt -Wall -g -ffreestanding -fno-builtin -m64 -mstackrealign -std=c++11 -fno-stack-protector -fno-strict-aliasing -fshort-wchar -nostdinc -I ./user/xapi/include -Wno-write-strings -MMD -MP -MF $out.d -c $in -o $out", log_desc("ASM", "$in"), depfile="$out.d")
    n.rule("xapi_head", "mkdir -p $$(dirname $out) && $user_cc $diagnostic_color $opt -Wall -g -ffreestanding -fno-builtin -m64 -mstackrealign -std=c++11 -fno-stack-protector -fno-strict-aliasing -fshort-wchar -nostdinc -I ./user/xapi/include -Wno-write-strings -MMD -MP -MF $out.d -c $in -o $out", log_desc("CXX", "start.cpp"), depfile="$out.d")
    n.rule("xapi_headcon", "mkdir -p $$(dirname $out) && $user_cc $diagnostic_color $opt -Wall -g -ffreestanding -fno-builtin -m64 -mstackrealign -std=c++11 -fno-stack-protector -fno-strict-aliasing -fshort-wchar -nostdinc -I ./user/xapi/include -Wno-write-strings -MMD -MP -MF $out.d -c $in -o $out", log_desc("CXX", "constart.cpp"), depfile="$out.d")
    n.rule("user_ld", "$user_ld -Ttext=0x200000 $in -o $out", log_desc("LD", "$out"))
    n.rule("host_cc", "mkdir -p $$(dirname $out) && $user_cc $diagnostic_color -O0 -g -Wall -Wextra $in -o $out", log_desc("HOSTLD", "$in"))
    n.rule("kmod_e1000_cxx", "mkdir -p $$(dirname $out) && $cxx $kmod_cxxflags -I./include -MF $out.d -c $in -o $out", log_desc("CXX", "$in"), depfile="$out.d")
    n.rule("kmod_xhci_cxx", "mkdir -p $$(dirname $out) && $cxx $kmod_cxxflags -I./include -I./kmod/xhci -MF $out.d -c $in -o $out", log_desc("CXX", "$in"), depfile="$out.d")
    n.rule("netserver_cc", "mkdir -p $$(dirname $out) && $cc $netserver_cflags -MF $out.d -c $in -o $out", log_desc("CC", "$in"), depfile="$out.d")
    n.rule("netserver_cxx", "mkdir -p $$(dirname $out) && $cxx $netserver_cxxflags -MF $out.d -c $in -o $out", log_desc("CXX", "$in"), depfile="$out.d")
    n.rule(
        "package_compliance",
        "$python tools/package_third_party.py --output out/compliance/third-party",
        log_desc("STAGE", "third-party compliance"),
        restat=True,
    )
    n.rule("kmod_link", "$cxx -shared $in $ldflags -o $out", log_desc("LD", "$out"))
    n.rule("python_cmd", "$python tools/ninja_build.py $cmd", "$desc")
    n.rule("shell_cmd", "$cmd", "$desc")

    n.comment("MANIFEST - regenerate build.ninja before every normal Ninja execution")
    manifest_inputs = [
        "LICENSES.md",
        "THIRD_PARTY_NOTICES.md",
        *[str(path) for path in sorted(Path("licenses").rglob("*")) if path.is_file()],
    ]
    # The manifest self-edge only tracks the generator itself.  The forced
    # preflight below checks hand-maintained graph-affecting inputs before every
    # normal build, without making source edits permanently dirty build.ninja.
    n.build(
        "build.ninja",
        "regen",
        ["tools/gen_ninja.py", "tools/ninja_build.py"],
        use_default_order_only=False,
    )
    # Ninja cannot safely force the build.ninja self-edge every run: that causes
    # manifest regeneration loops.  Instead, every real build edge below waits
    # for this forced preflight.  If the preflight changes build.ninja, it fails
    # before stale graph commands execute; the next `ninja` invocation reads the
    # refreshed graph.
    n.build("always_refresh_manifest", "phony", use_default_order_only=False)
    refresh_stamp = Path("out/.ninja-preflight")
    n.build(
        refresh_stamp,
        "refresh_manifest",
        ["tools/gen_ninja.py", "tools/ninja_build.py", "always_refresh_manifest"],
        implicit=manifest_inputs,
        use_default_order_only=False,
    )
    n.default_order_only = [refresh_stamp]
    n.comment("CORE ARTIFACTS - bootloader, config header, support library, kernel")
    n.build("kernel/build_config.h", "gen_config")

    kernel_objs = root_objects(n, nasm_files, asm_files, c_files, cpp_files)
    n.build("out/BOOTX64.efi", "boot", ["boot/bootx64.c", "boot/bootlib.c"], implicit=find_files("boot/include", (".h",)))
    n.build("font/hankaku.o", "objcopy_bin", "font/hankaku.bin", variables={"bin_input": "./font/hankaku.bin"})
    n.build("out/kernel.krl", "kernel_link", kernel_objs + [Path("font/hankaku.o"), Path("liballoc-x86_64.a")], implicit=["linker.ld"])
    compliance_bundle = Path("out/compliance/third-party/MANIFEST.json")
    compliance_manifest_path = Path("third_party/compliance-manifest.json")
    compliance_manifest = json.loads((ROOT / compliance_manifest_path).read_text(encoding="utf-8"))
    compliance_inputs = [
        Path("tools/package_third_party.py"),
        Path("tools/generate_lwip_notice.py"),
        compliance_manifest_path,
        Path("LICENSE"),
        Path("THIRD_PARTY_NOTICES.md"),
    ]
    for component in compliance_manifest["components"]:
        packaged_materials = list(component["license_files"])
        if component["bundle_source"]:
            packaged_materials.extend(component["source_files"])
        for material in packaged_materials:
            material_path = Path(material)
            if material_path not in compliance_inputs:
                compliance_inputs.append(material_path)
    n.build(
        compliance_bundle,
        "package_compliance",
        compliance_inputs,
    )

    n.comment("USERLAND AND KMODS - first-class ELF/module outputs")
    core_objs, constart_obj, xapi_liballoc = xapi(n)
    user_targets = user_apps(n, core_objs, constart_obj)
    kmod_targets = kmods(n)

    n.comment("PHONY TARGETS - compatibility names for common build workflows")
    all_deps = [Path("out/BOOTX64.efi"), Path("out/kernel.krl"), compliance_bundle] + user_targets + kmod_targets
    phony(n, "all", all_deps)
    phony(n, "build_xapi", core_objs + [constart_obj, xapi_liballoc])
    phony(n, "kmods", kmod_targets)

    n.comment("IMAGE AND UTILITY TARGETS - delegate staging/run helpers to tools/ninja_build.py")
    # Image creation, QEMU launch, formatting, and graph reporting stay in the
    # helper script because they are imperative workflows rather than compile
    # edges.  Ninja still tracks their visible target names here.
    # License files are copied during staging.  Make staging targets depend on
    # an always-dirty phony input so an existing image is refreshed on every
    # request, including `complete`, rather than being skipped by Ninja.
    n.build("always_stage_licenses", "phony", use_default_order_only=False)
    image_deps = all_deps + manifest_inputs + ["always_stage_licenses"]
    n.build(
        ["vdisk", "XJ380.img"],
        "python_cmd",
        image_deps,
        variables={"cmd": "vdisk", "desc": log_desc("VDISK", "XJ380.img"), "pool": "console"},
    )
    n.build(
        "complete",
        "python_cmd",
        image_deps,
        variables={"cmd": "complete", "desc": log_desc("VDISK", "complete XJ380.img"), "pool": "console"},
    )
    n.build(
        ["installer.system.stage", "out/system-payload.pak"],
        "python_cmd",
        image_deps,
        variables={"cmd": "installer-system-stage", "desc": log_desc("STAGE", "installer system")},
    )
    n.build(
        ["installer.root.stage", "out/installer-root.pak"],
        "python_cmd",
        all_deps,
        variables={"cmd": "installer-root-stage", "desc": log_desc("STAGE", "installer root")},
    )
    n.build(
        ["installer.iso", "XJ380-installer.iso"],
        "python_cmd",
        ["installer.system.stage", "installer.root.stage"],
        variables={"cmd": "installer-iso", "desc": log_desc("ISO", "XJ380-installer.iso")},
    )
    n.build(["vmdk", "XJ380.vmdk"], "python_cmd", ["vdisk"], variables={"cmd": "vmdk", "desc": log_desc("VMDK", "XJ380.vmdk")})
    n.build("run", "python_cmd", ["vdisk"], variables={"cmd": "run", "desc": log_desc("RUN", "QEMU"), "pool": "console"})
    n.build("justrun", "python_cmd", variables={"cmd": "run", "desc": log_desc("RUN", "QEMU"), "pool": "console"})
    n.build("prepare", "python_cmd", variables={"cmd": "prepare", "desc": log_desc("PREP", "root")})
    n.build("installer.prepare", "python_cmd", variables={"cmd": "installer-prepare", "desc": log_desc("PREP", "installer root")})
    n.build("stage.selfhost-user", "python_cmd", variables={"cmd": "stage-selfhost-user", "desc": log_desc("STAGE", "selfhost user")})
    n.build("stage.linux-compat", "python_cmd", variables={"cmd": "stage-linux-compat", "desc": log_desc("STAGE", "linux compat")})
    n.build("format", "python_cmd", variables={"cmd": "format", "desc": log_desc("FMT", "sources")})
    n.build("check", "python_cmd", variables={"cmd": "check", "desc": log_desc("CHECK", "sources")})
    n.build("gen.clangd", "python_cmd", variables={"cmd": "gen-clangd", "desc": log_desc("GEN", "clangd")})
    n.build("size", "python_cmd", variables={"cmd": "size", "desc": log_desc("SIZE", "report")})
    n.build("check.tools", "python_cmd", variables={"cmd": "check-tools", "desc": log_desc("CHECK", "tools")})
    n.build("graph", "python_cmd", variables={"cmd": "graph", "desc": log_desc("GRAPH", "out/build-graph.dot")})
    n.build("clean.clangd", "shell_cmd", variables={"cmd": "rm -f .clangd boot/.clangd", "desc": log_desc("CLEAN", "clangd")})
    n.build("clean", "python_cmd", variables={"cmd": "clean", "desc": log_desc("CLEAN", "build outputs")})
    n.build("reconfigure", "regen", ["tools/gen_ninja.py", "tools/ninja_build.py"])
    n.line("default all")

    output = Path(args.out)
    changed = write_if_changed(output, "\n".join(n.lines) + "\n")
    if changed and args.fail_if_changed:
        raise SystemExit("build.ninja was refreshed; rerun ninja so it can load the updated graph")


if __name__ == "__main__":
    main()
