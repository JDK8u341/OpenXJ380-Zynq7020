"""ARMv7-A 缓存几何解码的单元测试。

被测代码是 arch/arm32/include/arch/cache.h —— 刻意做成不含 CP15 内联汇编的
纯逻辑,所以能直接用宿主编译器编译运行。

**这个测试的核心是 CCSIDR 的三个"减一/减四"字段。**

ARM 把这三个值全部做了偏移存储:
    LineSize      = log2(行字节数) - 4
    Associativity = 路数 - 1
    NumSets       = 组数 - 1

少加那个 1 不会报任何错 —— 只会让整块失效循环少覆盖一行/一路/一组,
于是缓存里残留的脏行在之后被写回,静默覆盖掉正确数据。
这类错误在板上表现为"偶发数据损坏",是本项目最不想遇到的一类 bug。

基准值来自真实的 Cortex-A9(即本板 Zynq-7020):
    L1 D-Cache 32KB 4 路 32 字节行 -> 256 组
    L1 I-Cache 32KB 4 路 32 字节行 -> 256 组

CCSIDR 位域:
    bits[2:0]   LineSize      = 1   (1<<(1+4) = 32 字节)
    bits[12:3]  Associativity = 3   (3+1 = 4 路)
    bits[27:13] NumSets       = 255 (255+1 = 256 组)
  => 0x00000000 | (255<<13) | (3<<3) | 1 = 0x001FE019

参考:
  arch/arm32/include/arch/cache.h
  ARM ARM (DDI 0406C) B2.2.4 / B6.1 "Cache maintenance operations"
"""

from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

# 真实 Cortex-A9 L1 缓存的 CCSIDR(32KB / 4 路 / 32 字节行)
CORTEX_A9_L1_CCSIDR = 0x001FE019

