"""ARM32 控制台格式化输出的单元测试。

被测代码是 arch/arm32/src/console.c。它只依赖 uart_putc/uart_puts/uart_init
三个函数,测试里用桩替换掉,就能在宿主机上直接编译运行console 的真实实现,
验证格式化行为本身。

这与项目原有的 tests/test_krlibc_formatter.py 不同 —— 那个只是对源码做
字符串匹配(检查某些标记不存在),并没有真的跑过格式化器。这里跑真实代码。

对应 x86 侧的 kernel/krlibc.cpp 的格式化设施。
"""

from __future__ import annotations

import re
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

HARNESS = r"""
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/*
 * ⚠ 这里**故意不 #include <stdio.h>**,而是自己声明 printf。
 *
 * 原因(实测,不是推测):本机 clang 走的是 MSVC/UCRT 目标,而 UCRT 的
 * <stdio.h> 把 printf/sprintf/vsprintf 一族声明成 `__inline` —— 于是
 * **任何**包含它的编译单元都会在目标文件里生成 `sprintf` 的**强定义**:
 *
 *     $ llvm-nm probe_harness.o | grep sprintf
 *     00000000 T sprintf          <-- 来自 <stdio.h>,不是我们写的
 *
 * 而被测的 console.c 现在正好导出同名的 `sprintf`(源 OS 的同名 API,
 * M4A-1.2 起上游 VFS 会直接调它),两者在同一可执行文件里就是
 * `lld-link: error: duplicate symbol: sprintf`。
 *
 * 自己声明 printf 就避开了:目标文件里只留**引用**,
 * 定义仍从 CRT 的导入库来(printf 内部走 __stdio_common_vfprintf,
 * 不经过 sprintf,所以不会摸到被测的那份实现)。
 * ⇒ 可执行文件里的 `sprintf` 是**唯一**的一份,单测测到的就是产品代码。
 */
int printf(const char *fmt, ...);

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
/* 薄包装:把可变参数展开后交给 v 形式                                  */
/* ------------------------------------------------------------------ */

/*
 * 被测的 v 形式收的是 `va_list`,所以判据需要这样一层薄壳 ——
 * 它和 **C++ 落地层**为上游 `sprintf`/`write_serial_fmt` 做的事**完全一样**
 * (那两个符号必须由 C++ 侧给出,理由见 arch/console.h)。
 */
static int h_vprintf(const char *fmt, ...)
{
    va_list args;
    int     written;

    va_start(args, fmt);
    written = console_vprintf(fmt, args);
    va_end(args);

    return written;
}

static int h_vsprintf(char *buf, const char *fmt, ...)
{
    va_list args;
    int     written;

    va_start(args, fmt);
    written = console_vsprintf(buf, fmt, args);
    va_end(args);

    return written;
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

/* 缓冲区形式的判据:给 sprintf 用(它不写 g_out) */
static void expect_buf(const char *what, const char *got, const char *want)
{
    if (strcmp(got, want) != 0) {
        printf("FAIL %-22s got [%s] want [%s]\n", what, got, want);
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
     * ================================================================
     * ★ M4A-1.2:长度修饰符 / 精度 / 三个入口 ★
     * ================================================================
     *
     * 这一段钉的是"源 OS 会取走的实参,移植侧一个不少地取走" ——
     * 长度修饰符缺失的症状不是"打得难看",而是**后续实参全部错位**。
     */
    {
        reset(); console_printf("%llx", 0x1122334455667788ull);
        expect("llx", "1122334455667788");

        /* 错位哨兵:64 位实参之后的那个 %d 必须是 42,不能是别的东西 */
        reset(); console_printf("%llx|%d|%s", 0x1122334455667788ull, 42, "ok");
        expect("llx no desync", "1122334455667788|42|ok");

        reset(); console_printf("%lld", -1234567890123ll);
        expect("lld negative", "-1234567890123");

        reset(); console_printf("%zu", (size_t)4000000000u);
        expect("zu", "4000000000");

        reset(); console_printf("%hhd", 300);         expect("hhd truncate", "44");
        reset(); console_printf("%hd", 65541);        expect("hd truncate", "5");
        reset(); console_printf("%ld", 1234567890l);  expect("ld", "1234567890");

        /* ---- 进制与 # 标志 ---- */
        reset(); console_printf("%o", 0755u);         expect("o", "755");
        reset(); console_printf("%#o", 0755u);        expect("#o", "0755");
        reset(); console_printf("%#x", 0x1fu);        expect("#x", "0x1f");
        reset(); console_printf("%#X", 0x1fu);        expect("#X", "0X1F");
        reset(); console_printf("%b", 5u);            expect("b", "101");

        /* ---- 精度与 * 宽度 ---- */
        reset(); console_printf("%.3d", 7);           expect("precision d", "007");
        reset(); console_printf("%.0d", 0);           expect("precision 0", "");
        reset(); console_printf("%.2s", "abcdef");    expect("precision s", "ab");
        reset(); console_printf("%*d|", 5, 42);       expect("star width", "   42|");
        reset(); console_printf("%-*d|", 5, 42);      expect("star neg width", "42   |");

        /* ---- 0x 前缀与 0 填充的次序:0 必须插在前缀之后 ---- */
        reset(); console_printf("%#08x", 0x1fu);      expect("#08x order", "0x00001f");

        /*
         * ---- 接受但不生效的标志(源 OS 生效)----
         * 判据是"**吃掉了**它":`%+d` 若不被吃掉,`%+` 会被当成未知格式符,
         * 后面那个实参就没人取 —— 于是 `|42` 会变成别的东西。
         */
        reset(); console_printf("%+d|%d", 5, 42);     expect("plus ignored", "5|42");
        reset(); console_printf("% d|%d", 5, 42);     expect("space ignored", "5|42");

        /*
         * ---- `%n` 是空操作(源 OS 语义)----
         * 取走实参、**不写内存**;哨兵值必须原封不动。
         */
        {
            int marker = 0x5A5A5A5A;

            reset(); console_printf("%n|%d", &marker, 9);
            expect("n is noop", "|9");
            check("n wrote nothing", marker == 0x5A5A5A5A);
        }

        /* ---- 空指针:两个入口都要打得出东西,不能崩 ---- */
        reset(); console_printf("%s", (const char *)NULL); expect("s null", "(null)");
        reset(); console_printf("%p", (void *)NULL);  expect("p null", "0x00000000");

        /*
         * ---- 换行:转换在**汇**里,所以 %c 的 '\n' 也被补 CR ----
         * (从前只有格式串里的字面 '\n' 会被补,`%c` 漏掉。)
         */
        reset(); console_printf("%c", '\n');          expect("c newline", "\r\n");
    }

    /*
     * ================================================================
     * ★ M4A-1.2:sprintf / write_serial_fmt 的**实现**(v 形式)★
     * ================================================================
     *
     * ⚠ 上游要的符号名是**名字修饰过**的(`_Z7sprintfPcPKcz`),那两个符号
     *   由 C++ 落地层给出(见 arch/console.h 与 upstream_api.cpp);
     *   这里测的是它们转发到的**实现** `console_vsprintf` /
     *   `console_vprintf` —— 格式化语义全在这两个函数里。
     *
     * 下面两个薄包装只是为了让判据读起来像上游的调用方式:
     * 把可变参数展开后交给 v 形式(和落地层做的是同一件事)。
     */
    {
        char buf[128];
        int  n;

        /* ============================================================ */
        /* console_vprintf:走控制台汇                                   */
        /* ============================================================ */

        reset();
        check("vprintf returns count", h_vprintf("x=%d", 7) == 3);
        expect("vprintf text", "x=7");

        reset();
        h_vprintf("a\nb");
        expect("vprintf CRLF", "a\r\nb");

        /* ============================================================ */
        /* console_vsprintf:走缓冲区汇(源 OS 的 UnsafeBufWriter 也不做换行转换) */
        /* ============================================================ */

        n = h_vsprintf(buf, "%s/%s", "a", "bb");
        check("vsprintf returns len", n == 4);
        expect_buf("vsprintf join", buf, "a/bb");

        n = h_vsprintf(buf, "%llx", 0x1122334455667788ull);
        check("vsprintf llx len", n == 16);
        expect_buf("vsprintf llx", buf, "1122334455667788");

        n = h_vsprintf(buf, "%d", -42);
        check("vsprintf negative len", n == 3);
        expect_buf("vsprintf negative", buf, "-42");

        n = h_vsprintf(buf, "");
        check("vsprintf empty len", n == 0);
        expect_buf("vsprintf empty", buf, "");

        /* 空格式串之后缓冲区必须**被写过**(结尾 NUL) */
        {
            char dirty[8];

            memset(dirty, 'X', sizeof(dirty));
            n = h_vsprintf(dirty, "%s", "hi");
            check("vsprintf overwrites", dirty[2] == '\0' && n == 2);
        }

        /*
         * ⚠ 与 console_printf 的**关键差异**:缓冲区汇不做换行转换
         *   (源 OS 的 UnsafeBufWriter 也不做)。上游拿它拼的是路径,
         *   路径里塞进一个 CR 就是 bug。
         */
        n = h_vsprintf(buf, "a\nb");
        check("vsprintf keeps LF", n == 3);
        expect_buf("vsprintf no CRLF", buf, "a\nb");

        /* 路径拼接:这正是上游 vfs.cpp:651 的用法 */
        n = h_vsprintf(buf, "/%s/%s", "dev", "stdio");
        expect_buf("vsprintf path", buf, "/dev/stdio");
        check("vsprintf path len", n == 10);
    }

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


# ----------------------------------------------------------------------
# 契约:sprintf / write_serial_fmt 必须与**源 OS 的声明逐字一致**
# ----------------------------------------------------------------------

"""
为什么这条非钉不可:

