"""内核虚拟内存细粒度映射(M4-4)的单元测试。

被测代码是 arch/arm32/src/vmap.c —— 页表就是普通内存,L1 表与 L2 表池
都由调用方提供,所以宿主编译器能直接编译它。

**为什么值得测**:本模块的核心操作是"把 1MB 恒等映射段就地拆成 L2 表,
且保持每一页的映射与属性不变"。这一步有两条各自独立的失败方式:

  1. **少写一页** —— 那一页会在 TLB 失效之后立刻变成 translation fault,
     而故障点离"我刚才拆了段"这个原因隔得很远(可能是几毫秒后的某次访存);
  2. **属性搬运而不是翻译** —— 段与小页的属性位布局完全不同
     (S 位在段里是 bit16,在小页里 bit16 属于**物理地址**),
     直接搬过去会让每一页的物理地址凭空多出 0x10000。

自检对**全部 256 页**逐页核对了物理地址与属性,正是为了同时钉住这两条。

参考:
  arch/arm32/include/arch/vmap.h
  arch/arm32/src/vmap.c
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

#include <arch/mmu.h>
#include <arch/vmap.h>
#include <krlibc.h>

static int failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL %d: %s\n", __LINE__, #cond);                                              \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

/* 见 vmap.c 的说明:16KB 对齐是 TTBR0 的硬件要求,与本测试的逻辑无关 */
static u32 l1[4096] __attribute__((aligned(8192)));
static u32 l2[2 * VMAP_L2_ENTRIES] __attribute__((aligned(1024)));
static vmap_t v;

/* 合成物理地址:宿主指针是 64 位,塞不进 32 位描述符 */
#define L2_PA 0x30000000u

int main(void)
{
    vmap_attr_t normal;
    u32         self;
    u32         i;

    /* ---- 1. 模块自检必须整体通过 ---- */
    self = vmap_selftest();
    if (self != 0) {
        printf("vmap_selftest failed at check %u\n", self);
        return 1;
    }
    CHECK(vmap_selftest() == 0); /* 确定性 */

    /* ---- 2. 独立复核:拆段之后逐页核对 ---- */
    normal = vmap_attr_normal();

    memset(l1, 0, sizeof(l1));
    memset(l2, 0, sizeof(l2));

    l1[vmap_l1_index(0x50000000u)] = mmu_section_descriptor(0x50000000u, 0x00015DE6u);

    CHECK(vmap_init(&v, l1, l2, L2_PA, 2u, 0x50000000u, 0x50200000u) == VMAP_OK);
    CHECK(vmap_split_section(&v, 0x50000000u) == VMAP_OK);
    CHECK((l1[vmap_l1_index(0x50000000u)] & 0x3u) == MMU_L1_TYPE_PAGE_TABLE);

    for (i = 0; i < VMAP_L2_ENTRIES; i++) {
        u32 addr = 0x50000000u + (i << 12);
        u32 got  = vmap_lookup(&v, addr);

        CHECK(got != 0u);
        /* ★ 物理地址必须逐页相等 —— 属性搬运的 bug 会让这里多出 0x10000 ★ */
        CHECK((got & ~0xFFFu) == addr);
    }

    /* ---- 3. 独立复核:属性是翻译的,不是搬的 ---- */
    {
        u32 got = vmap_lookup(&v, 0x50000000u);

        /*
         * 0x15DE6 按**段**的位布局解码出来是 (ap3=3, tex=5, c=0, b=1, s=1, xn=0)
         * —— 注意 tex=5、c=0,不是"看起来像"的 tex=1/c=1。
         * 所以期望值必须由**小页构造函数**从这组参数重建,而不是沿用段的值。
         */
        u32 expect = mmu_l2_small_page_attr(3u, 5u, 0u, 1u, true, false);
        u32 naive  = 0x00015DE6u & 0xFFFu; /* 直接搬运低 12 位的结果 */

        CHECK((got & 0xFFFu) == expect);
        CHECK((got & 0xFFFu) != naive); /* ★ 搬运会得到另一个值 —— 这就是那个 bug ★ */
        CHECK((got & ~0xFFFu) == 0x50000000u); /* 而且物理地址没被属性污染 */
    }

    /* ---- 4. 越界写入被拒绝,且区间外那一页没被碰过 ---- */
    CHECK(vmap_map(&v, 0x50200000u, 0x1000u, &normal) == VMAP_ERR_OUT_OF_RANGE);
    CHECK(vmap_map(&v, 0x4FFFF000u, 0x1000u, &normal) == VMAP_ERR_OUT_OF_RANGE);
    CHECK(l1[vmap_l1_index(0x50200000u)] == 0u);
    CHECK(l1[vmap_l1_index(0x4FF00000u)] == 0u);

    /* ---- 5. 换映射的流程是 unmap -> map ---- */
    CHECK(vmap_unmap(&v, 0x50000000u) == VMAP_OK);
    CHECK(vmap_map(&v, 0x50000000u, 0x60000000u, &normal) == VMAP_OK);
    CHECK((vmap_lookup(&v, 0x50000000u) & ~0xFFFu) == 0x60000000u);
    /* 重复映射被拒,且原值不变 */
    CHECK(vmap_map(&v, 0x50000000u, 0x70000000u, &normal) == VMAP_ERR_ALREADY);
    CHECK((vmap_lookup(&v, 0x50000000u) & ~0xFFFu) == 0x60000000u);

    /* 邻居不受影响 */
    CHECK((vmap_lookup(&v, 0x50001000u) & ~0xFFFu) == 0x50001000u);

    /* ---- 6. 改属性之后 XN 位真的变了(小页的 XN 在 bit0) ---- */
    {
        vmap_attr_t ro = vmap_attr_ro_xn();
        u32 before = vmap_lookup(&v, 0x50000000u);

        CHECK(vmap_set_attr(&v, 0x50000000u, &ro) == VMAP_OK);
        CHECK((vmap_lookup(&v, 0x50000000u) & 0x1u) == 0x1u);
        CHECK((before & 0x1u) == 0u);
        /* 物理地址不变 */
        CHECK((vmap_lookup(&v, 0x50000000u) & ~0xFFFu) == 0x60000000u);
    }

    if (failures != 0) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }

    printf("vmap: all checks passed\n");
    return 0;
}
"""


class Arm32VmapTests(unittest.TestCase):
    COMPILER_CANDIDATES = ("gcc", "cc", "clang")

    def test_fine_grained_mapping(self) -> None:
        capture = {"capture_output": True, "text": True, "encoding": "utf-8", "errors": "replace"}

        with tempfile.TemporaryDirectory() as tmp:
            harness = Path(tmp) / "vmap_test.c"
            binary = Path(tmp) / "vmap_test"
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
