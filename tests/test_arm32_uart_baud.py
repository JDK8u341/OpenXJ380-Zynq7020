"""ARM32 UART 波特率分频搜索的单元测试。

被测代码是 arch/arm32/src/uart_baud.c —— 刻意做成不依赖 MMIO 的纯函数,
所以能直接用宿主编译器编译运行,不需要板子。

这段数学值得单测的原因:它曾经出过一个 7 倍的偏差 bug。
当时的标定点选了 AMD 驱动遍历范围之外的 BAUDDIV=0,
导致"参考时钟"被低估 7.033 倍,串口配置写着 9600 而线上实际跑 67.5k。

参考:arch/arm32/README.md「UART 参考时钟」与 docs/ZYNQ7020_PORT_PLAN.md
"""

from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

# 本板实测值:闭环收敛得到的 UART 参考时钟
BOARD_UART_REF_CLK = 100_000_000

HARNESS = r"""
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include <arch/uart_baud.h>

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
    uart_baud_result_t r;

    /* ---- 1. 本板实测点:100MHz 参考时钟下的 9600 ---- */
    uart_baud_search(BOARD_UART_REF_CLK, 9600u, &r);
    check(r.baudgen == 1736u, "100MHz/9600 -> BAUDGEN should be 1736");
    check(r.bauddiv == 5u, "100MHz/9600 -> BAUDDIV should be 5");
    check(r.actual == 9600u, "100MHz/9600 -> actual should be exactly 9600");
    check(r.error_ppm == 0u, "100MHz/9600 -> error should be 0 ppm");
    check(r.valid, "100MHz/9600 -> should be valid");
    check(r.requested == 9600u, "requested should be echoed back");

    /* ---- 2. BAUDDIV 下界约束(7 倍 bug 的根源) ---- */
    /*
     * 不论什么输入组合,BAUDDIV 都必须落在 AMD 遍历区间 4..254 内。
     * 用了区间外的值,公式 "baud = clk/(BAUDGEN*(BAUDDIV+1))" 就不成立。
     */
    {
        const unsigned int clocks[] = {1000000u, 10000000u, 50000000u,
                                       100000000u, 200000000u};
        const unsigned int bauds[] = {1200u, 2400u, 9600u, 19200u, 38400u,
                                      57600u, 115200u, 230400u, 460800u};
        unsigned int ci;
        unsigned int bi;

        for (ci = 0; ci < sizeof(clocks) / sizeof(clocks[0]); ci++) {
            for (bi = 0; bi < sizeof(bauds) / sizeof(bauds[0]); bi++) {
                unsigned int baudgen;
                unsigned int bauddiv;

                uart_baud_search(clocks[ci], bauds[bi], &r);
                if (!r.valid) {
                    continue; /* 组合本身不可用,无需检查区间 */
                }

                baudgen = r.baudgen;
                bauddiv = r.bauddiv;

                check(bauddiv >= 4u, "BAUDDIV must be >= 4");
                check(bauddiv <= 254u, "BAUDDIV must be <= 254");
                check(baudgen >= 1u, "BAUDGEN must be >= 1");
                check(baudgen <= 65535u, "BAUDGEN must fit in 16 bits");
            }
        }
    }

    /* ---- 3. 公式自洽:actual 必须等于按寄存器值算出来的结果 ---- */
    {
        const unsigned int clocks[] = {100000000u, 50000000u, 24000000u};
        const unsigned int bauds[] = {9600u, 115200u};
        unsigned int ci;
        unsigned int bi;

        for (ci = 0; ci < sizeof(clocks) / sizeof(clocks[0]); ci++) {
            for (bi = 0; bi < sizeof(bauds) / sizeof(bauds[0]); bi++) {
                uart_baud_search(clocks[ci], bauds[bi], &r);
                if (r.baudgen != 0u) {
                    unsigned int recomputed =
                        clocks[ci] / (r.baudgen * (r.bauddiv + 1u));
                    check(recomputed == r.actual,
                          "actual must match clk/(BAUDGEN*(BAUDDIV+1))");
                }
            }
        }
    }

    /* ---- 4. 误差必须真的小 ---- */
    {
        const unsigned int bauds[] = {9600u, 19200u, 38400u, 57600u, 115200u};
        unsigned int bi;

        for (bi = 0; bi < sizeof(bauds) / sizeof(bauds[0]); bi++) {
            uart_baud_search(BOARD_UART_REF_CLK, bauds[bi], &r);
            check(r.valid, "common baud must be reachable on a 100MHz clock");
            check(r.error_ppm < 5000u, "error should stay under 0.5%");
        }
    }

    /* ---- 5. 非法输入必须安全返回,不能崩也不能给可用结果 ---- */
    uart_baud_search(0u, 9600u, &r);
    check(!r.valid, "zero clock must be rejected");

    uart_baud_search(BOARD_UART_REF_CLK, 0u, &r);
    check(!r.valid, "zero baud must be rejected");

    /* 时钟太低,任何分频都到不了 115200 */
    uart_baud_search(100000u, 115200u, &r);
    check(!r.valid, "clock below 2x baud must be rejected");

    /* NULL 输出指针不能崩 */
    uart_baud_search(BOARD_UART_REF_CLK, 9600u, NULL);

    if (failures == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d check(s) failed\n", failures);
    return 1;
}
"""


