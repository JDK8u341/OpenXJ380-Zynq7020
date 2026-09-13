"""内核栈池 + guard page(M4-5)的单元测试。

被测代码是 arch/arm32/src/kstack.c —— 页表交给 vmap(M4-4)操作,
TLB 维护通过调用方给的**钩子**外置,所以宿主编译器能直接编译它。

**为什么值得测**:本模块要保证的是一条"静默失效"性质 ——
栈溢出必须当场被抓住。它有三种各自独立、而且**都不会报错**的失败方式:

  1. **guard 页其实是映射着的** —— 拆段会把整段 256 页都填成恒等映射,
     guard 页也在其中。"guard 不可访问"不是默认成立的,必须显式做一次。
     漏掉的后果是溢出静默踩进相邻内存;
  2. **guard 落在未拆的段上** —— 栈区起点贴着 1MB 段边界时,guard 页属于
     **上一个**段,那一段还没被拆过,`vmap_unmap()` 会返回 IS_SECTION。
     必须先拆段。自检用 `stat_guard_split` 证明这条分支**真的被走到过**
     (不然那项检查是空的);
  3. **改完页表忘了失效 TLB** —— TLB 里留着拆段之前的**段表项**,
     guard 页照样能访问,而页表里看上去完全正确。

第 3 条是本模块把 TLB 维护做成**函数指针**的理由:钩子被调用过、
且区间覆盖了 guard 页,这两件事在这里是**可断言**的,而不是"靠人记得"。

另外测试了探针的地址算术:整段写入必须装得进一页,否则"无 guard"
那组对照会写穿 guard 页冲进下面那个槽的栈区。

参考:
  arch/arm32/include/arch/kstack.h
  arch/arm32/src/kstack.c
"""

from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

# 与 test_arm32_krlibc.py 同样的理由:宿主 UCRT 已导出这些符号
KR_PREFIXED = ("memcpy", "memmove", "memset", "memcmp", "strlen", "strcmp", "strncmp",
               "strcpy", "strncpy", "strcat", "strchr", "strrchr", "strtok", "isdigit", "atoi")