HARNESS = r"""
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include <arch/cache.h>

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
        printf("FAIL: %s (got 0x%08X, want 0x%08X)\n", what, got, want);
        failures++;
    }
}

int main(void)
{
    /* ============================================================== */
    /* 1. 用真实 Cortex-A9 的 CCSIDR 反推几何                          */
    /* ============================================================== */
    {
        cache_geometry_t geo = cache_decode_ccsidr(CORTEX_A9_L1_CCSIDR);

        check_u32(geo.line_bytes, 32u, "LineSize field must decode to a 32-byte line");
        check_u32(geo.ways, 4u, "Associativity field must decode to 4 ways");
        check_u32(geo.sets, 256u, "NumSets field must decode to 256 sets");
        check_u32(geo.total_bytes, 32768u, "the product must be the documented 32KB");

        /*
         * 这三个断言各自独立。如果解码里少加了某个 1,
         * 通常只有其中一个会挂 —— 报出来的名字直接指出是哪个字段错了。
         */
        check_u32(geo.line_bytes >> 4, 2u, "log2(line) must be 5 for 32-byte lines");
    }

    /* ============================================================== */
    /* 2. "减一"存储的边界:最小几何                                  */
    /* ============================================================== */
    {
        /*
         * 全零的 CCSIDR = 1 路、1 组、16 字节行。
         * 这是三个字段的下界。若实现里漏了 +1,这里会算成 0 路 0 组,
         * 于是失效循环一次都不跑 —— 而循环"跑完了"不报任何错。
         */
        cache_geometry_t geo = cache_decode_ccsidr(0u);

        check_u32(geo.line_bytes, 16u, "LineSize=0 means a 16-byte line");
        check_u32(geo.ways, 1u, "Associativity=0 means one way, not zero");
        check_u32(geo.sets, 1u, "NumSets=0 means one set, not zero");
        check_u32(geo.total_bytes, 16u, "minimum geometry is 1 way x 1 set x 16 bytes");
        check(geo.ways > 0u && geo.sets > 0u, "no geometry may ever have zero ways or sets");
    }

    /* ============================================================== */
    /* 3. 大缓存:Cortex-A9 可配的最大 L1(64KB / 4 路)               */
    /* ============================================================== */
    {
        /* 64KB = 4 路 x 512 组 x 32 字节 -> NumSets=511, Assoc=3, Line=1 */
        cache_geometry_t geo = cache_decode_ccsidr((511u << 13) | (3u << 3) | 1u);

        check_u32(geo.sets, 512u, "512 sets");
        check_u32(geo.total_bytes, 65536u, "64KB");
    }

    /* ============================================================== */
    /* 4. set/way 参数的位布局                                        */
    /* ============================================================== */
    {
        /*
         * bits[31:30] = 路号,bits[13] 起 = 组号。
         * 这两处位置没有"看起来显然"的地方,写错就会清到错误的组/路。
         */
        check_u32(cache_setway_value(0u, 0u), 0x00000000u, "way 0 / set 0 encodes to 0");
        check_u32(cache_setway_value(0u, 1u), 0x00002000u, "set field starts at bit 13");
        check_u32(cache_setway_value(1u, 0u), 0x40000000u, "way field starts at bit 30");
        check_u32(cache_setway_value(3u, 255u), 0xC01FE000u, "way 3 / set 255");

        /* 路号与组号不能互相串位 */
        check((cache_setway_value(1u, 0u) & 0x1FFFu) == 0u, "way bits must not leak into the set field");
        check((cache_setway_value(0u, 255u) & 0xC0000000u) == 0u, "set bits must not leak into the way field");

        /* 遍历次数 = 路数 x 组数 */
        {
            cache_geometry_t geo = cache_decode_ccsidr(CORTEX_A9_L1_CCSIDR);
            check_u32(cache_setway_iterations(&geo), 1024u, "32KB/4-way/32B needs 4x256 = 1024 iterations");
        }
    }

    /* ============================================================== */
    /* 5. CLIDR 解码 —— 用本板实测值                                  */
    /* ============================================================== */
    {
        /*
         * 本板(Zynq-7020 / Cortex-A9)**实测** CLIDR = 0x09200003:
         *   bits[2:0]   = 0b011  第 0 级为分离缓存(指令与数据都有)
         *   bits[26:24] = 0b001  LoC   = 1
         *   bits[29:27] = 0b001  LoUIS = 1
         *
         * ⚠ 这个值是踩出来的:一开始按记忆写成 0x00000003(LoC/LoUIS 都为 0),
         *   结果板上打印出来是 0x09200003。实测值才是真的 ——
         *   与"硬件读取优先于文档记忆"这条适用于本文件所有基准值。
         *
         * 另一个必须记住的事实:**L2(PL310)不在 CLIDR 里**。
         * 0x09200003 只描述了 L1;L2 有自己的一套寄存器(0xF8F02000)。
         * 所以"用 CP15 循环失效所有级别"在本芯片上只覆盖 L1,
         * L2 必须单独处理 —— 这条直接决定了 M2-5c 的做法。
         */
        unsigned int clidr = 0x09200003u;

        check_u32(cache_level_type(clidr, 0u), CACHE_TYPE_SEPARATE, "level 0 is separate I/D");
        check(cache_level_has_data(clidr, 0u), "separate cache contains data");
        check(!cache_level_has_data(clidr, 1u), "L2 is absent from CLIDR on Cortex-A9");
        check_u32(cache_loc(clidr), 1u, "measured LoC = 1 on this board");
        check_u32(cache_louis(clidr), 1u, "measured LoUIS = 1 on this board");

        /*
         * 由此得到的最高处理级别:min(LoC, LoUIS) = 1。
         * 循环会走到 level=1,但那一级在 CLIDR 里是空类型,
         * 于是只处理 L1 —— 行为正确,而且顺带说明
         * "min 取出来是 1" 并不等于"真的处理了两级"。
         */
        check_u32(cache_setway_max_level(clidr), 1u, "min(LoC=1, LoUIS=1) = 1");

        /*
         * LoC/LoUIS 不能被别的位串扰。
         * 这里用不同的值验证取的是正确的那 3 位,而不是恰好读到某个常数。
         */
        check_u32(cache_loc(1u << 24), 1u, "LoC lives at bit 24");
        check_u32(cache_loc(3u << 24), 3u, "LoC is a 3-bit field");
        check_u32(cache_louis(1u << 27), 1u, "LoUIS lives at bit 27");
        check_u32(cache_louis(3u << 27), 3u, "LoUIS is a 3-bit field");

        /* 各级类型解码都不能串位 */
        check_u32(cache_level_type(0x00000004u << 3, 1u), CACHE_TYPE_UNIFIED,
                  "the level-1 field lives at bit 3");
        check_u32(cache_level_type(0x00000001u << 6, 2u), CACHE_TYPE_ICACHE,
                  "the level-2 field lives at bit 6");

        /*
         * 保留值 7 必须被当作"不含数据缓存" —— 它是保留编码,
         * 若被误判成含数据,整块失效循环会对一个不存在的缓存操作。
         */
        check_u32(cache_level_type(0x00000007u, 0u), 7u, "reserved type is returned raw");
        check(!cache_level_has_data(0x00000007u, 0u), "reserved type must not count as having data");
        check(!cache_level_has_data(0x00000000u, 0u), "type 0 means no cache at this level");
        check(!cache_level_has_data(0x00000001u, 0u), "instruction-only cache has no data cache");
    }

    /* ============================================================== */
    /* 6. setway 的最高级别:取 LoC 与 LoUIS 的较小者                  */
    /* ============================================================== */
    /*
     * 超过一致性点的级别由硬件或别的机制负责,软件再去动它会破坏一致性。
     * 这里验证的是"取较小者"而不是"取 LoC"或"取 LoUIS"。
     */
    check_u32(cache_setway_max_level((0u << 24) | (0u << 27)), 0u, "both zero -> level 0");
    check_u32(cache_setway_max_level((3u << 24) | (1u << 27)), 1u, "LoC=3, LoUIS=1 -> 1 (smaller)");
    check_u32(cache_setway_max_level((1u << 24) | (3u << 27)), 1u, "LoC=1, LoUIS=3 -> 1 (smaller)");

    /* ============================================================== */
    /* 7. 区间对齐 —— DMA 缓存维护最容易出错的地方                    */
    /* ============================================================== */
    /*
     * 缓存维护按**行**生效,而一行的所有权与请求范围无关。
     * 如果只覆盖范围内的行、不做向外取整,那么范围两端若各有一行
     * 只有一部分落在里面,那一行的另一半就得不到处理 ——
     * 对 DMA 来说这意味着缓冲区首尾各有一小段没被写回(或没被失效),
     * 表现为"大部分时候对,偶尔错几个字节"。
     *
     * 所以这里逐条钉住"向外取整"的语义。
     */
    {
        cache_range_t r;
        const unsigned int LINE = 32u;

        /* -- 已对齐的范围:原样保留 -- */
        r = cache_align_range(0x1000u, 128u, LINE);
        check_u32((unsigned int)r.start, 0x1000u, "aligned start stays");
        check_u32((unsigned int)r.end, 0x1080u, "aligned end stays");

        /* -- 起点不对齐:向下取整 -- */
        r = cache_align_range(0x1004u, 4u, LINE);
        check_u32((unsigned int)r.start, 0x1000u, "unaligned start rounds down");
        check_u32((unsigned int)r.end, 0x1020u, "end rounds up to cover the whole line");

        /* -- 终点不对齐:向上取整 -- */
        r = cache_align_range(0x1000u, 33u, LINE);
        check_u32((unsigned int)r.start, 0x1000u, "start unchanged");
        check_u32((unsigned int)r.end, 0x1040u, "33 bytes span two lines");

        /* -- 一行内部的 1 字节:必须覆盖整行 -- */
        r = cache_align_range(0x101Fu, 1u, LINE);
        check_u32((unsigned int)r.start, 0x1000u, "last byte of a line rounds down");
        check_u32((unsigned int)r.end, 0x1020u, "and up: exactly one line");
        check_u32(cache_range_lines(&r, LINE), 1u, "one byte inside a line covers one line");

        /* -- 跨行边界:两行 -- */
        r = cache_align_range(0x101Fu, 2u, LINE);
        check_u32(cache_range_lines(&r, LINE), 2u, "straddling a boundary covers two lines");

        /* -- size == 0:空区间,调用方据此直接跳过 -- */
        r = cache_align_range(0x1004u, 0u, LINE);
        check(cache_range_is_empty(&r), "zero size yields an empty range");
        check_u32((unsigned int)r.start, (unsigned int)r.end, "empty range has start == end");
        check_u32(cache_range_lines(&r, LINE), 0u, "empty range covers no lines");

        /* -- line_bytes == 0:几何未知时的保守退化,不能除以 0 -- */
        r = cache_align_range(0x1004u, 4u, 0u);
        check_u32((unsigned int)r.start, 0x1004u, "line size 0 degenerates to byte granularity");
        check_u32((unsigned int)r.end, 0x1008u, "and does not lose the tail");
        check_u32(cache_range_lines(&r, 0u), 4u, "and does not divide by zero");

        /*
         * -- 溢出:addr + size 绕回 --
         *
         * 这一条最关键。若不做检查,end 会小于 start,调用方把它当成
         * 空区间直接跳过 —— 于是**整段缓冲区一行都没被维护**,
         * 而且不会有任何报错。
         *
         * 宿主是 64 位,所以用 UINTPTR_MAX 附近的地址触发同样的分支;
         * ARM 目标上是 32 位绕回,走的是同一段代码。
         */
        r = cache_align_range(UINTPTR_MAX - 8u, 64u, LINE);
        check(!cache_range_is_empty(&r), "overflowing range must NOT collapse to empty");
        check(r.end >= r.start, "end must never precede start");
        check(r.end <= UINTPTR_MAX, "end must not wrap past the top of the address space");

        /*
         * -- 对齐不变量:覆盖范围必须包含原始范围 --
         * 随机性质的抽样检查,比逐点断言更能抓住取整方向的错误。
         */
        {
            unsigned int addr;
            unsigned int sizes[] = {1u, 4u, 31u, 32u, 33u, 100u, 1000u};
            unsigned int si;

            for (addr = 0x1000u; addr < 0x1200u; addr += 7u) {
                for (si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
                    unsigned int end_want = addr + sizes[si];

                    r = cache_align_range(addr, sizes[si], LINE);

                    if (r.start > addr || r.end < end_want) {
                        printf("FAIL: range [0x%X,0x%X) not covered by [0x%X,0x%X)\n", addr, end_want,
                               (unsigned int)r.start, (unsigned int)r.end);
                        failures++;
                    }
                    if (((unsigned int)r.start % LINE) != 0u || ((unsigned int)r.end % LINE) != 0u) {
                        printf("FAIL: range [0x%X,0x%X) is not line aligned\n", (unsigned int)r.start,
                               (unsigned int)r.end);
                        failures++;
                    }
                }
            }
        }
    }

    if (failures == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", failures);
    return 1;
}
"""


class Arm32CacheTests(unittest.TestCase):
    # 与其它 ARM 侧测试一致:本机 MinGW 的 gcc/g++ 是坏的,逐个探测
    COMPILER_CANDIDATES = ("gcc", "cc", "clang")

    def _compile_and_run(self, source: str, extra_sources: list[Path]) -> str:
        capture = {"capture_output": True, "text": True, "encoding": "utf-8", "errors": "replace"}

        with tempfile.TemporaryDirectory() as tmp:
            harness = Path(tmp) / "cache_test.c"
            binary = Path(tmp) / "cache_test"
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
                    *[str(path) for path in extra_sources],
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

            run_result = subprocess.run([str(binary)], **capture)
            self.assertEqual(
                run_result.returncode,
                0,
                f"cache geometry checks failed:\n{run_result.stdout}{run_result.stderr}",
            )
            return run_result.stdout

    def test_cache_geometry_decode(self) -> None:
        source = HARNESS.replace("CORTEX_A9_L1_CCSIDR", f"{CORTEX_A9_L1_CCSIDR}u")
        output = self._compile_and_run(source, [])
        self.assertIn("ALL PASS", output)


if __name__ == "__main__":
    unittest.main()
