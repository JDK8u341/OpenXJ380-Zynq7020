#!/usr/bin/env python3
"""x86 单文件语法检查 —— 在原生 Windows 上验证"上游改动没弄坏 x86"。

## 为什么需要它

x86 的构建图(`build.ninja`)**在原生 Windows 上跑不起来**:它的
`refresh_manifest` 规则用的是 Unix shell 语法
(`python tools/gen_ninja.py ... && mkdir -p $(dirname ...) && touch ...`),
而 ninja 在 Windows 上走 CreateProcess、不经 shell —— `&&` 会被当成
python 的参数传进去。所以本机无法用 `ninja -f build.ninja` 验证 x86。

但**单个翻译单元可以绕过 ninja 直接编**:从 `build.ninja` 里抄出
`cpp_flags`,用 clang 对指定文件做 `-fsyntax-only`。这足以回答
"我改了某个上游头文件之后,包含它的那些文件还能不能编"。

## 用法

    python tmp-test/check_x86_syntax.py                    # 全部
    python tmp-test/check_x86_syntax.py <文件> [<文件>...]  # 指定

退出码 0 = 全部通过。输出 rc != 0 的文件与它的诊断。

⚠ 两个已知的、**与环境无关**的例外:
  - `kernel/main.cpp` 依赖生成的 `kernel/build_config.h`,这里没有 ⇒ 跳过;
  - 其余文件必须 rc=0。若某个文件本来就不通过,先记下基线再谈改动。
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# 从 build.ninja 的 CPP_FLAGS 抄来(去掉与语法检查无关的 -g/-MMD/-MP/-O)。
X86_CPP_FLAGS = [
    "--target=x86_64-unknown-linux-gnu",
    "-mno-red-zone", "-mstackrealign", "-nostdlib", "-ffreestanding", "-fno-builtin",
    "-m64", "-fno-stack-protector", "-fno-exceptions", "-fno-strict-aliasing", "-fno-rtti",
    "-std=gnu++17", "-fshort-wchar", "-nostdinc", "-fno-use-cxa-atexit",
    "-fno-threadsafe-statics", "-mno-80387",
    "-Wno-int-to-pointer-cast", "-Wno-macro-redefined", "-Wno-c11-extensions",
    "-Wno-c99-extensions", "-Wno-gnu-statement-expression-from-macro-expansion",
    "-Wno-writable-strings", "-Wno-c++11-narrowing",
    "-I./include",
    "-DXHCI_BUILTIN",
    "-fsyntax-only",
]

# 需要生成头(kernel/build_config.h),本检查器造不出来
SKIP = {"kernel/main.cpp"}

DEFAULT_SET = [
    # fs/vfs/vfs.h 的全部包含者(改那个头必须全过)
    "driver/fs/partition.cpp", "driver/fs/fatfs/fatfs.cpp",
    "driver/fs/vfs/dev.cpp", "driver/fs/vfs/dnsfs.cpp", "driver/fs/vfs/nmfs.cpp",
    "driver/fs/vfs/pipefs.cpp", "driver/fs/vfs/procfs.cpp", "driver/fs/vfs/pty.cpp",
    "driver/fs/vfs/socketfs.cpp", "driver/fs/vfs/tmpfs.cpp", "driver/fs/vfs/vfs.cpp",
    "driver/hda/vsound.cpp", "driver/ide/ide.cpp",
    "kernel/dlinker.cpp", "kernel/installer_mode.cpp", "kernel/xflib.cpp",
    "kernel/syscall/message.cpp", "kernel/syscall/sys.cpp", "kernel/syscall/syscall.cpp",
    "kernel/syscall/xapi/xfile.cpp", "kernel/syscall/xapi/xtui.cpp",
    "kernel/task/pcb.cpp", "kernel/task/reaper.cpp",
    "kernel/user/hardware_reg.cpp", "kernel/user/runfile.cpp", "kernel/user/ulog.cpp",
    "kernel/user/user.cpp", "kernel/user/x3tp.cpp",
    "lib/stdio.cpp",
    # 顺带:设备层与 id 分配器(与 ARM 侧的契约相关)
    "driver/device.cpp", "kernel/id_alloc.cpp",
]


def check(files: list[str]) -> int:
    bad = 0
    for rel in files:
        if rel in SKIP:
            print(f"   SKIP  {rel}")
            continue
        r = subprocess.run(
            ["clang++", *X86_CPP_FLAGS, rel],
            cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace",
        )
        if r.returncode == 0:
            print(f"   rc=0  {rel}")
        else:
            bad += 1
            print(f"   rc={r.returncode}  {rel}")
            for line in (r.stderr or "").strip().splitlines()[:6]:
                print(f"         {line[:150]}")
    return bad


def main() -> int:
    files = sys.argv[1:] or DEFAULT_SET
    print(f"x86 语法检查:{len(files)} 个文件")
    bad = check(files)
    print(f"未通过:{bad}")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