HARNESS = r"""
int printf(const char *fmt, ...);

#include <arch/kstack.h>
#include <arch/mmu.h>
#include <arch/vmap.h>
#include <krlibc.h>

static int failures;

/*
 * TLB 钩子的桩。真实实现在 arch/arm32/src/kstack_hw.c(CP15,宿主编不了),
 * 这里替身的同时顺带计数 —— "改完页表忘了刷 TLB"于是是一条可断言的性质,
 * 而不是"靠人记得"。见 test_arm32_kstack.py 的说明。
 */
static u32 g_flush_calls;

static void flush_stub(u32 va_begin, u32 va_end)
{
    (void)va_begin;
    (void)va_end;
    g_flush_calls++;
}

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL %d: %s\n", __LINE__, #cond);                                              \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

int main(void)
{
    u32 self;

    /* ---- 1. 模块自检必须整体通过 ---- */
    self = kstack_selftest();
    if (self != 0) {
        printf("kstack_selftest failed at check %u\n", self);
        return 1;
    }
    CHECK(kstack_selftest() == 0); /* 确定性:自检不依赖上一次的残留状态 */

    /* ---- 2. 独立复核:探针的地址算术 ---- */
    {
        /*
         * 用**真实的宿主缓冲区**做核对。base 取"缓冲区末尾再往后一个字",
         * 于是写入全部落在缓冲区之内 —— 探针在真实板上写的是 guard 页,
         * 但算术是同一套。
         */
        static u32 buf[KSTACK_OVERFLOW_WORDS + 4];
        uintptr_t  base = (uintptr_t)&buf[KSTACK_OVERFLOW_WORDS];
        u32        i;
        u32        n;
        u32        distinct = 0;
        u32        seen[KSTACK_OVERFLOW_WORDS];

        /* 第一个字落在 base-4 */
        CHECK(kstack_overflow_addr(base, 0) == base - 4u);

        /* 单调递减,步长 4 */
        for (i = 1; i < KSTACK_OVERFLOW_WORDS; i++) {
            CHECK(kstack_overflow_addr(base, i) == kstack_overflow_addr(base, i - 1u) - 4u);
        }

        /* ★ 整段必须装得进一页 ★ 否则"无 guard"的对照组会写穿 guard 页 */
        CHECK(kstack_overflow_addr(base, KSTACK_OVERFLOW_WORDS - 1u) >= base - KSTACK_PAGE_SIZE);
        CHECK(KSTACK_OVERFLOW_WORDS * 4u <= KSTACK_PAGE_SIZE);

        /* 图案必须两两不同 —— 否则"写进去了"和"本来就是那个值"分不开 */
        for (i = 0; i < KSTACK_OVERFLOW_WORDS; i++) {
            u32 j;
            u32 w = kstack_overflow_word(i);

            seen[i] = w;
            for (j = 0; j < i; j++) {
                CHECK(seen[j] != w);
            }
            distinct++;
        }
        CHECK(distinct == KSTACK_OVERFLOW_WORDS);

        /* 写下去、读回来 */
        CHECK(kstack_overflow_visible(base) == false); /* 还没写 */
        n = kstack_overflow_write(base);
        CHECK(n == KSTACK_OVERFLOW_WORDS);
        CHECK(kstack_overflow_visible(base) == true);

        /* 写进去的确实是探针图案,不是碰巧 */
        for (i = 0; i < KSTACK_OVERFLOW_WORDS; i++) {
            CHECK(buf[KSTACK_OVERFLOW_WORDS - 1u - i] == kstack_overflow_word(i));
        }
        /* 缓冲区里没被碰到的部分仍然干净 */
        CHECK(buf[KSTACK_OVERFLOW_WORDS] == 0u);
    }

    /* ---- 3. 独立复核:槽位几何与池的切分 ---- */
    {
        static vmap_t  vm;
        static u32     l1[4096] __attribute__((aligned(8192)));
        static u32     l2[8 * VMAP_L2_ENTRIES] __attribute__((aligned(1024)));
        static u32     bm[KSTACK_BITMAP_WORDS];
        kstack_pool_t  pool;
        kstack_t       s0;
        kstack_t       s1;
        u32            i;
        u32            flush_before;

        memset(l1, 0, sizeof(l1));
        memset(l2, 0, sizeof(l2));
        memset(bm, 0, sizeof(bm));

        /* 预置成段映射,模拟真实的 DDR 恒等映射(否则拆段路径根本没被走到) */
        for (i = 0; i < 8u; i++) {
            u32 base = 0x60000000u + (i << MMU_SECTION_SHIFT);
            l1[vmap_l1_index(base)] = mmu_section_descriptor(base, 0x00015DE6u);
        }

        CHECK(vmap_init(&vm, l1, l2, 0x30000000u, 8u, 0x60000000u, 0x60800000u) == VMAP_OK);

        /* 4 个槽 x (2 页栈 + 1 页 guard) = 12 页 */
        CHECK(kstack_pool_init(&pool, &vm, 0x60000000u, 12u * KSTACK_PAGE_SIZE, 2u,
                               KSTACK_GUARD_UNMAPPED, bm, KSTACK_BITMAP_WORDS,
                               flush_stub) == KSTACK_OK);
        CHECK(pool.slot_count == 4u);
        CHECK(pool.slot_pages == 3u);

        flush_before = g_flush_calls;
        CHECK(kstack_alloc(&pool, &s0) == KSTACK_OK);
        /* ★ 钩子必须被调用过 —— 这是 guard 不会静默失效的前提 ★ */
        CHECK(g_flush_calls > flush_before);
        CHECK(pool.stat_flush > 0u);
        CHECK(s0.guard == 0x60000000u);
        CHECK(s0.base == 0x60001000u);
        CHECK(s0.top == 0x60003000u);
        CHECK((s0.top & 7u) == 0u);

        /* guard 真的不可访问 */
        CHECK(vmap_lookup(&vm, s0.guard) == 0u);
        /* 栈区两页都是恒等映射 */
        CHECK((vmap_lookup(&vm, s0.base) & ~0xFFFu) == s0.base);
        CHECK((vmap_lookup(&vm, s0.top - KSTACK_PAGE_SIZE) & ~0xFFFu) == s0.top - KSTACK_PAGE_SIZE);

        CHECK(kstack_alloc(&pool, &s1) == KSTACK_OK);
        CHECK(s1.guard == s0.top);          /* ★ 无缝相邻 ★ */
        CHECK(s1.base == s0.top + 0x1000u);
        CHECK(vmap_lookup(&vm, s1.guard) == 0u);

        /* A/B:关掉 guard 之后那一页必须变成可访问 */
        CHECK(kstack_guard_disable(&pool, &s0) == KSTACK_OK);
        CHECK((vmap_lookup(&vm, s0.guard) & ~0xFFFu) == s0.guard);
        CHECK(kstack_guard_enable(&pool, &s0) == KSTACK_OK);
        CHECK(vmap_lookup(&vm, s0.guard) == 0u);

        /* 首次适配:释放槽 0 之后应当拿回槽 0 */
        CHECK(kstack_free(&pool, &s0) == KSTACK_OK);
        CHECK(kstack_alloc(&pool, &s0) == KSTACK_OK);
        CHECK(s0.slot == 0u);

        /* 越界的槽返回 0 而不是野地址 */
        CHECK(kstack_slot_base(&pool, 4u) == 0u);
        CHECK(kstack_slot_base(&pool, 0xFFFFFFFFu) == 0u);

        /* 池的起点不是 1MB 对齐时也必须能工作(真实板级就是这样) */
        CHECK(kstack_pool_init(&pool, &vm, 0x60001000u, 12u * KSTACK_PAGE_SIZE, 2u,
                               KSTACK_GUARD_UNMAPPED, bm, KSTACK_BITMAP_WORDS,
                               flush_stub) == KSTACK_OK);
        CHECK(kstack_alloc(&pool, &s0) == KSTACK_OK);
        CHECK(s0.guard == 0x60001000u);
        CHECK(kstack_free(&pool, &s0) == KSTACK_OK);
    }

    if (failures != 0) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }

    printf("kstack: all checks passed\n");
    return 0;
}
"""


class Arm32KstackTests(unittest.TestCase):
    COMPILER_CANDIDATES = ("gcc", "cc", "clang")

    def test_kernel_stack_pool(self) -> None:
        capture = {"capture_output": True, "text": True, "encoding": "utf-8", "errors": "replace"}

        with tempfile.TemporaryDirectory() as tmp:
            harness = Path(tmp) / "kstack_test.c"
            binary = Path(tmp) / "kstack_test"
            harness.write_text(HARNESS, encoding="utf-8")

            attempts: list[str] = []
            for compiler in self.COMPILER_CANDIDATES:
                command = [
                    compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT / "arch/arm32/include"),
                ]
                for fn in KR_PREFIXED:
                    command.append(f"-D{fn}=krtest_{fn}")
                command += [
                    str(harness),
                    str(ROOT / "arch/arm32/src/kstack.c"),
                    str(ROOT / "arch/arm32/src/vmap.c"),
                    str(ROOT / "arch/arm32/src/mmu.c"),
                    str(ROOT / "arch/arm32/src/krlibc.c"),
                    "-o", str(binary),
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
