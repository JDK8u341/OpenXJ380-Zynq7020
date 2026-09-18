"""libc 堆接口(M4A-1.1a)的单元测试。

被测代码是 arch/arm32/src/kmalloc.c —— 它刻意只有"转发 + 补语义",
分配策略全在 heap.c(已由 test_arm32_heap.py 穷尽测)。所以这里测的
**不是分配器**,而是"把四百年历史的 C 语义接到 ARM 堆上"时最容易接错的几条:

  1. **free(NULL) 必须是 no-op** —— 上游代码里"可能为 NULL 的指针直接 free"
     到处都是。少了这一条,每次都会记一次假错误,而那个计数就没人信了。
     ⇒ 判据:free(NULL) 不得让 kmalloc_bad_free_count() 变化。

  2. **★ 释放失败必须可观测 ★** —— 这是整个 shim 存在的理由之一。
     heap.h 的原话是"释放失败必须能被调用方看见",而 libc 的 free 是 void。
     接成 void 就把信号扔了,于是"双重释放"这种错误会静默变成
     "堆慢慢坏掉,很久以后在无关的地方崩"。⇒ 判据:free() 一个已经释放过的
     指针,计数必须 +1,而堆本身不得被它弄坏(heap_check 仍然 OK)。

  3. **绑定之前必须"分配失败"而不是崩** —— 启动早期谁都可能先调到。
     ⇒ 判据:未绑定时 malloc/calloc/realloc 返回 NULL。

  4. **realloc 的 C 语义** —— realloc(NULL, n) 等价 malloc(n);
     扩容要保留内容;失败时原指针仍然有效。

参考:
  arch/arm32/include/arch/kmalloc.h(语义与"刻意没提供的两个")
  arch/arm32/include/arch/heap.h
  arch/arm32/src/kmalloc.c
"""

from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

# 与 test_arm32_heap.py 同样的理由:宿主 UCRT 已经导出了这些符号,
# 直接链接会 duplicate symbol。四个堆函数也要加前缀 —— 它们是本测试的**被测对象**。
KR_PREFIXED = ("memcpy", "memmove", "memset", "memcmp", "strlen", "strcmp", "strncmp",
               "strcpy", "strncpy", "strcat", "strchr", "strrchr", "strtok", "isdigit", "atoi")
HEAP_PREFIXED = ("malloc", "calloc", "realloc", "free")

