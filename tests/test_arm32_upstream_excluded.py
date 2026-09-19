"""上游文件"刻意没进 ARM 图"的那张表的不变量(M4A-1.5)

为什么要测这个:**"决定不编"与"忘了编"在构建图里长得一模一样**,
而两者的后续动作完全相反(前者要写理由并等条件,后者要立刻补上)。
所以把"没编进来的"写成数据(`tools/gen_ninja.py` 的 `ARM32_UPSTREAM_EXCLUDED`),
并在这里钉住三条不变量:

  1. 每条都有**非空理由**(没有理由的条目 = 没做过的决定);
  2. 同一路径**不能**同时出现在 `ARM32_UPSTREAM_CXX`(自相矛盾 = 构建图在骗人);
  3. 理由里要写清"什么能解开它" —— 判据是理由文本足够长且提到 M4A/阶段,
     因为一个没有解锁条件的"暂不支持"等于永久放弃(那种决定不该悄悄做)。

⚠ 这一组是**文本契约**:它不编译任何东西,所以跑得飞快,可以每轮都跑。
"""

from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _load_gen_ninja():
    spec = importlib.util.spec_from_file_location("gen_ninja_under_test", ROOT / "tools/gen_ninja.py")
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class Arm32UpstreamExcludedTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.gen = _load_gen_ninja()

    def test_table_exists_and_is_not_empty(self) -> None:
        excluded = getattr(self.gen, "ARM32_UPSTREAM_EXCLUDED", None)
        self.assertIsNotNone(excluded, "tools/gen_ninja.py 里没有 ARM32_UPSTREAM_EXCLUDED 表")
        self.assertGreater(len(excluded), 0, "排除表是空的 —— 那说明它没有被维护")

    def test_every_entry_has_a_substantive_reason(self) -> None:
        for path, reason in self.gen.ARM32_UPSTREAM_EXCLUDED:
            with self.subTest(path=path):
                self.assertTrue(path.endswith((".cpp", ".c")), f"{path} 看起来不是源文件")
                self.assertGreaterEqual(
                    len(reason), 80,
                    f"{path} 的理由太短({len(reason)} 字)—— "
                    f"\"暂不支持\"这种话分不清\"决定\"与\"遗忘\"")
                self.assertIn("解开条件", reason,
                              f"{path} 的理由里没写\"什么能解开它\":"
                              f"没有解锁条件的排除等于永久放弃,那种决定不该悄悄做")

    def test_no_path_is_both_included_and_excluded(self) -> None:
        included = set(self.gen.ARM32_UPSTREAM_CXX)
        for path, _ in self.gen.ARM32_UPSTREAM_EXCLUDED:
            with self.subTest(path=path):
                self.assertNotIn(path, included,
                                 f"{path} 同时在'已进图'与'已排除'两张表里 —— "
                                 f"构建图在自相矛盾")

    def test_reasons_cite_evidence_or_stage(self) -> None:
        """
        理由要落到**可核的东西**上:要么给行号/符号(实测证据),
        要么给阶段号(M4A-x/M7)。只写感受的条目在这里会被挡下。
        """
        import re

        evidence = re.compile(r"\.cpp:\d+|\bM4A-\d|\bM7\b|0x[0-9A-Fa-f]+|`\w+`")
        for path, reason in self.gen.ARM32_UPSTREAM_EXCLUDED:
            with self.subTest(path=path):
                self.assertRegex(reason, evidence,
                                 f"{path} 的理由里没有行号/符号/阶段号 —— 不可核")


if __name__ == "__main__":
    unittest.main()
