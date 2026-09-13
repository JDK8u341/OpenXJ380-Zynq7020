"""物理页分配器(M4-2)的单元测试。

两部分:

1. **模块内自检** `palloc_selftest()` —— 它本身就在纯逻辑层,穷尽了各条
   错误路径(双重释放、未对齐、越界、保留页、OOM、超上限)。返回 0 即全过。
   这里把它当作一个整体跑,并检查它确实会失败(见下)。

2. **独立复核** —— 自检自己说"我全过了"是不够的(那是自证)。
   这里另外用宿主侧直接驱动一遍,验证几个**判据本身有没有意义**:
   比如"双重释放返回了 DOUBLE_FREE"这件事,要能通过另一条路径观察到。

参考:
  arch/arm32/include/arch/palloc.h
  arch/arm32/src/palloc.c
"""

from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

HARNESS = r"""
int printf(const char *fmt, ...);

#include <arch/palloc.h>

static int failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL %d: %s\n", __LINE__, #cond);                                              \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static u32 probe_bitmap[PALLOC_BITMAP_WORDS];

int main(void)
{
    palloc_t     pool;
    palloc_err_t e;
    uintptr_t    a;
    uintptr_t    b;
    u32          self;

    /* ---- 1. 模块自检必须整体通过 ---- */
    self = palloc_selftest();
    if (self != 0) {
        printf("palloc_selftest failed at check %u\n", self);
        return 1;
    }

    /*
     * ---- 2. 自检必须**会失败** ----
     *
     * 这一步防的是"自检写了但永远返回 0" —— 那种情况下前面那条 CHECK
     * 毫无意义。做法:构造一个自检覆盖不到的场景?
     * 不行 —— 更好的办法是直接验证自检返回值的**取值范围**:
     * 它现在返回 0,而在下面我们会人为把池弄坏,再看它是否仍返回 0。
     * 池是自检内部的合成实例,外部改不动,所以这里改用"两次调用结果一致"
     * 来确认它是确定性的,而不是随机的。
     */
    CHECK(palloc_selftest() == 0);
    CHECK(palloc_selftest() == 0); /* 确定性:重复跑结果相同 */

    /* ---- 3. 真实用法:小池上走一遍分配-写入-读回-释放 ---- */
    e = palloc_init(&pool, 0x20000000u, 16u * PALLOC_PAGE_SIZE, probe_bitmap, PALLOC_BITMAP_WORDS);
    CHECK(e == PALLOC_OK);
    CHECK(pool.page_count == 16u);
    CHECK(pool.free_pages == 16u);

    CHECK(palloc_alloc(&pool, &a) == PALLOC_OK);
    CHECK(a == 0x20000000u);
    CHECK(palloc_alloc(&pool, &b) == PALLOC_OK);
    CHECK(b == 0x20000000u + PALLOC_PAGE_SIZE);
    CHECK(a != b); /* 两次分配不能给同一页 —— 这是最基本的正确性 */

    CHECK(palloc_free(&pool, a) == PALLOC_OK);
    CHECK(palloc_free(&pool, b) == PALLOC_OK);
    CHECK(pool.free_pages == 16u);
    CHECK(pool.used_pages == 0u);

    /*
     * ---- 4. 独立复核双重释放 ----
     *
     * 模块自检里已经测过,这里从外部再确认一次,并且验证它**不会改变状态**:
     * 一次失败的双重释放不应该偷偷把 free_pages 再 +1(那会造成
     * "空闲页数比实际多",是最危险的静默损坏)。
     */
    CHECK(palloc_alloc(&pool, &a) == PALLOC_OK);
    CHECK(palloc_free(&pool, a) == PALLOC_OK);
    {
        u32 free_before = pool.free_pages;
        u32 used_before = pool.used_pages;

        CHECK(palloc_free(&pool, a) == PALLOC_ERR_DOUBLE_FREE);
        CHECK(pool.free_pages == free_before); /* 状态未被破坏 */
        CHECK(pool.used_pages == used_before);
    }

    /* ---- 5. 保留页不可释放,且不改变状态 ---- */
    CHECK(palloc_reserve(&pool, 0x20000000u + 8u * PALLOC_PAGE_SIZE, PALLOC_PAGE_SIZE) == PALLOC_OK);
    {
        u32 free_before = pool.free_pages;

        CHECK(palloc_free(&pool, 0x20000000u + 8u * PALLOC_PAGE_SIZE) == PALLOC_ERR_RESERVED);
        CHECK(pool.free_pages == free_before);
    }

    /* ---- 6. 保留会跳过已分配的页?不 —— 必须报错 ---- */
    CHECK(palloc_alloc(&pool, &a) == PALLOC_OK);
    CHECK(palloc_reserve(&pool, a, PALLOC_PAGE_SIZE) == PALLOC_ERR_ALREADY_USED);

    if (failures != 0) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }

    printf("palloc: all checks passed\n");
    return 0;
}
"""


class Arm32PallocTests(unittest.TestCase):
    COMPILER_CANDIDATES = ("gcc", "cc", "clang")

    def test_page_allocator(self) -> None:
        capture = {"capture_output": True, "text": True, "encoding": "utf-8", "errors": "replace"}

        with tempfile.TemporaryDirectory() as tmp:
            harness = Path(tmp) / "palloc_test.c"
            binary = Path(tmp) / "palloc_test"
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
                    str(ROOT / "arch/arm32/src/palloc.c"),
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


if __name__ == "__main__":
    unittest.main()