HARNESS = r"""
int printf(const char *fmt, ...);

#include <arch/heap.h>
#include <arch/kmalloc.h>
#include <krlibc.h>

static int failures;
static int checks;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(cond)) {                                                                             \
            printf("FAIL %d: %s\n", __LINE__, #cond);                                              \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static u8 area[4096] __attribute__((aligned(8)));

int main(void)
{
    heap_t h;
    u8    *a;
    u8    *a2;
    u8    *b;
    u8    *c;
    u32    bad;

    /* ---- 1. 绑定之前:必须"分配失败",不是崩 ---- */
    CHECK(malloc(16u) == NULL);
    CHECK(calloc(4u, 4u) == NULL);
    CHECK(realloc(NULL, 16u) == NULL);

    /* 未绑定时释放一个非 NULL 指针 —— 这个指针不可能有合法来源,要计数 */
    bad = kmalloc_bad_free_count();
    free((void *)area);
    CHECK(kmalloc_bad_free_count() == bad + 1u);

    /* free(NULL) 无论绑没绑定都必须是 no-op */
    free(NULL);
    CHECK(kmalloc_bad_free_count() == bad + 1u);

    /* ---- 2. 绑定 ---- */
    CHECK(heap_init(&h, (uintptr_t)area, sizeof(area)) == HEAP_OK);
    kmalloc_set_heap(&h);
    bad = kmalloc_bad_free_count();

    /* ---- 3. 基本分配 ---- */
    a = (u8 *)malloc(100u);
    b = (u8 *)malloc(200u);
    CHECK(a != NULL);
    CHECK(b != NULL);
    CHECK(a != b);
    CHECK(((uintptr_t)a & 7u) == 0u);          /* 堆保证 8 字节对齐 */
    CHECK(kmalloc_bad_free_count() == bad);

    /* malloc(0) 返回**可释放的非 NULL 块**(沿用 ARM 堆的语义,不是 NULL) */
    c = (u8 *)malloc(0u);
    CHECK(c != NULL);
    free(c);
    CHECK(kmalloc_bad_free_count() == bad);

    /* ---- 4. 写满负载不能破坏块头 ---- */
    memset(a, 0xAA, 100u);
    memset(b, 0xBB, 200u);
    CHECK(heap_check(&h) == HEAP_OK);

    /* ---- 5. calloc 必须清零,且乘法溢出要给 NULL ---- */
    c = (u8 *)calloc(16u, 8u);
    CHECK(c != NULL);
    {
        u32 i;
        u32 zero = 1u;
        for (i = 0; i < 128u; i++) { if (c[i] != 0u) { zero = 0u; break; } }
        CHECK(zero);
    }
    free(c);
    CHECK(kmalloc_bad_free_count() == bad);

    /* count * size 溢出 -> NULL(而不是一个看起来成功的小块) */
    CHECK(calloc(((size_t)-1 / 2u) + 1u, 2u) == NULL);
    CHECK(kmalloc_bad_free_count() == bad);

    /* ---- 6. realloc 的 C 语义 ---- */
    c = (u8 *)realloc(NULL, 64u);              /* 等价 malloc(64) */
    CHECK(c != NULL);
    memset(c, 0x5A, 64u);

    c = (u8 *)realloc(c, 256u);                /* 扩容要保留前 64 字节 */
    CHECK(c != NULL);
    {
        u32 i;
        u32 kept = 1u;
        for (i = 0; i < 64u; i++) { if (c[i] != 0x5Au) { kept = 0u; break; } }
        CHECK(kept);
    }

    CHECK(realloc(c, 32u) == (void *)c);       /* 够大就原块返回,不做就地收缩 */
    free(c);
    CHECK(kmalloc_bad_free_count() == bad);

    /* realloc 一个非法指针:必须失败,且不得把它 free 掉(否则计数会涨) */
    CHECK(realloc((void *)area, 32u) == NULL);
    CHECK(kmalloc_bad_free_count() == bad);

    /* ---- 7. ★ 双重释放必须被计数,且堆不能被它弄坏 ★ ---- */
    /*
     * ⚠ 这一节第一版是**假的**:它只 free 了一次就断言"计数没变" ——
     *   标题写着双重释放,代码里根本没有第二次释放。判据必须真的触发
     *   它要测的那件事(本项目的坑 43 就是这个形状)。
     */
    a2 = a;
    free(a);
    a = NULL;
    CHECK(kmalloc_bad_free_count() == bad);        /* 第一次释放是合法的 */
    CHECK(heap_check(&h) == HEAP_OK);

    free(a2);                                      /* ★ 故意再释放一次 */
    CHECK(kmalloc_bad_free_count() == bad + 1u);   /* ★ 必须被计数 */
    CHECK(heap_check(&h) == HEAP_OK);              /* ★ 且堆不能被它弄坏 */

    /* 释放一个堆外指针(比如栈上的 heap_t):同样要计数,不能静默 */
    bad = kmalloc_bad_free_count();
    free((void *)&h);
    CHECK(kmalloc_bad_free_count() == bad + 1u);
    CHECK(heap_check(&h) == HEAP_OK);

    /* free(NULL) 在任何情况下都是 no-op,不能被算成错误 */
    free(NULL);
    CHECK(kmalloc_bad_free_count() == bad + 1u);
    bad = kmalloc_bad_free_count();

    /* ---- 8. 收尾:全部释放之后堆必须完全恢复 ---- */
    free(b);
    b = NULL;
    CHECK(kmalloc_bad_free_count() == bad);
    CHECK(heap_check(&h) == HEAP_OK);
    CHECK(h.used_bytes == 0u);

    /* ---- 9. 解绑之后又回到"分配失败" ---- */
    kmalloc_set_heap(NULL);
    CHECK(malloc(16u) == NULL);

    printf("checks=%d\n", checks);
    if (failures == 0) {
        printf("all checks passed\n");
    } else {
        printf("%d check(s) failed\n", failures);
    }

    return failures == 0 ? 0 : 1;
}
"""


