"""量一量上游 FATFS 那一组在 ARM 上的真实编译结果。

为什么要有它(M4A-1.2b 的教训):**`-fsyntax-only` 会给出过于乐观的结论** ——
内联汇编的约束只在生成代码时才检查,实测那时得到"14/17",而真正的 `-c`
是 7/17。所以这里一律用 `-c`。

用法(在仓库根目录):
    python tmp-test/measure_arm_cxx.py driver/fs/fatfs/ff.cpp ...

标志**从生成的 build-arm.ninja 里读**,不在这里抄一份 ——
抄一份就会和构建图漂移,那正是"量出来的数"与"构建出来的数"分家的经典原因。
"""

from __future__ import annotations

import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NINJA = ROOT / "build-arm.ninja"


def load_flags() -> list[str]:
    text = NINJA.read_text(encoding="utf-8")

    def var(name: str) -> str:
        match = re.search(rf"^{re.escape(name)} = (.*)$", text, re.M)
        if match is None:
            raise SystemExit(f"build-arm.ninja 里没有 {name}")
        return match.group(1).strip()

    arm_cc = var("arm_cc")
    rest = [
        *var("arm_arch_flags").split(),
        *var("arm_cxx_cflags").split(),
        "-I./arch/arm32/include/upstream",
        "-I./include",
        "-I./arch/arm32/include",
        "-std=gnu++17",
        "-fno-rtti",
        "-Wno-error",
        "-O2",
    ]
    return [arm_cc, *rest]


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2

    flags = load_flags()
    ok = 0
    bad: list[tuple[str, int, str]] = []

    with tempfile.TemporaryDirectory(prefix="measure-arm-cxx-") as tmp:
        for rel in sys.argv[1:]:
            src = ROOT / rel
            obj = Path(tmp) / (src.stem + ".o")
            result = subprocess.run([*flags, "-c", str(src), "-o", str(obj)],
                                    capture_output=True, text=True, errors="replace")
            if result.returncode == 0:
                ok += 1
                print(f"[OK ] {rel}")
                continue

            # 只留**第一处**错误的正文:后面往往是它的连锁反应
            lines = [ln for ln in (result.stderr or "").splitlines() if " error: " in ln]
            head = lines[0].strip() if lines else (result.stderr or "").strip().splitlines()[0:1]
            count = len(lines)
            bad.append((rel, count, str(head)[:220]))
            print(f"[ERR] {rel}  ({count} 处 error)")

    print(f"\n{ok}/{len(sys.argv) - 1} 编过")
    for rel, count, head in bad:
        print(f"  ✗ {rel}  ({count})\n      {head}")
    return 0 if not bad else 1


if __name__ == "__main__":
    raise SystemExit(main())
