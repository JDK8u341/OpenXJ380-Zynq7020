"""启动自检的判定逻辑与报告解析的单元测试。

两部分:

**一、C 侧的判定函数**(arch/arm32/include/arch/selftest.h)
  判据方向极易写反:GE 写成 LE 之后,"加速比 >= 2" 会变成 "<= 2",
  而**两种写法在通过时都打印 PASS** —— 只有在真出问题时才会表现出
  相反的结论。也就是说,判据写反的验证工具会在最需要它的时候给错答案。

**二、Python 侧的解析器**(tmp-test/verify_board.py)
  这里最危险的错误是**把"没解析到"当成"通过"**。
  报告缺失、被截断、格式被污染时,必须抛 ReportError 而不是返回 0 项通过 ——
  否则验证脚本会在内核根本没跑到自检阶段时,兴高采烈地报告"全部通过"。

  所以下面专门测:零检查项、缺 END、缺 SUMMARY、SUMMARY 与实际不符、
  区间里混入杂行 —— 每一条都必须是**失败**。
"""

from __future__ import annotations

import importlib.util
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

# 从真实板上运行抓下来的报告(2026-09 那次 16/16 通过的输出)
REAL_REPORT = """
 OpenXJ380 / ARMv7-A (Zynq-7020)
 Board probe  : total=10 disabled=0 probed=1 unclaimed=9 failed=0
=== SELF-TEST BEGIN ===
CHECK uart_present = 1 (expect == 1) PASS
CHECK uart_clock_source = 1 (expect == 1) PASS
CHECK uart_baud_ppm = 0 (expect <= 50) PASS
CHECK mmu_stage = 4 (expect == 4) PASS
CHECK mmu_enabled = 1 (expect == 1) PASS
CHECK cache_dcache_on = 1 (expect == 1) PASS
CHECK cache_icache_on = 1 (expect == 1) PASS
CHECK cache_speedup = 6 (expect >= 2) PASS
CHECK cache_maint_fail = 0 (expect == 0) PASS
CHECK l2_enabled = 1 (expect == 1) PASS
CHECK l2_effect_pct = 140 (expect >= 120) PASS
CHECK board_devices = 10 (expect >= 1) PASS
CHECK probe_probed = 1 (expect >= 1) PASS
CHECK led_writeback_fail = 0 (expect == 0) PASS
CHECK irq_ticks_eq_irq = 1 (expect == 1) PASS
CHECK irq_spurious = 0 (expect == 0) PASS
=== SELF-TEST SUMMARY: 16 passed, 0 failed ===
=== SELF-TEST END ===
[XJ380/arm32] alive loop=1 led=0x02 ticks=1279 irq=1279
"""


def _load_verify_module():
    spec = importlib.util.spec_from_file_location(
        "verify_board", ROOT / "tmp-test" / "verify_board.py"
    )
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class VerdictTests(unittest.TestCase):
    """C 侧判定函数。"""

    HARNESS = r"""
#include <stdbool.h>
#include <stdio.h>

#include <arch/selftest.h>

static int failures;

static void check(int condition, const char *what)
{
    if (!condition) {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

int main(void)
{
    /* EQ */
    check(selftest_verdict(4, 4, SELFTEST_EQ), "4 == 4");
    check(!selftest_verdict(4, 5, SELFTEST_EQ), "4 != 5 must fail EQ");

    /* GE:方向写反是这里最危险的错误 */
    check(selftest_verdict(6, 2, SELFTEST_GE), "6 >= 2");
    check(selftest_verdict(2, 2, SELFTEST_GE), "2 >= 2 (boundary passes)");
    check(!selftest_verdict(1, 2, SELFTEST_GE), "1 >= 2 must fail");

    /* LE */
    check(selftest_verdict(0, 50, SELFTEST_LE), "0 <= 50");
    check(selftest_verdict(50, 50, SELFTEST_LE), "50 <= 50 (boundary passes)");
    check(!selftest_verdict(51, 50, SELFTEST_LE), "51 <= 50 must fail");

    /* NE */
    check(selftest_verdict(1, 0, SELFTEST_NE), "1 != 0");
    check(!selftest_verdict(0, 0, SELFTEST_NE), "0 != 0 must fail");

    /*
     * 方向不能混:GE 与 LE 在非边界值上必须给出相反结论。
     * 若实现里把两者写反了,上面那些断言会有一部分通过一部分失败 ——
     * 这一条把它们钉成互斥关系。
     */
    check(selftest_verdict(6, 2, SELFTEST_GE) != selftest_verdict(6, 2, SELFTEST_LE),
          "GE and LE must disagree on 6 vs 2");

    /* 未知判据一律判失败,不放过 */
    check(!selftest_verdict(1, 1, (selftest_op_t)99), "unknown op must fail, not pass");

    /* 文本形式 */
    check(selftest_op_text(SELFTEST_EQ)[0] == '=', "EQ text");
    check(selftest_op_text(SELFTEST_GE)[0] == '>', "GE text");
    check(selftest_op_text(SELFTEST_LE)[0] == '<', "LE text");

    if (failures == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", failures);
    return 1;
}
"""

    def test_verdict_direction(self) -> None:
        capture = {"capture_output": True, "text": True, "encoding": "utf-8", "errors": "replace"}

        with tempfile.TemporaryDirectory() as tmp:
            harness = Path(tmp) / "selftest_test.c"
            binary = Path(tmp) / "selftest_test"
            harness.write_text(self.HARNESS, encoding="utf-8")

            attempts = []
            for compiler in ("gcc", "cc", "clang"):
                command = [
                    compiler,
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "arch/arm32/include"),
                    str(harness),
                    "-o",
                    str(binary),
                ]
                try:
                    result = subprocess.run(command, **capture)
                except FileNotFoundError:
                    attempts.append(f"{compiler}: not found")
                    continue
                if result.returncode == 0:
                    break
                attempts.append(f"{compiler}: rc={result.returncode}")
            else:
                self.fail("no working host C compiler: " + "; ".join(attempts))

            run = subprocess.run([str(binary)], **capture)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("ALL PASS", run.stdout)


