"""krlibc 子集(M4-1)的单元测试。

两部分:

1. **纯函数行为** —— 把 arch/arm32/src/krlibc.c 和宿主编译器编在一起跑。
   这些函数刻意不含任何 MMIO/CP15,所以能这样验证。

2. **errno 数值与旧 XJ380 逐项一致** —— 这一条是这个文件存在的更重要理由。
   arch/arm32/include/errno.h 是 include/errno.h 的**副本**,而"靠自觉保持
   两份同步"是不可接受的:漂移的表现是"某个驱动返回了错误的 errno",
   极难定位。这里解析两个文件、逐条比对,一致才算过。

   这不是洁癖:B5 要求两个架构的驱动接口兼容,errno 是其中最基础的一层。

参考:
  arch/arm32/include/krlibc.h
  arch/arm32/src/krlibc.c
  arch/arm32/include/errno.h
  include/errno.h            (旧 XJ380 侧,数值的唯一权威)
"""

from __future__ import annotations

import re
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

# 刻意不 include <stdio.h> / <string.h>:
#   - <string.h> 会与我们要测的 strcpy/strlen 等重名,而且在 Windows 的 clang 下
#     strcpy 被标为 deprecated,配 -Werror 会直接编不过(本项目踩过);
#   - 手工声明 printf 两行就够,不用把整个宿主头文件树拖进来。
# 被测的纯函数(与 M4-1 的搬运范围一致,见 arch/arm32/include/krlibc.h)。
# 测试时统一加 krtest_ 前缀,理由见下面编译命令处的注释。
KR_PURE_FUNCS = (
    "memcpy",
    "memmove",
    "memset",
    "memcmp",
    "strlen",
    "strcmp",
    "strncmp",
    "strcpy",
    "strncpy",
    "strcat",
    "strchr",
    "strrchr",
    "strtok",
    "isdigit",
    "atoi",
)

HARNESS = r"""
int printf(const char *fmt, ...);

#include <arch/errno.h>
#include <krlibc.h>

static int failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL %d: %s\n", __LINE__, #cond);                                              \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

int main(void)
{
    char buf[32];
    char ov[8];

    /* ---- 内存 ---- */
    {
        char src[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        char dst[8] = {0};

        CHECK(memcpy(dst, src, 8) == dst);
        CHECK(memcmp(dst, src, 8) == 0);
        dst[3] = 99;
        CHECK(memcmp(dst, src, 8) != 0);
        CHECK(memcmp(dst, src, 3) == 0); /* 只比前 3 字节,应当仍相等 */

        memset(dst, 0xAB, 8);
        CHECK((unsigned char)dst[0] == 0xAB && (unsigned char)dst[7] == 0xAB);
    }

    /*
     * memmove 的重叠语义 —— 这是最容易写错的一处。
     * 目标在源之后且重叠时必须反向拷贝,否则前几个字节会把后几个覆盖掉。
     */
    {
        char overlap[11] = "0123456789"; /* 10 字符 + NUL,少一字节编译器会报 initializer too long */

        memmove(overlap + 2, overlap, 8); /* 向后搬,区间重叠 */
        CHECK(overlap[2] == '0');
        CHECK(overlap[9] == '7');
    }
    {
        char overlap[11] = "0123456789"; /* 10 字符 + NUL,少一字节编译器会报 initializer too long */

        memmove(overlap, overlap + 2, 8); /* 向前搬,区间重叠 */
        CHECK(overlap[0] == '2');
        CHECK(overlap[7] == '9');
    }

    /* ---- 字符串 ---- */
    CHECK(strlen("") == 0);
    CHECK(strlen("abc") == 3);

    CHECK(strcmp("abc", "abc") == 0);
    CHECK(strcmp("abc", "abd") < 0);
    CHECK(strcmp("abd", "abc") > 0);
    CHECK(strcmp("abc", "abcd") < 0);

    CHECK(strncmp("abcXX", "abcYY", 3) == 0);
    CHECK(strncmp("abcXX", "abcYY", 4) != 0);

    CHECK(strcpy(buf, "hello") == buf);
    CHECK(strcmp(buf, "hello") == 0);

    /*
     * strncpy 的两条反直觉语义,标准明确规定,也是文件系统里最危险的一处:
     *   源短于 n -> 剩余全部补 0;源不短于 n -> 目标不以 0 结尾。
     * 漏掉补 0 会让目标尾部留着上一次的内容 —— 在 fs 里就是"读到别的文件的数据"。
     */
    memset(ov, 'X', sizeof(ov));
    strncpy(ov, "ab", 5);
    CHECK(ov[0] == 'a' && ov[1] == 'b');
    CHECK(ov[2] == 0 && ov[3] == 0 && ov[4] == 0); /* 必须补 0 */
    CHECK(ov[5] == 'X');                           /* 第 n 字节之后不动 */

    memset(ov, 'X', sizeof(ov));
    strncpy(ov, "abcdefgh", 3);
    CHECK(ov[0] == 'a' && ov[1] == 'b' && ov[2] == 'c'); /* 不补 0,这是标准行为 */

    CHECK(strcat(strcpy(buf, "ab"), "cd") != 0);
    CHECK(strcmp(buf, "abcd") == 0);

    CHECK(strchr("hello", 'l') != 0);
    CHECK(strchr("hello", 'l')[0] == 'l');
    CHECK(strchr("hello", 'z') == 0);
    /* strchr 找 '\0' 必须返回指向结尾的指针,而不是 NULL */
    CHECK(strchr("hello", 0) != 0);
    CHECK(strchr("hello", 0)[0] == 0);

    CHECK(strrchr("hello", 'l') != 0);
    CHECK(strrchr("hello", 'l')[0] == 'l');
    CHECK(strrchr("hello", 'l')[1] == 'o'); /* 必须是**最后**一个 l */
    CHECK(strrchr("hello", 'z') == 0);

    /* ---- strtok ---- */
    {
        char  text[32];
        char *t;

        strcpy(text, "a,bb,,ccc");
        t = strtok(text, ",");
        CHECK(t != 0 && strcmp(t, "a") == 0);

        t = strtok(0, ","); /* 传 NULL 继续 */
        CHECK(t != 0 && strcmp(t, "bb") == 0);

        /* 连续分隔符不产生空 token */
        t = strtok(0, ",");
        CHECK(t != 0 && strcmp(t, "ccc") == 0);

        t = strtok(0, ",");
        CHECK(t == 0);
    }

    /* ---- 数字 ---- */
    CHECK(isdigit('0') && isdigit('9'));
    CHECK(!isdigit('/') && !isdigit(':'));
    CHECK(!isdigit('a'));

    CHECK(atoi("0") == 0);
    CHECK(atoi("123") == 123);
    CHECK(atoi("-45") == -45);
    CHECK(atoi("+7") == 7);
    CHECK(atoi("  42abc") == 42); /* 前导空白 + 尾随非数字 */
    CHECK(atoi("abc") == 0);

    /* ---- errno 存储存在且初值为 EOK ---- */
    CHECK(errno == EOK);
    errno = EINVAL;
    CHECK(errno == EINVAL);
    errno = EOK;

    if (failures != 0) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }

    printf("krlibc: all checks passed\n");
    return 0;
}
"""


