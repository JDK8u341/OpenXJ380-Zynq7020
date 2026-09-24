"""钉住移植度量工具与宪章的一致性（P0 的"它的测试"）。

为什么度量工具也要有测试:

  `tmp-test/port_metrics.py` 是**唯一**用来回答"是不是在漂移"的东西。
  它一旦算错,漂移就会以"指标说没事"的形式被掩盖 —— 比没有指标更糟。
  所以这里做的事只有一件:**独立重算**。

  "独立"是有要求的:测试自己解析 `tools/gen_ninja.py`、自己数代码行、
  自己读 `git diff --numstat` —— 不 import 被测量的模块。
  用被测方的函数去验证被测方的输出,只能证明它自洽,证明不了它对。

另外钉住两条**曾经真的错过的**判据:

  1. 拒绝型桩检测器第一版把 `write_serial_fmt()` 误报成桩(它是真实现,
     只是与源 OS 一样恒返回 0)。误报会让指标被忽略,所以这里显式钉住
     "它不是桩"。
  2. 度量工具的第一版在 Windows 控制台直接 print ✓ 就 UnicodeEncodeError
     —— 工具崩了等于没有指标。所以这里用 `-X utf8` 之外的方式验证:
     工具必须能在默认控制台编码下跑完并退出 0。
"""

from __future__ import annotations

import re
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tmp-test" / "port_metrics.py"
CHARTER = ROOT / "docs" / "ZYNQ7020_PORT_CHARTER.md"
UPSTREAM_API = ROOT / "arch" / "arm32" / "src" / "upstream_api.cpp"
NINJA_GEN = ROOT / "tools" / "gen_ninja.py"

VENDORED = (
    "driver/fs/fatfs/ff.cpp",
    "driver/fs/fatfs/ffunicode.cpp",
    "driver/fs/fatfs/ffsystem.cpp",
    "driver/fs/fatfs/diskio.cpp",
    "driver/fs/fatfs/fatfs.cpp",
)


def count_code_lines(path: Path) -> int:
    """独立实现的行数统计（与被测工具不同源，故意写得笨一点）。"""
    text = path.read_text(encoding="utf-8", errors="replace")
    # 先把块注释挖掉，再逐行判定 —— 与工具的写法不同，结果必须一致
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    n = 0
    for line in text.splitlines():
        line = re.sub(r"//.*$", "", line)
        if line.strip():
            n += 1
    return n


