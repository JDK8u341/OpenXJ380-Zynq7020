"""内核堆(M4-3)的单元测试。

被测代码是 arch/arm32/src/heap.c —— 刻意做成**不含 MMIO/CP15**,
底层内存通过 grow 回调从外部要来,所以宿主编译器能直接编译运行。

**为什么值得测**:堆是那种"写错了也能跑很久"的组件。三个最危险的失败模式
在本实现里各自被一条判据盯住:

  1. **双重释放** —— 同一块被发两次,后果是两个使用者互相覆盖数据。
     判据:第二次必须返回 HEAP_ERR_DOUBLE_FREE,且**统计值不得变化**。
     只检查返回值是不够的 —— 一个"报了错但仍然改了统计"的实现同样有害。

  2. **块头被越界写** —— 少了魔数检查,链表会按垃圾 size 去改 next/prev,
     整条链表被拆散,而现场离真正的错误已经隔了很远。
     判据:手动破坏块头后 free 必须返回 HEAP_ERR_BAD_MAGIC。

  3. **相邻空闲未合并** —— 堆"看起来还能用",但最大可分配块越来越小。
     判据:交替分配/释放 4 轮之后,堆必须**完全恢复原样**
     (used=0 且最大空闲块等于全部空闲)。

参考:
  arch/arm32/include/arch/heap.h
  arch/arm32/src/heap.c
"""

from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

# krlibc 里被 heap.c 用到的符号。与 test_arm32_krlibc.py 同样的理由要加前缀:
# 宿主 UCRT 已经导出了 atoi/memcpy/... 直接链接会 duplicate symbol。
# 详见 tests/test_arm32_krlibc.py 里的说明。
KR_PREFIXED = ("memcpy", "memmove", "memset", "memcmp", "strlen", "strcmp", "strncmp",
               "strcpy", "strncpy", "strcat", "strchr", "strrchr", "strtok", "isdigit", "atoi")

HARNESS = r"""
int printf(const char *fmt, ...);

#include <arch/heap.h>
#include <krlibc.h>   /* memset —— harness 要用,而 heap.c 的实现依赖它 */

static int failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL %d: %s\n", __LINE__, #cond);                                              \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static u8 area[2048] __attribute__((aligned(8)));
static u8 area2[2048] __attribute__((aligned(8)));

int main(void)
{
    heap_t h;
    void  *a;
    void  *b;
    void  *c;
    u32    self;

    /* ---- 1. 模块自检必须整体通过 ---- */
    self = heap_selftest();
    if (self != 0) {
        printf("heap_selftest failed at check %u\n", self);
        return 1;
    }
    CHECK(heap_selftest() == 0); /* 确定性:重复跑结果相同 */

    /* ---- 2. 独立复核:对齐 ---- */
    CHECK(heap_init(&h, (uintptr_t)area, sizeof(area)) == HEAP_OK);

    a = heap_alloc(&h, 1u);
    b = heap_alloc(&h, 1u);
    c = heap_alloc(&h, 1u);
    CHECK(a && b && c);
    CHECK(((uintptr_t)a & 7u) == 0u);
    CHECK(((uintptr_t)b & 7u) == 0u);
    CHECK(((uintptr_t)c & 7u) == 0u);
    CHECK(a != b && b != c && a != c);

    CHECK(heap_free(&h, a) == HEAP_OK);
    CHECK(heap_free(&h, b) == HEAP_OK);
    CHECK(heap_free(&h, c) == HEAP_OK);
    CHECK(heap_check(&h) == HEAP_OK);

    /* ---- 3. 独立复核:失败的双重释放不能改变统计 ---- */
    a = heap_alloc(&h, 64u);
    CHECK(a != 0);
    CHECK(heap_free(&h, a) == HEAP_OK);
    {
        size_t fb = h.free_bytes;
        size_t ub = h.used_bytes;
        u32    fc = h.free_count;

        CHECK(heap_free(&h, a) == HEAP_ERR_DOUBLE_FREE);
        CHECK(h.free_bytes == fb);
        CHECK(h.used_bytes == ub);
        CHECK(h.free_count == fc); /* 失败的释放不该被计数 */
    }

    /* ---- 4. 独立复核:块头被破坏 ---- */
    a = heap_alloc(&h, 64u);
    CHECK(a != 0);
    {
        heap_block_t *hdr = (heap_block_t *)((uintptr_t)a - HEAP_HEADER_SIZE);
        u32 saved = hdr->magic;

        hdr->magic = 0x12345678u;
        CHECK(heap_free(&h, a) == HEAP_ERR_BAD_MAGIC);
        hdr->magic = saved;
    }
    CHECK(heap_free(&h, a) == HEAP_OK);
    CHECK(heap_check(&h) == HEAP_OK);

    /*
     * ---- 5. ★ 用真实的越界写来验证检测有效 ★ ----
     *
     * 上面那一步是手工改块头的,能证明检查存在;这一步更能说明问题:
     * 在负载上写超出申请长度的数据,看它是否真的会被发现。
     * (越界写会踩到下一块的块头,所以下一次 free 或 heap_check 应当报错。)
     */
    memset(area, 0, sizeof(area));
    CHECK(heap_init(&h, (uintptr_t)area, sizeof(area)) == HEAP_OK);
    a = heap_alloc(&h, 32u);
    b = heap_alloc(&h, 32u);
    CHECK(a && b);
    CHECK(heap_check(&h) == HEAP_OK);
    /* 故意越界:c 只有 32 字节,却写 64 字节,踩到 b 的块头 */
    memset(a, 0x77, 64u);
    CHECK(heap_check(&h) == HEAP_ERR_BAD_MAGIC || heap_check(&h) == HEAP_ERR_CORRUPT);

    /* ---- 6. 碎片压力:反复分配/释放后必须完全回收 ---- */
    memset(area2, 0, sizeof(area2));
    CHECK(heap_init(&h, (uintptr_t)area2, sizeof(area2)) == HEAP_OK);
    {
        void *p[8];
        u32   round;
        u32   i;

        for (round = 0; round < 8u; round++) {
            for (i = 0; i < 8u; i++) {
                p[i] = heap_alloc(&h, 24u + i * 16u);
                CHECK(p[i] != 0);
            }
            for (i = 0; i < 8u; i++) {
                CHECK(heap_free(&h, p[i]) == HEAP_OK);
            }
            CHECK(heap_check(&h) == HEAP_OK);
        }
        CHECK(h.used_bytes == 0u);
        CHECK(heap_largest_free(&h) == h.free_bytes); /* 又变回一整块 */
    }

    if (failures != 0) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }

    printf("heap: all checks passed\n");
    return 0;
}
"""


class Arm32HeapTests(unittest.TestCase):
    COMPILER_CANDIDATES = ("gcc", "cc", "clang")

    def test_kernel_heap(self) -> None:
        capture = {"capture_output": True, "text": True, "encoding": "utf-8", "errors": "replace"}

        with tempfile.TemporaryDirectory() as tmp:
            harness = Path(tmp) / "heap_test.c"
            binary = Path(tmp) / "heap_test"
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
                ]
                for fn in KR_PREFIXED:
                    command.append(f"-D{fn}=krtest_{fn}")
                command += [
                    str(harness),
                    str(ROOT / "arch/arm32/src/heap.c"),
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


if __name__ == "__main__":
    unittest.main()
