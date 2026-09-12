"""ARM32 控制台格式化输出的单元测试。

被测代码是 arch/arm32/src/console.c。它只依赖 uart_putc/uart_puts/uart_init
三个函数,测试里用桩替换掉,就能在宿主机上直接编译运行console 的真实实现,
验证格式化行为本身。

这与项目原有的 tests/test_krlibc_formatter.py 不同 —— 那个只是对源码做
字符串匹配(检查某些标记不存在),并没有真的跑过格式化器。这里跑真实代码。

对应 x86 侧的 kernel/krlibc.cpp 的格式化设施。
"""

from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

HARNESS = r"""
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <arch/console.h>
#include <arch/uart_ps.h>

/* ------------------------------------------------------------------ */
/* 桩:替代真实 UART,把输出收进缓冲区                                  */
/* ------------------------------------------------------------------ */

static char   g_out[4096];
static size_t g_len;

/*
 * 行为刻意与 arch/arm32/src/uart_ps.c 保持一致:
 *   uart_putc 不做换行转换,uart_puts 才把 \n 展开成 \r\n。
 * 桩若与真实实现不一致,测出来的就不是产品的行为了。
 */
void uart_putc(uintptr_t base, char c)
{
    (void)base;
    if (g_len + 1 < sizeof(g_out)) {
        g_out[g_len++] = c;
    }
}

void uart_puts(uintptr_t base, const char *str)
{
    (void)base;
    while (*str != '\0') {
        if (*str == '\n') {
            uart_putc(base, '\r');
        }
        uart_putc(base, *str++);
    }
}

void uart_init(uintptr_t base, u32 clk, u32 baud, uart_baud_result_t *result)
{
    (void)base;
    (void)clk;
    (void)baud;
    if (result != NULL) {
        result->valid = false;
    }
}

/* ------------------------------------------------------------------ */
/* 测试骨架                                                            */
/* ------------------------------------------------------------------ */

static int failures;

static void reset(void)
{
    g_len = 0;
    memset(g_out, 0, sizeof(g_out));
}

static void expect(const char *what, const char *want)
{
    g_out[g_len] = '\0';
    if (strcmp(g_out, want) != 0) {
        printf("FAIL %-22s got [%s] want [%s]\n", what, g_out, want);
        failures++;
    }
}

int main(void)
{
    /* 任意非 0 基址即可,uart_init 是桩不会碰硬件 */
    console_init((uintptr_t)1, 100000000u, 9600u);

    /* ---- 普通文本 ---- */
    reset(); console_printf("hello");                 expect("plain", "hello");
    reset(); console_printf("");                      expect("empty", "");

    /* ---- 有符号整数 ---- */
    reset(); console_printf("%d", 0);                 expect("d zero", "0");
    reset(); console_printf("%d", 1234);              expect("d positive", "1234");
    reset(); console_printf("%d", -42);               expect("d negative", "-42");
    reset(); console_printf("%i", 7);                 expect("i", "7");

    /* ---- 无符号与进制 ---- */
    reset(); console_printf("%u", 4000000000u);       expect("u large", "4000000000");
    reset(); console_printf("%x", 0xDEADBEEFu);       expect("x lower", "deadbeef");
    reset(); console_printf("%X", 0xDEADBEEFu);       expect("X upper", "DEADBEEF");
    reset(); console_printf("%x", 0u);                expect("x zero", "0");

    /* ---- 宽度与填充 ---- */
    reset(); console_printf("%08X", 0x1234u);         expect("zero pad", "00001234");
    reset(); console_printf("%5d", 42);               expect("right align", "   42");
    reset(); console_printf("%-5d|", 42);             expect("left align", "42   |");
    reset(); console_printf("%3d", 12345);            expect("width overflow", "12345");

    /* ---- 字符与字符串 ---- */
    reset(); console_printf("%c", 'Z');               expect("c", "Z");
    reset(); console_printf("%s", "abc");             expect("s", "abc");
    reset(); console_printf("%5s", "ab");             expect("s width", "   ab");
    reset(); console_printf("%-5s|", "ab");           expect("s left", "ab   |");

    /* ---- 指针 ---- */
    reset(); console_printf("%p", (void *)0x1234);    expect("p", "0x00001234");

    /* ---- 百分号与混合 ---- */
    reset(); console_printf("100%%");                 expect("percent", "100%");
    reset(); console_printf("a%sb%dc", "X", 7);       expect("mixed", "aXb7c");

    /* ---- 换行:console_printf 自己会补 \r ---- */
    reset(); console_printf("a\nb");                  expect("newline", "a\r\nb");

    /* ---- 未知格式符原样输出,便于发现拼写错误 ---- */
    reset(); console_printf("%q");                    expect("unknown verb", "%q");

    /* ---- 专用辅助函数 ---- */
    reset(); console_put_hex32(0xDEADBEEFu);          expect("put_hex32", "0xDEADBEEF");
    reset(); console_put_dec32(0u);                   expect("put_dec32 zero", "0");
    reset(); console_put_dec32(123456u);              expect("put_dec32", "123456");

    if (failures == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d check(s) failed\n", failures);
    return 1;
}
"""


class Arm32ConsoleTests(unittest.TestCase):
    # 见 test_arm32_uart_baud.py 中关于探测编译器的说明:
    # 本机 MinGW 的 gcc/g++ 坏了(cc1.exe 静默失败),所以逐个试。
    COMPILER_CANDIDATES = ("gcc", "cc", "clang")

    def test_console_formatting(self) -> None:
        capture = {"capture_output": True, "text": True, "encoding": "utf-8", "errors": "replace"}

        with tempfile.TemporaryDirectory() as tmp:
            harness = Path(tmp) / "console_test.c"
            binary = Path(tmp) / "console_test"
            harness.write_text(HARNESS, encoding="utf-8")

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
                    str(ROOT / "arch/arm32/src/console.c"),
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
                f"console formatting checks failed:\n{run_result.stdout}{run_result.stderr}",
            )
            self.assertIn("ALL PASS", run_result.stdout)


if __name__ == "__main__":
    unittest.main()