class Arm32UartBaudTests(unittest.TestCase):
    # 候选宿主编译器,按顺序尝试第一个能用的。
    #
    # 之所以要探测而不是写死:本机(Windows + MinGW)的 gcc/g++ 坏了 ——
    # 它们会调起 cc1.exe 然后静默退出(退出码 1、无任何输出),
    # 连 `int main(void){return 0;}` 都编不过。项目原有的
    # tests/test_dma_plan.py 同样用 g++,因此在同一台机器上也是失败的。
    # 探测一下就能让本测试在坏工具链的机器上照常跑,在 Linux CI 上走 gcc。
    COMPILER_CANDIDATES = ("gcc", "cc", "clang")

    def _compile_and_run(self, source: str, extra_sources: list[Path]) -> str:
        """编译并运行测试程序,返回其 stdout。"""
        # 显式指定 UTF-8:本项目的源码注释是中文(UTF-8),而 Windows 上
        # subprocess 默认按本地代码页(简中环境是 GBK)解码,
        # 编译器回显源码行时会抛 UnicodeDecodeError,
        # 连带把 stderr 变成 None。errors="replace" 保证即使有杂字节也不炸。
        capture = {"capture_output": True, "text": True, "encoding": "utf-8", "errors": "replace"}

        with tempfile.TemporaryDirectory() as tmp:
            harness = Path(tmp) / "uart_baud_test.c"
            binary = Path(tmp) / "uart_baud_test"
            harness.write_text(source, encoding="utf-8")

            attempts: list[str] = []
            for compiler in self.COMPILER_CANDIDATES:
                command = [
                    compiler,
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "arch/arm32/include"),
                    str(harness),
                    *[str(path) for path in extra_sources],
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

                detail = (result.stderr or "").strip().replace("\n", " ")[:200]
                attempts.append(f"{compiler}: rc={result.returncode} {detail}")
            else:
                self.fail("no working host C compiler found:\n  " + "\n  ".join(attempts))

            run_result = subprocess.run([str(binary)], **capture)
            self.assertEqual(
                run_result.returncode,
                0,
                f"baud checks failed:\n{run_result.stdout}{run_result.stderr}",
            )
            return run_result.stdout

    def test_baud_divisor_search(self) -> None:
        source = HARNESS.replace("BOARD_UART_REF_CLK", f"{BOARD_UART_REF_CLK}u")
        output = self._compile_and_run(source, [ROOT / "arch/arm32/src/uart_baud.c"])
        self.assertIn("ALL PASS", output)


if __name__ == "__main__":
    unittest.main()