M4A-1.2 起,上游的 VFS/tmpfs 会**直接**调 `sprintf`(driver/fs/vfs/vfs.cpp、
tmpfs.cpp 里的路径拼接),而它们的声明来自**上游头**(include/proto.hpp 与
user/xapi/include/krlibc.h)。实现在移植侧(arch/arm32/src/console.c),
声明在 arch/console.h。

两边只要差半个字 —— 少个 `const`、返回 `void` 而不是 `int`、参数顺序不同 ——
上游调用点就会**用错调用约定**,而且**链接期看不出来**:C 没有名字修饰,
符号名两边都是 `sprintf`,链接器会高高兴兴地把它们接在一起。
唯一的拦路办法就是对着上游头逐字比(本测试),以及让编译器同时看到两边
(做不到 —— 两个世界的头文件互不可见,那正是合流纪律 #4)。
"""


def _strip_comments(text: str) -> str:
    """去掉 /* ... */ 与 // 注释,免得注释里的示例声明被当成真声明。"""
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def _param_types(params: str) -> tuple[str, ...]:
    """把形参表规范化成"只有类型、没有名字"的元组。"""
    result: list[str] = []
    for raw in params.split(","):
        piece = " ".join(raw.split())
        if not piece:
            continue
        if piece == "...":
            result.append("...")
            continue
        piece = re.sub(r"\[[^\]]*\]", " *", piece)  # 数组形参在 C 里是指针
        piece = piece.replace("*", " * ")
        words = piece.split()
        if len(words) > 1:
            words = words[:-1]  # 末位是参数名
        result.append("".join(words))
    return tuple(result)


def _prototype(text: str, name: str) -> tuple[str, tuple[str, ...]]:
    """取 `name` 的(返回类型, 形参类型元组)。"""
    pattern = re.compile(
        r"^[ \t]*([A-Za-z_][A-Za-z_0-9 \t\*]*?)\b" + re.escape(name) + r"[ \t]*\(([^;{)]*)\)[ \t]*;",
        re.M,
    )
    match = pattern.search(_strip_comments(text))
    if match is None:
        raise AssertionError(f"找不到 {name} 的声明")
    return " ".join(match.group(1).split()), _param_types(match.group(2))


def _extern_c_ranges(text: str) -> list[tuple[int, int]]:
    """粗略取出 `extern "C" {` … `}` 的字符区间,用来判"某声明在不在 C 块里"。"""
    ranges: list[tuple[int, int]] = []
    for match in re.finditer(r'extern\s*"C"\s*\{', text):
        depth = 1
        index = match.end()
        while index < len(text) and depth > 0:
            if text[index] == "{":
                depth += 1
            elif text[index] == "}":
                depth -= 1
            index += 1
        ranges.append((match.start(), index))
    return ranges


def _in_extern_c(text: str, position: int) -> bool:
    return any(start <= position < end for start, end in _extern_c_ranges(text))


class Arm32UpstreamFormatterContract(unittest.TestCase):
    """
    `sprintf` / `write_serial_fmt` 这两个符号的**形状**必须和源 OS 一致。

    为什么这条非钉不可:M4A-1.2 起上游 VFS/tmpfs 会直接调它们
    (driver/fs/vfs/vfs.cpp:651 拼路径、:1246 起打诊断),而声明来自上游头
    (include/proto.hpp:38,42)。实现在移植侧。两边只要差半个字 ——
    少个 `const`、返回 `void` 而不是 `int` —— 上游调用点就会用错调用约定,
    而且**链接期看不出来**:C 没有名字修饰,链接器只认符号名。

    ★ 更隐蔽的一层:M4A-1.2b 第一次链接时,移植侧在 console.c 里定义了
      **C 链接**的 `sprintf`,而 proto.hpp 里那两条声明**不在** `extern "C"`
      块内 —— 于是上游要的是 `_Z7sprintfPcPKcz`,我们给的是 `sprintf`,
      链接器报 `undefined reference to 'sprintf(char*, char const*, ...)'`。
      源码上两个名字**长得一模一样**,只有 nm 能看出不是同一个符号。
      ⇒ 所以这里同时钉住三件事:上游怎么声明的、落地层怎么定义的、
        以及移植侧的 C 头**没有**再造一个同名陷阱。
    """

    UPSTREAM_PROTO = "include/proto.hpp"
    UPSTREAM_KRLIBC = "user/xapi/include/krlibc.h"
    LANDING_LAYER = "arch/arm32/src/upstream_api.cpp"
    PORT_HEADER = "arch/arm32/include/arch/console.h"
    SYMBOLS = ("sprintf", "write_serial_fmt")

    def _upstream(self, name: str) -> tuple[str, tuple[str, ...]]:
        for relative in (self.UPSTREAM_PROTO, self.UPSTREAM_KRLIBC):
            text = _strip_comments((ROOT / relative).read_text(encoding="utf-8"))
            try:
                return _prototype(text, name)
            except AssertionError:
                continue
        self.fail(f"上游两个头文件里都没有 {name} 的声明")

    def test_upstream_actually_declares_them(self) -> None:
        """先确认这两个符号**上游真的有**,不是移植侧凭空加的。"""
        self.assertEqual(self._upstream("sprintf")[0], "int")
        self.assertEqual(self._upstream("write_serial_fmt")[0], "int")

    def test_upstream_declares_them_with_cpp_linkage(self) -> None:
        """
        ★ 这条是整组判据的**根因** ★

        proto.hpp 里 `sprintf` / `write_serial_fmt` 的声明若哪天被挪进
        `extern "C"` 块,符号形状就会**变成未修饰的** —— 那时落地层里那两条
        C++ 定义(以及它们产生的修饰名)反而成了错的。
        这条断言会在那一刻失败,把"上游改了链接约定"这件事**显式地**摊开,
        而不是让人从一次莫名其妙的 undefined reference 里猜。
        """
        text = _strip_comments((ROOT / self.UPSTREAM_PROTO).read_text(encoding="utf-8"))
        for name in self.SYMBOLS:
            with self.subTest(symbol=name):
                match = re.search(r"^[ \t]*[A-Za-z_][A-Za-z_0-9 \t\*]*?\b" + re.escape(name) + r"[ \t]*\(",
                                  text, re.M)
                self.assertIsNotNone(match, f"proto.hpp 里找不到 {name}")
                self.assertFalse(
                    _in_extern_c(text, match.start()),
                    f"{name} 在 proto.hpp 里被挪进了 extern \"C\" 块 —— "
                    f"符号形状变了,落地层的 C++ 定义要跟着改",
                )

    def test_landing_layer_defines_them_with_cpp_linkage(self) -> None:
        """落地层的定义必须与上游声明**同形**,且同样在 extern "C" 之外。"""
        text = _strip_comments((ROOT / self.LANDING_LAYER).read_text(encoding="utf-8"))
        for name in self.SYMBOLS:
            with self.subTest(symbol=name):
                pattern = re.compile(
                    r"^int[ \t]+" + re.escape(name) + r"[ \t]*\(([^;{)]*)\)[ \t]*\n?\{",
                    re.M,
                )
                match = pattern.search(text)
                self.assertIsNotNone(match, f"落地层里找不到 {name} 的定义")
                self.assertEqual(_param_types(match.group(1)), self._upstream(name)[1])
                self.assertFalse(
                    _in_extern_c(text, match.start()),
                    f"落地层的 {name} 落在了 extern \"C\" 块里 —— 那会给出未修饰符号,"
                    f"而上游要的是修饰过的名字",
                )

    def test_port_c_header_does_not_declare_them(self) -> None:
        """
        ★ 防回归 ★ 移植侧的 C 头里**不许**出现这两个名字。

        它们是 C++ 链接的符号;C 头里一份同名声明会读起来像"已经提供了",
        实际给出的是另一个符号(未修饰)。第一次链接就是栽在这上面。
        """
        text = _strip_comments((ROOT / self.PORT_HEADER).read_text(encoding="utf-8"))
        for name in self.SYMBOLS:
            with self.subTest(symbol=name):
                self.assertIsNone(
                    re.search(r"^[ \t]*int[ \t]+" + re.escape(name) + r"[ \t]*\(", text, re.M),
                    f"{self.PORT_HEADER} 里出现了 {name} 的 C 声明 —— "
                    f"那个符号与上游要的修饰名不是同一个,别把它加回来",
                )

    def test_port_provides_the_v_forms_instead(self) -> None:
        """替代物是 v 形式:落地层的转发就调它们,所以它们必须在。"""
        text = _strip_comments((ROOT / self.PORT_HEADER).read_text(encoding="utf-8"))
        for name in ("console_vprintf", "console_vsprintf"):
            with self.subTest(symbol=name):
                self.assertIsNotNone(
                    re.search(r"^[ \t]*int[ \t]+" + re.escape(name) + r"[ \t]*\(", text, re.M),
                    f"{self.PORT_HEADER} 里缺少 {name}",
                )

    def test_definition_matches_declaration(self) -> None:
        """
        console.c 里 v 形式的定义与头文件声明一致(形参顺序也是签名的一部分)。
        """
        header = _strip_comments((ROOT / self.PORT_HEADER).read_text(encoding="utf-8"))
        source = _strip_comments((ROOT / "arch/arm32/src/console.c").read_text(encoding="utf-8"))
        for name in ("console_vprintf", "console_vsprintf"):
            with self.subTest(symbol=name):
                # 声明以 `;` 收尾,定义以 `{` 开头 —— 两种都要吃
                pattern = re.compile(
                    r"^int[ \t]+" + re.escape(name) + r"[ \t]*\(([^;{)]*)\)[ \t]*(?:\n?\{|;)",
                    re.M,
                )
                declared = pattern.search(header)
                defined = pattern.search(source)
                self.assertIsNotNone(declared, f"{self.PORT_HEADER} 里找不到 {name} 的声明")
                self.assertIsNotNone(defined, f"console.c 里找不到 {name} 的定义")
                self.assertEqual(_param_types(defined.group(1)), _param_types(declared.group(1)))


if __name__ == "__main__":
    unittest.main()