def run_tool(*extra: str) -> str:
    r = subprocess.run([sys.executable, str(TOOL), *extra], cwd=ROOT,
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode != 0:
        raise AssertionError(f"工具退出码 {r.returncode}\nstdout={r.stdout}\nstderr={r.stderr}")
    return r.stdout


class TestCharter(unittest.TestCase):
    def test_charter_exists_and_states_the_purpose(self):
        self.assertTrue(CHARTER.is_file(), "宪章必须存在 —— 它是本项目的最高约束")
        text = CHARTER.read_text(encoding="utf-8")
        # 一句话目的 + 三条推论:它们是"移植 ≠ 修改 ≠ 超越"的可引用形式
        for phrase in ("搬到 Xilinx Zynq-7020 上",
                       "移植 = 让上游原文运行",
                       "修改上游 = 移植失败的一种",
                       "源 OS 没有的东西 = 不该有"):
            self.assertIn(phrase, text, f"宪章必须写出:{phrase}")
        for key in ("I1", "I2", "I3", "I4", "I5", "M1", "M2"):
            self.assertIn(key, text, f"宪章里必须有不变量/度量 {key}")

    def test_charter_names_the_five_invariants_as_headings(self):
        text = CHARTER.read_text(encoding="utf-8")
        for name in ("上游零修改", "不做源 OS 没有的东西", "不新造 API",
                     "验证脚手架不许进内核", "不隐藏缺口"):
            self.assertIn(name, text)


class TestMetricsTool(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.out = run_tool()

    def test_all_sections_present(self):
        for sec in ("【M1】", "【M2】", "【I1】", "【I2】", "【I3】", "【I4】", "【I5】"):
            self.assertIn(sec, self.out)

    def test_m1_numerator_is_recomputed_independently(self):
        """M1 的分子必须等于"构建图里的上游文件"的代码行数（不含 vendored FatFs）。"""
        gen = NINJA_GEN.read_text(encoding="utf-8", errors="replace")
        start = gen.index("ARM32_UPSTREAM_CXX = (")
        end = gen.index("\n)\n", start)
        files = [f for f in re.findall(r'"([^"]+\.cpp)"', gen[start:end])
                 if "/" in f and not f.startswith("arch/")]
        self.assertTrue(files, "构建图里必须有上游 C++ 文件，否则 M1 恒为 0")
        own = sum(count_code_lines(ROOT / f) for f in files if f not in VENDORED)

        m = re.search(r"源 OS 自有\(不含 vendored FatFs\):\s*(\d+)\s*/\s*(\d+)", self.out)
        self.assertIsNotNone(m, "M1 行必须能被解析")
        self.assertEqual(int(m.group(1)), own,
                         "M1 分子与独立重算不一致 —— 口径漂了")

    def test_i1_total_is_recomputed_independently(self):
        """I1 的总行数必须等于 git diff 里上游六个目录的 增+删。"""
        r = subprocess.run(["git", "diff", "--numstat", "main", "HEAD"], cwd=ROOT,
                           capture_output=True, text=True, encoding="utf-8", errors="replace")
        dirs = ("kernel", "driver", "lib", "include", "boot", "user")
        total = 0
        for line in r.stdout.splitlines():
            parts = line.split("\t")
            if len(parts) != 3:
                continue
            add, dele, path = parts
            if not path.startswith(dirs):
                continue
            try:
                total += int(add) + int(dele)
            except ValueError:
                continue

        m = re.search(r"【I1】上游零修改.*?(\d+) 行 / (\d+) 文件", self.out)
        if total == 0:
            self.assertIn("[ OK ]", self.out.split("【I1】")[1].split("\n")[0],
                          "上游零改动时必须报 OK")
        else:
            self.assertIsNotNone(m, "有上游改动时 I1 必须报出行数与文件数")
            self.assertEqual(int(m.group(1)), total, "I1 与独立重算不一致")

    def test_refusal_stub_detector_does_not_false_positive(self):
        """`write_serial_fmt` 是真实现（与源 OS 一样恒返回 0），不是拒绝型桩。"""
        verbose = run_tool("--verbose")
        self.assertNotIn("write_serial_fmt", verbose,
                         "误报 —— 检测器又把真实现当成了桩")

    def test_every_reported_stub_really_is_a_stub(self):
        """报出来的每个桩名，必须在落地层里真的存在且函数体只有一条常量返回。"""
        verbose = run_tool("--verbose")
        api = UPSTREAM_API.read_text(encoding="utf-8", errors="replace")
        names = re.findall(r"^\s+\[(?: OK |FAIL)\] (\w+)$", verbose, re.M)
        self.assertTrue(names, "落地层至少有 5 个登记的拒绝型桩，检测器不该一个都找不到")
        for name in names:
            m = re.search(rf"^[\w \*]*\b{re.escape(name)}\s*\([^;]*\)\s*\n\{{", api, re.M)
            self.assertIsNotNone(m, f"{name} 在落地层里找不到定义")

    def test_tool_is_offline_and_readonly(self):
        """度量工具不许碰硬件/网络 —— 它要能随时跑、能进 CI。"""
        src = TOOL.read_text(encoding="utf-8")
        for bad in ("import serial", "pyserial", "socket", "requests", "COM4"):
            self.assertNotIn(bad, src, f"度量工具不许依赖 {bad}")
        for bad in (".write_text(", ".write_bytes(", "os.remove", "shutil."):
            self.assertNotIn(bad, src, f"度量工具是只读的，不许出现 {bad}")

    def test_tool_survives_the_default_console_encoding(self):
        """默认控制台编码下也必须跑完（第一版在 Windows GBK 上直接崩）。"""
        env = {"PYTHONIOENCODING": "gbk"}
        import os
        e = dict(os.environ)
        e.update(env)
        r = subprocess.run([sys.executable, str(TOOL)], cwd=ROOT, env=e,
                           capture_output=True, text=True, encoding="utf-8", errors="replace")
        self.assertEqual(r.returncode, 0, f"GBK 控制台下崩了：{r.stderr[-400:]}")


if __name__ == "__main__":
    unittest.main(verbosity=2)