def parse_errno(path: Path) -> dict[str, str]:
    """从 errno.h 里抽出 `#define E... <值>`,值按原样字符串保留。

    只比对**数值型**的定义。像 `#define EWOULDBLOCK EAGAIN` 这种别名,
    原样比较别名指向的名字 —— 那同样是兼容性的一部分。
    """
    out: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        m = re.match(r"\s*#define\s+(E[A-Z0-9_]+)\s+(\S+)", line)
        if m:
            out[m.group(1)] = m.group(2)
    return out


class Arm32KrlibcTests(unittest.TestCase):
    COMPILER_CANDIDATES = ("gcc", "cc", "clang")

    def test_pure_functions(self) -> None:
        capture = {"capture_output": True, "text": True, "encoding": "utf-8", "errors": "replace"}

        with tempfile.TemporaryDirectory() as tmp:
            harness = Path(tmp) / "krlibc_test.c"
            binary = Path(tmp) / "krlibc_test"
            harness.write_text(HARNESS, encoding="utf-8")

            attempts: list[str] = []
            for compiler in self.COMPILER_CANDIDATES:
                command = [
                    compiler,
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fno-builtin",
                    "-I",
                    str(ROOT / "arch/arm32/include"),
                ]

                # 给被测符号加前缀,避免与宿主 libc 撞名。
                #
                # 为什么必须这么做:Windows 的 UCRT 已经导出了 atoi/memcpy/strlen...
                # 直接链接会报 duplicate symbol(clang 实测 `atoi >>> defined at
                # libucrt.lib(atox.obj)`)。靠 -Wl 的"允许多重定义"能绕过,但那个
                # 开关各平台拼写不同(GNU ld 是 --allow-multiple-definition,
                # lld-link 是 /force:multiple),测试会变成依赖工具链。
                #
                # 改名是编译器无关的,而且有个附带好处:它**彻底阻止**编译器用内建
                # 替掉我们的实现 —— 这正是本测试想避免的(要测的是我们的代码)。
                # 真正的符号名由 ARM 侧的构建(-nostdlib)负责验证。
                for fn in KR_PURE_FUNCS:
                    command.append(f"-D{fn}=krtest_{fn}")

                command += [
                    str(harness),
                    str(ROOT / "arch/arm32/src/krlibc.c"),
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
                detail = (result.stderr or "").strip().replace("\n", " ")[:400]
                attempts.append(f"{compiler}: rc={result.returncode} {detail}")
            else:
                self.fail("no working host C compiler found:\n  " + "\n  ".join(attempts))

            run = subprocess.run([str(binary)], **capture)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("all checks passed", run.stdout)

    def test_errno_matches_legacy_xj380(self) -> None:
        """ARM 侧的 errno 必须与旧 XJ380 的 include/errno.h 逐项一致。

        这是 B5"驱动接口兼容"在最基础一层上的机械保证 ——
        靠自觉同步两份副本,迟早会漂移,而漂移的表现是
        "某个驱动返回了错误的 errno",极难定位。
        """
        legacy = parse_errno(ROOT / "include" / "errno.h")
        arm = parse_errno(ROOT / "arch/arm32" / "include" / "arch" / "errno.h")

        self.assertGreater(len(legacy), 100, "旧 errno.h 解析结果太少,解析器可能坏了")
        self.assertGreater(len(arm), 100, "ARM errno.h 解析结果太少,解析器可能坏了")

        missing = sorted(set(legacy) - set(arm))
        extra = sorted(set(arm) - set(legacy))
        self.assertEqual(missing, [], f"ARM 侧缺少这些 errno: {missing}")
        self.assertEqual(extra, [], f"ARM 侧多出这些 errno(旧 XJ380 里没有): {extra}")

        mismatched = {k: (legacy[k], arm[k]) for k in legacy if legacy[k] != arm[k]}
        self.assertEqual(mismatched, {}, f"errno 数值不一致(名字: 旧值 / ARM 值): {mismatched}")


if __name__ == "__main__":
    unittest.main()
