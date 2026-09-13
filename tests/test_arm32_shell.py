"""串口命令通道的行编辑与解析的单元测试。

被测代码是 arch/arm32/include/arch/shell.h —— 纯逻辑,无 MMIO,
所以能直接用宿主编译器编译运行。

**为什么值得测**:行编辑看起来是"显然"的代码,但它有一条容易被忽略的
安全性要求 ——

    缓冲区满之后到达的字符**不能被静默丢掉**。那样提交的是一条
    "被截断的命令",而**前缀很可能恰好是另一条合法命令**。
    例如将来若有 `peek2`,缓冲区只装得下 `peek`,于是执行了完全不同的
    操作,而且不会有任何报错。

所以下面专门钉:溢出过的行必须带 overflow 标志、必须能被调用方识别出来。
(src/shell.c 收到该标志时拒绝执行,而不是执行截断版。)

另外测两处"看起来显然但会写错"的地方:
  - 空行按退格**不能回显** —— 否则终端会吃掉提示符;
  - 命令匹配必须**精确**,不做前缀 —— 调试通道里"敲少一个字母却执行了
    另一条命令"比"命令不存在"危险得多。

参考:
  arch/arm32/include/arch/shell.h
  arch/arm32/src/shell.c
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

#include <arch/shell.h>

static int failures;

static void check(int condition, const char *what)
{
    if (!condition) {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

static void check_u32(unsigned int got, unsigned int want, const char *what)
{
    if (got != want) {
        printf("FAIL: %s (got %u, want %u)\n", what, got, want);
        failures++;
    }
}

/* 不用 strcpy:Windows 上的 clang 以 -Werror 报它已废弃 */
static void set_buf(char *dst, const char *src)
{
    while ((*dst++ = *src++) != 0) {
    }
}

static void check_str(const char *got, const char *want, const char *what)
{
    if (strcmp(got, want) != 0) {
        printf("FAIL: %s (got \"%s\", want \"%s\")\n", what, got, want);
        failures++;
    }
}

/* 把一串字符喂进编辑器,返回最后一次的动作 */
static shell_act_t feed_all(shell_line_t *line, const char *text)
{
    shell_act_t last = SHELL_ACT_IGNORED;
    const char *p = text;

    while (*p != '\0') {
        last = shell_line_feed(line, *p);
        p++;
    }
    return last;
}

/* ================= 假的命令表 ================= */
static void run_a(const char *args) { (void)args; }
static void run_b(const char *args) { (void)args; }

static const shell_cmd_t CMDS[] = {
    {"help", "help text", run_a},
    {"peek", "peek text", run_b},
    {"peek2", "peek2 text", run_a},
};
#define CMDS_N (sizeof(CMDS) / sizeof(CMDS[0]))

int main(void)
{
    shell_line_t line;
    const char  *name;
    const char  *args;

    /* ============================================================== */
    /* 1. 基本输入与提交                                              */
    /* ============================================================== */
    shell_line_reset(&line);
    check_u32(feed_all(&line, "dump"), SHELL_ACT_ECHO, "plain characters are echoed");
    check_u32(line.len, 4u, "four characters buffered");
    check_str(line.buf, "dump", "buffer content");
    check(!line.overflow, "no overflow yet");

    check_u32(shell_line_feed(&line, '\r'), SHELL_ACT_SUBMIT, "CR submits");
    check_str(line.buf, "dump", "buffer survives until the caller resets it");

    /* \n 也当回车 —— 终端通常发 \r\n,两个都要认 */
    shell_line_reset(&line);
    feed_all(&line, "ver");
    check_u32(shell_line_feed(&line, '\n'), SHELL_ACT_SUBMIT, "LF also submits");

    /* ============================================================== */
    /* 2. 退格                                                        */
    /* ============================================================== */
    shell_line_reset(&line);
    feed_all(&line, "dumX");
    check_u32(shell_line_feed(&line, '\b'), SHELL_ACT_ERASE, "backspace erases");
    check_u32(line.len, 3u, "length shrinks");
    check_str(line.buf, "dum", "character removed");

    /* DEL(0x7F)与 BS(0x08)都要认:不同终端发的不一样 */
    check_u32(shell_line_feed(&line, 0x7F), SHELL_ACT_ERASE, "DEL erases too");
    check_u32(line.len, 2u, "length shrinks again");

    /*
     * 空行上按退格必须**不回显**。
     * 回显 "\b \b" 会让终端把提示符 "> " 擦掉一格,看起来像提示符缺了一块。
     */
    shell_line_reset(&line);
    check_u32(shell_line_feed(&line, '\b'), SHELL_ACT_IGNORED, "backspace on empty line is ignored");
    check_u32(line.len, 0u, "still empty");

    /* ============================================================== */
    /* 3. 取消                                                        */
    /* ============================================================== */
    shell_line_reset(&line);
    feed_all(&line, "half-typed");
    check_u32(shell_line_feed(&line, 0x03), SHELL_ACT_CANCEL, "Ctrl-C cancels");
    check_u32(line.len, 0u, "line cleared by cancel");
    check_str(line.buf, "", "buffer cleared by cancel");

    /* ============================================================== */
    /* 4. 不可打印字符一律忽略,不能把终端搞乱                        */
    /* ============================================================== */
    shell_line_reset(&line);
    check_u32(shell_line_feed(&line, 0x1B), SHELL_ACT_IGNORED, "ESC ignored");
    check_u32(shell_line_feed(&line, 0x01), SHELL_ACT_IGNORED, "Ctrl-A ignored");
    check_u32(shell_line_feed(&line, (char)0x80), SHELL_ACT_IGNORED, "high-bit byte ignored");
    check_u32(line.len, 0u, "nothing buffered");

    /* 可打印范围的边界要放行 */
    shell_line_reset(&line);
    check_u32(shell_line_feed(&line, 0x20), SHELL_ACT_ECHO, "space is printable");
    check_u32(shell_line_feed(&line, 0x7E), SHELL_ACT_ECHO, "'~' is printable");
    check_u32(line.len, 2u, "both buffered");

    /* ============================================================== */
    /* 5. ★ 溢出:必须留下标志,不能被静默截断 ★                      */
    /* ============================================================== */
    /*
     * 这一条是本文件存在的主要理由。
     *
     * 缓冲区满之后到达的字符若被静默丢掉,提交的就是一条被截断的命令 ——
     * 而长命令的前缀很可能恰好是另一条**合法**命令。将来若有 `peek2`,
     * 用户敲 `peek2 0x1000` 却只执行了 `peek`,不会有任何报错。
     */
    shell_line_reset(&line);
    {
        unsigned int i;
        for (i = 0; i < SHELL_LINE_MAX + 20u; i++) {
            shell_line_feed(&line, 'x');
        }
    }
    check(line.overflow, "overflow flag must be set when the buffer fills");
    check_u32(line.len, SHELL_LINE_MAX - 1u, "buffer never exceeds its capacity");
    check_u32((unsigned int)strlen(line.buf), SHELL_LINE_MAX - 1u, "buffer stays NUL terminated");

    /* 溢出之后提交,标志必须还在 —— 调用方要靠它决定拒绝 */
    check_u32(shell_line_feed(&line, '\r'), SHELL_ACT_SUBMIT, "overflowing line still submits");
    check(line.overflow, "overflow flag survives until the caller resets the line");

    /* 复位之后标志要清掉,否则下一行会被误拒 */
    shell_line_reset(&line);
    check(!line.overflow, "reset clears the overflow flag");
    feed_all(&line, "help");
    check(!line.overflow, "a short line after a reset is clean");

    /* ============================================================== */
    /* 6. shell_split:就地拆出命令名与参数                            */
    /* ============================================================== */
    {
        char buf[64];
        const char *ret;

        /* 普通:命令 + 一个参数 */
        set_buf(buf, "peek 41200000");
        ret = shell_split(buf, &name);
        check_str(name, "peek", "command name");
        check_str(ret, "41200000", "args point past the command");

        /* 行首空白要跳过 */
        set_buf(buf, "   dump");
        ret = shell_split(buf, &name);
        check_str(name, "dump", "leading spaces are skipped");
        check_str(ret, "", "no args -> empty string, never NULL");

        /* 多个空格只当一次分隔 */
        set_buf(buf, "peek    0x1000");
        ret = shell_split(buf, &name);
        check_str(name, "peek", "name extracted");
        check_str(ret, "0x1000", "extra spaces skipped");

        /* 空行:name 指向空串,调用方据此忽略 */
        set_buf(buf, "");
        ret = shell_split(buf, &name);
        check_str(name, "", "empty line yields an empty name");
        check_str(ret, "", "and empty args");

        /* 全是空白也当空行 */
        set_buf(buf, "    ");
        ret = shell_split(buf, &name);
        check_str(name, "", "whitespace-only line yields an empty name");

        /* 只有命令没有参数 */
        set_buf(buf, "help");
        ret = shell_split(buf, &name);
        check_str(name, "help", "bare command");
        check_str(ret, "", "empty args");
    }

    /* ============================================================== */
    /* 7. shell_lookup:精确匹配,不做前缀                             */
    /* ============================================================== */
    check(shell_lookup(CMDS, CMDS_N, "help") == &CMDS[0], "exact match finds the command");
    check(shell_lookup(CMDS, CMDS_N, "peek") == &CMDS[1], "peek is distinct from peek2");
    check(shell_lookup(CMDS, CMDS_N, "peek2") == &CMDS[2], "peek2 found");

    /*
     * 前缀绝不能匹配 —— 这是本通道里"宁可报错也不要猜"的另一处体现:
     * 敲少一个字母却执行了另一条命令,比"命令不存在"危险得多。
     */
    check(shell_lookup(CMDS, CMDS_N, "pe") == NULL, "a prefix must NOT match");
    check(shell_lookup(CMDS, CMDS_N, "peekx") == NULL, "a longer string must NOT match");
    check(shell_lookup(CMDS, CMDS_N, "HELP") == NULL, "matching is case sensitive");

    /* 空名字与 NULL 不能匹配到任何东西 */
    check(shell_lookup(CMDS, CMDS_N, "") == NULL, "empty name matches nothing");
    check(shell_lookup(CMDS, CMDS_N, NULL) == NULL, "NULL name matches nothing");
    check(shell_lookup(NULL, 0u, "help") == NULL, "NULL table is tolerated");

    /* ============================================================== */
    /* 8. 端到端:敲一行 -> 拆 -> 查表                                 */
    /* ============================================================== */
    {
        char buf[64];

        shell_line_reset(&line);
        feed_all(&line, "peek 0x41200000");
        check(!line.overflow, "the line fits");
        shell_line_feed(&line, '\r');

        set_buf(buf, line.buf);
        args = shell_split(buf, &name);
        check(shell_lookup(CMDS, CMDS_N, name) == &CMDS[1], "dispatch finds peek");
        check_str(args, "0x41200000", "argument survives the round trip");
    }

    if (failures == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", failures);
    return 1;
}
"""


class Arm32ShellTests(unittest.TestCase):
    COMPILER_CANDIDATES = ("gcc", "cc", "clang")

    def _compile_and_run(self, source: str) -> str:
        capture = {"capture_output": True, "text": True, "encoding": "utf-8", "errors": "replace"}

        with tempfile.TemporaryDirectory() as tmp:
            harness = Path(tmp) / "shell_test.c"
            binary = Path(tmp) / "shell_test"
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
                detail = (result.stderr or "").strip().replace("\n", " ")[:300]
                attempts.append(f"{compiler}: rc={result.returncode} {detail}")
            else:
                self.fail("no working host C compiler found:\n  " + "\n  ".join(attempts))

            run = subprocess.run([str(binary)], **capture)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            return run.stdout

    def test_line_editor_and_dispatch(self) -> None:
        output = self._compile_and_run(HARNESS)
        self.assertIn("ALL PASS", output)


if __name__ == "__main__":
    unittest.main()
