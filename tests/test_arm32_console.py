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
/* 桩:排他钩子(M4-8.4)                                               */
/* ------------------------------------------------------------------ */

/*
 * 产品里这两个钩子是 `sched_disable` / `sched_enable`(由内核装入)。
 * 这里只数调用次数 —— 要验的是"成对、按嵌套深度",不是调度器本身。
 */
static int hook_begin_calls;
static int hook_end_calls;

static void stub_excl_begin(void) { hook_begin_calls++; }
static void stub_excl_end(void)   { hook_end_calls++; }

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

/* 整数形式的判据(排他钩子那几个用它 —— 它们不看输出,看调用次数) */
static void check(const char *what, int cond)
{
    if (!cond) {
        printf("FAIL %-22s\n", what);
        failures++;
    }
}

int main(void)
{
    /* 任意非 0 基址即可,uart_init 是桩不会碰硬件 */
    console_init((uintptr_t)1, 100000000u, 9600u);
    console_set_excl_hooks(stub_excl_begin, stub_excl_end);

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

    /*
     * ---- 排他输出(M4-8.4;★ M4-11.1 起本文件只做"转发" ★)----
     *
     * ⚠ 判据在 M4-11.1 **必须**改,而且理由来自上板实测:
     *
     *   原来这里是"按**全局**嵌套深度调用钩子"(进两次只叫一次 begin、
     *   出到 0 才叫 end、多退一次不叫 end)。那条规则在"排他 = 关调度"的
     *   前提下成立 —— 调度关着的时候没有别的上下文能跑起来。
     *
     *   换成真正的锁之后它就是**漏洞**:全局计数不认识持有者,于是
     *   B 线程看到 `depth != 0` 就一声不吭地直接打印,锁形同虚设。
     *   实测症状(第一次带锁上板):状态行被劈进自检报告中间
     *   (`verify_board.py` 判"报告不完整"),而"锁被竞争过吗"那个计数恒为 0。
     *
     *   ⇒ 现在本文件**每一次 begin/end 都如实叫钩子**,嵌套与"多退一次"
     *     由钩子背后那把**递归互斥**按持有者计数负责(源 OS `rec = true`
     *     的 `rcc`)。那三条性质在 tests/test_arm32_mutex.py 里逐条钉住,
     *     这里只钉"转发"这一件事。
     */
    {
        int b = hook_begin_calls;
        int e = hook_end_calls;

        console_excl_begin();
        check("excl begin forwards", hook_begin_calls == b + 1);
        console_excl_begin(); /* 嵌套:照样如实转发(递归计数在锁那一层)*/
        check("excl nested forwards", hook_begin_calls == b + 2);
        console_excl_end();
        check("excl end forwards 1", hook_end_calls == e + 1);
        console_excl_end();
        check("excl end forwards 2", hook_end_calls == e + 2);

        /* 没装钩子时是空操作:启动早期只有一个写者,那正是对的 */
        console_set_excl_hooks(NULL, NULL);
        console_excl_begin();
        console_excl_end();
        check("excl without hooks is noop",
              (hook_begin_calls == b + 2) && (hook_end_calls == e + 2));
        console_set_excl_hooks(stub_excl_begin, stub_excl_end);
    }

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