class Arm32KmallocTests(unittest.TestCase):
    COMPILER_CANDIDATES = ("gcc", "cc", "clang")
    CAPTURE = {"capture_output": True, "text": True, "encoding": "utf-8", "errors": "replace"}

    # 把"释放失败被吞掉"这个变异体的源码写出来用 —— 见 test_..._has_discrimination
    BAD_FREE_SWALLOWED = (
        "    if (heap_free(g_heap, ptr) != HEAP_OK) {\n"
        "        g_bad_free++;\n"
        "    }\n",
        "    (void)heap_free(g_heap, ptr); /* 变异体:把失败吞掉 */\n",
    )

    def _compile(self, tmp: Path, harness: Path, kmalloc_src: Path) -> Path | None:
        """用宿主编译器把 harness + heap.c + kmalloc.c + krlibc.c 编成一个可执行文件。

        四个堆函数要加 -D 前缀:宿主 UCRT 已经导出了同名符号。
        返回可执行文件路径,或 None(附失败详情)。
        """
        binary = Path(tmp) / "kmalloc_test"
        attempts: list[str] = []
        for compiler in self.COMPILER_CANDIDATES:
            command = [
                compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-I", str(ROOT / "arch/arm32/include"),
            ]
            for fn in KR_PREFIXED + HEAP_PREFIXED:
                command.append(f"-D{fn}=krtest_{fn}")
            command += [
                str(harness),
                str(ROOT / "arch/arm32/src/heap.c"),
                str(kmalloc_src),
                str(ROOT / "arch/arm32/src/krlibc.c"),
                "-o", str(binary),
            ]
            try:
                result = subprocess.run(command, **self.CAPTURE)
            except FileNotFoundError:
                attempts.append(f"{compiler}: not found")
                continue
            if result.returncode == 0:
                return binary
            detail = (result.stderr or "").strip().replace("\n", " ")[:400]
            attempts.append(f"{compiler}: rc={result.returncode} {detail}")
        self.fail("no working host C compiler found:\n  " + "\n  ".join(attempts))

    def test_libc_heap_interface(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            harness = Path(tmp) / "kmalloc_test.c"
            harness.write_text(HARNESS, encoding="utf-8")

            binary = self._compile(Path(tmp), harness, ROOT / "arch/arm32/src/kmalloc.c")
            run = subprocess.run([str(binary)], **self.CAPTURE)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("all checks passed", run.stdout)

            # "没有 FAIL" 也可能是"没跑几条" —— 把实际执行的判据数钉住,
            # 免得将来有人误删整段用例而测试依然全绿(本项目的坑 43)。
            executed = [line for line in run.stdout.splitlines() if line.startswith("checks=")]
            self.assertEqual(len(executed), 1, f"harness 没报判据数:\n{run.stdout}")
            count = int(executed[0].split("=", 1)[1])
            self.assertGreaterEqual(count, 30, f"只执行了 {count} 条判据,疑似用例被削弱")

    def test_the_bad_free_counter_has_discrimination(self) -> None:
        """★ 破坏性对照:把"释放失败"吞掉之后,这个测试**必须**失败。

        没有这一条的话,一个恒为 0 的计数、或者一段根本没接到 heap_free 上的
        包装,都能让上面那条用例全绿 —— 本项目的坑 43 就是这个形状
        ("判据成立,但这一相什么都没发生")。

        变异的正是这一层存在的理由:libc 的 free 没有返回值,
        所以"双重释放/野指针"是**唯一**只能从这个计数看出来的错误。
        """
        src = (ROOT / "arch/arm32/src/kmalloc.c").read_text(encoding="utf-8")
        original, mutant = self.BAD_FREE_SWALLOWED
        self.assertIn(original, src, "变异点找不到了 —— 这条对照用例本身已失效")
        broken = src.replace(original, mutant, 1)
        self.assertNotEqual(broken, src, "变异没有生效")

        with tempfile.TemporaryDirectory() as tmp:
            harness = Path(tmp) / "kmalloc_test.c"
            harness.write_text(HARNESS, encoding="utf-8")
            mutant_src = Path(tmp) / "kmalloc_mutant.c"
            mutant_src.write_text(broken, encoding="utf-8")

            binary = self._compile(Path(tmp), harness, mutant_src)
            run = subprocess.run([str(binary)], **self.CAPTURE)

            self.assertNotEqual(
                run.returncode, 0,
                "把释放失败吞掉之后测试竟然还是绿的 —— 这个计数判据没有区分能力:\n"
                + run.stdout,
            )
            self.assertIn("FAIL", run.stdout, f"没有 FAIL 行:\n{run.stdout}")


if __name__ == "__main__":
    unittest.main()