class ParserTests(unittest.TestCase):
    """Python 侧解析器。"""

    @classmethod
    def setUpClass(cls) -> None:
        cls.mod = _load_verify_module()

    def test_real_report_parses(self) -> None:
        checks, passed, failed = self.mod.parse_report(REAL_REPORT)
        self.assertEqual(len(checks), 16)
        self.assertEqual((passed, failed), (16, 0))
        self.assertTrue(all(c.passed for c in checks))
        self.assertEqual(checks[0].name, "uart_present")
        self.assertEqual(checks[10].name, "l2_effect_pct")
        self.assertEqual(checks[10].expect, 120)
        self.assertEqual(checks[10].op, ">=")

    def test_a_failing_check_is_reported(self) -> None:
        """单项 FAIL 时,passed/failed 计数要跟着变,且该项的 passed 为 False。"""
        text = REAL_REPORT.replace(
            "CHECK cache_speedup = 6 (expect >= 2) PASS",
            "CHECK cache_speedup = 1 (expect >= 2) FAIL",
        ).replace("16 passed, 0 failed", "15 passed, 1 failed")

        checks, passed, failed = self.mod.parse_report(text)
        self.assertEqual((passed, failed), (15, 1))
        speedup = next(c for c in checks if c.name == "cache_speedup")
        self.assertFalse(speedup.passed)

    # ------------------------------------------------------------------
    # 以下每一条都必须是**失败**。把"解析不到"当成"通过"是这个脚本
    # 能犯的最危险的错误 —— 它会在内核根本没跑起来时报"全部通过"。
    # ------------------------------------------------------------------

    def test_missing_report_is_an_error(self) -> None:
        with self.assertRaises(self.mod.ReportError):
            self.mod.parse_report("kernel booted\nbut no selftest here\n")

    def test_truncated_report_is_an_error(self) -> None:
        """只有 BEGIN 没有 END —— 输出在自检途中被截断,结论不可信。"""
        text = REAL_REPORT.split("=== SELF-TEST END ===")[0]
        with self.assertRaises(self.mod.ReportError):
            self.mod.parse_report(text)

    def test_missing_summary_is_an_error(self) -> None:
        text = REAL_REPORT.replace("=== SELF-TEST SUMMARY: 16 passed, 0 failed ===\n", "")
        with self.assertRaises(self.mod.ReportError):
            self.mod.parse_report(text)

    def test_summary_mismatch_is_an_error(self) -> None:
        """SUMMARY 声明的数目与实际解析到的不符 -> 报告不完整。"""
        text = REAL_REPORT.replace("16 passed, 0 failed", "99 passed, 0 failed")
        with self.assertRaises(self.mod.ReportError):
            self.mod.parse_report(text)

    def test_empty_report_body_is_an_error(self) -> None:
        """区间里一项 CHECK 都没有 —— 绝不能当成"零项失败即通过"。"""
        text = "\n".join(
            [
                "=== SELF-TEST BEGIN ===",
                "=== SELF-TEST SUMMARY: 0 passed, 0 failed ===",
                "=== SELF-TEST END ===",
            ]
        )
        with self.assertRaises(self.mod.ReportError):
            self.mod.parse_report(text)

    def test_stray_lines_inside_the_report_are_an_error(self) -> None:
        """
        区间里混入别的行 -> 报警。
        若不报,脚本可能静默地少检查几项,而"少检查"与"检查通过"
        在输出上长得一样。
        """
        text = REAL_REPORT.replace(
            "CHECK mmu_stage = 4 (expect == 4) PASS",
            "CHECK mmu_stage = 4 (expect == 4) PASS\nsomething else entirely",
        )
        with self.assertRaises(self.mod.ReportError):
            self.mod.parse_report(text)

    def test_summary_line_outside_the_report_is_ignored(self) -> None:
        """区间外的同名文本不该被当作报告 —— 定位必须靠 BEGIN/END。"""
        text = "=== SELF-TEST SUMMARY: 0 passed, 0 failed ===\n" + REAL_REPORT
        checks, passed, failed = self.mod.parse_report(text)
        self.assertEqual((passed, failed), (16, 0))
        self.assertEqual(len(checks), 16)


if __name__ == "__main__":
    unittest.main()
