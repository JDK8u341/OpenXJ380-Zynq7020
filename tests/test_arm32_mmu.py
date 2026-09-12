"""ARMv7-A 短描述符页表定义的单元测试。

被测代码是 arch/arm32/include/arch/mmu.h + arch/arm32/src/mmu.c ——
刻意做成不含 CP15/MMIO 的纯逻辑,所以能直接用宿主编译器编译运行。

**这个测试最核心的价值是交叉验证位域理解**:
Xilinx BSP 的 xil_mmu.h 里有 6 个在 Zynq-7000 上用了十几年的属性常量
(0x15DE6 / 0x16DEA / 0x11DE2 / 0xC02 / 0xC06 / 0)。
本测试用自己写的 mmu_l1_section_attr() 从 AP/TEX/C/B/Domain/S/XN
这些**语义参数**重新算出这些常量,要求逐位相等。

如果我对 AP[1:0] 在 bit11:10、TEX 在 bit14:12、AP[2] 在 bit15、S 在 bit16
这些位置的理解有任何一处偏差,这 5 个常量就不可能同时对上。
这比"我自己写、我自己断言"有意义得多 —— 它拿一个独立来源当真值。

第二个重点是**一级与二级描述符的位域不对称**。两者的 AP/TEX/AP[2]/S
全在不同位置,而且二级小页的 bit0 是 XN 而不是类型位。
抄错不会有任何编译错误,只会让那页的权限和缓存属性变得莫名其妙,
在板上表现为偶发数据损坏 —— 所以这里显式断言两边不共用位域。

参考:
  arch/arm32/include/arch/mmu.h
  AMD/Xilinx standalone BSP (MIT): arm/cortexa9/xil_mmu.h,
  arm/cortexa9/gcc/translation_table.S
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

#include <arch/mmu.h>

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

/* 前缀匹配。只比首字母不够 —— "PS peripherals" 与 "PL (AXI GP)" 都是 'P' */
static int starts_with(const char *text, const char *prefix)
{
    while (*prefix != '\0') {
        if (*text != *prefix) {
            return 0;
        }
        text++;
        prefix++;
    }
    return 1;
}

static void check_region(u32 addr, const char *want, const char *what)
{
    const char *got = mmu_region_name_for(addr);

    if (!starts_with(got, want)) {
        printf("FAIL: %s (addr 0x%08X -> \"%s\", want \"%s\")\n", what, addr, got, want);
        failures++;
    }
}

int main(void)
{
    /* ============================================================== */
    /* 1. 用语义参数反推 Xilinx 的既有常量                            */
    /* ============================================================== */
    /*
     * 这 5 个值来自 Xilinx xil_mmu.h / translation_table.S,
     * 是 Zynq-7000 上长期验证过的。这里不是"把我的常量抄一遍",
     * 而是从 AP/TEX/C/B/Domain/S/XN 这些语义参数重新算出来再比对 ——
     * 位域只要错一处就对不上。
     */
    check_u32(mmu_l1_section_attr(MMU_AP_FULL, 0x5u, 0u, 1u, 0xFu, true, false),
              MMU_ATTR_NORMAL_WB, "NORMAL_WB must reproduce 0x15DE6");
    check_u32(mmu_l1_section_attr(MMU_AP_FULL, 0x6u, 1u, 0u, 0xFu, true, false),
              MMU_ATTR_NORMAL_WT, "NORMAL_WT must reproduce 0x16DEA");
    check_u32(mmu_l1_section_attr(MMU_AP_FULL, 0x1u, 0u, 0u, 0xFu, true, false),
              MMU_ATTR_NORMAL_NC, "NORMAL_NC must reproduce 0x11DE2");
    check_u32(mmu_l1_section_attr(MMU_AP_FULL, 0x0u, 0u, 0u, 0x0u, false, false),
              MMU_ATTR_STRONG_ORDERED, "STRONG_ORDERED must reproduce 0xC02");
    check_u32(mmu_l1_section_attr(MMU_AP_FULL, 0x0u, 0u, 1u, 0x0u, false, false),
              MMU_ATTR_DEVICE, "DEVICE must reproduce 0xC06");

    /* RESERVED 是 fault 描述符,类型位为 0,不能由 attr 组合函数生成 */
    check_u32(MMU_ATTR_RESERVED, 0u, "RESERVED must be 0");
    check(MMU_IS_L1_FAULT(MMU_ATTR_RESERVED), "RESERVED must read as an L1 fault");

    /* ============================================================== */
    /* 2. 一级与二级的位域不对称                                      */
    /* ============================================================== */
    {
        unsigned int l1 = mmu_l1_section_attr(MMU_AP_FULL, 0x5u, 0u, 1u, 0x0u, true, false);
        unsigned int l2 = mmu_l2_small_page_attr(MMU_AP_FULL, 0x5u, 0u, 1u, true, false);

        check(l1 != l2, "identical semantic input must give different L1/L2 words");

        /* 一级:AP[1:0] 在 bit11:10,TEX 在 bit14:12,AP[2] 在 bit15,S 在 bit16 */
        check((l1 & (0x3u << 10)) == (0x3u << 10), "L1 AP[1:0] lives at bit11:10");
        check((l1 & (0x7u << 12)) == (0x5u << 12), "L1 TEX lives at bit14:12");
        check((l1 & (1u << 16)) != 0u, "L1 S lives at bit16");

        /* 二级:AP[1:0] 在 bit5:4,TEX 在 bit8:6,S 在 bit10 —— 位置全不同 */
        check((l2 & (0x3u << 4)) == (0x3u << 4), "L2 AP[1:0] lives at bit5:4");
        check((l2 & (0x7u << 6)) == (0x5u << 6), "L2 TEX lives at bit8:6");
        check((l2 & (1u << 10)) != 0u, "L2 S lives at bit10");

        /*
         * 反向断言:一级独有的高位字段在二级字里必须是空的。
         *
         * 只挑真正不重叠的位来断言。注意 bits[11:10] 在二级里是
         * nG 与 S,**与一级的 AP[1:0] 位置重叠**,所以那一对比无意义 ——
         * 两套布局在低 12 位里本来就是交错的,不是简单的"整体平移"。
         */
        check((l2 & (0x7u << 12)) == 0u, "L2 must not carry an L1 TEX field");
        check((l2 & (1u << 15)) == 0u, "L2 must not carry an L1 AP[2] bit");
        check((l2 & (1u << 16)) == 0u, "L2 must not carry an L1 S bit");

        /* AP[2] 位置也不同:一级 bit15,二级 bit9 */
        check_u32(mmu_l1_section_attr(MMU_AP_PL1_RO, 0u, 0u, 0u, 0u, false, false) & (1u << 15),
                  1u << 15, "L1 AP[2] lives at bit15");
        check_u32(mmu_l2_small_page_attr(MMU_AP_PL1_RO, 0u, 0u, 0u, false, false) & (1u << 9),
                  1u << 9, "L2 AP[2] lives at bit9");
    }

    /* ============================================================== */
    /* 2b. 把一级属性用到二级项上会**改掉物理地址**                   */
    /* ============================================================== */
    {
        /*
         * 这是两套布局混用最具体的危害,值得单独钉住。
         *
         * 一级属性字在 bits[19:12] 有 Domain 与 TEX,而二级项的
         * bits[31:12] 全是物理地址。于是"一级属性 | 二级页基址"出来的
         * 描述符,地址字段被悄悄改动了 ——
         *
         *   mmu_small_page_descriptor(0x00123000, MMU_ATTR_NORMAL_WB)
         *     = 0x00137DE6  -> 解析出的物理地址是 0x00137000 而不是 0x00123000
         *
         * 它编译通过、运行也不报错,只是每次访问都落到错误的物理页上。
         * 这个断言存在的意义就是让"顺手抄一个常量过去"当场暴露。
         */
        unsigned int wrong = mmu_small_page_descriptor(0x00123000u, MMU_ATTR_NORMAL_WB);
        unsigned int right = mmu_small_page_descriptor(
            0x00123000u, mmu_l2_small_page_attr(MMU_AP_FULL, 0x5u, 0u, 1u, true, false));

        check(MMU_IS_L2_SMALL_PAGE(wrong), "an L1 attribute still looks like a valid L2 entry");
        check(mmu_descriptor_pa(wrong, 2) != 0x00123000u,
              "an L1 attribute word silently corrupts the L2 physical address");
        check_u32(mmu_descriptor_pa(wrong, 2), 0x00137000u, "the corruption is exactly the TEX/Domain bits");
        check_u32(mmu_descriptor_pa(right, 2), 0x00123000u, "the L2 helper preserves the address");
    }

    /* ============================================================== */
    /* 3. XN 在一级和二级落点不同,且二级小页的 bit0 就是 XN           */
    /* ============================================================== */
    {
        unsigned int l1_xn = mmu_l1_section_attr(MMU_AP_FULL, 0u, 0u, 0u, 0u, false, true);
        unsigned int l2_xn = mmu_l2_small_page_attr(MMU_AP_FULL, 0u, 0u, 0u, false, true);
        unsigned int l2_exec = mmu_l2_small_page_attr(MMU_AP_FULL, 0u, 0u, 0u, false, false);

        check((l1_xn & (1u << 4)) != 0u, "L1 XN lives at bit4");
        check((l2_xn & 0x1u) != 0u, "L2 XN lives at bit0");

        /*
         * 二级小页有 0b10(XN=0)和 0b11(XN=1)两种编码。
         * 若像一级那样写 `(desc & 3) == 2` 判定小页,就会漏掉 XN=1 的那一半 ——
         * 于是"明明映射了却报 fault"。这里两种都必须被认出来。
         */
        check(MMU_IS_L2_SMALL_PAGE(l2_exec), "small page with XN=0 must be recognised");
        check(MMU_IS_L2_SMALL_PAGE(l2_xn), "small page with XN=1 must also be recognised");
        check(!MMU_IS_L2_LARGE_PAGE(l2_xn), "XN=1 must not be mistaken for a large page");
        check(l2_exec == 0x32u, "small page XN=0 with AP=full keeps type bits 0b10");
        check((l2_exec & 0x3u) == 0x2u, "small page type bits must be 0b10 when XN=0");

        /* 描述符文本要能区分这两种,否则诊断输出会把它们混为一谈 */
        {
            const char *exec_text = mmu_descriptor_kind_text(l2_exec, 2);
            const char *xn_text = mmu_descriptor_kind_text(l2_xn, 2);

            check(exec_text != xn_text, "XN and executable small pages need distinct text");
            check(exec_text[0] == 's' && xn_text[0] == 's', "both must read as small pages");
        }
    }

    /* ============================================================== */
    /* 4. 索引提取                                                    */
    /* ============================================================== */
    check_u32(mmu_l1_index(0x00000000u), 0u, "L1 index of 0");
    check_u32(mmu_l1_index(0x00100000u), 1u, "L1 index of the kernel load address");
    check_u32(mmu_l1_index(0xFFFFFFFFu), 0xFFFu, "L1 index of the last byte of the space");
    check_u32(mmu_l1_index(0xE0001000u), 0xE00u, "L1 index of UART1 (0xE0001000)");
    check_u32(mmu_l1_index(0xF8F01000u), 0xF8Fu, "L1 index of the GIC distributor");
    check_u32(mmu_l1_index(0x41200000u), 0x412u, "L1 index of the AXI GPIO");

    check_u32(mmu_l2_index(0x00000000u), 0u, "L2 index of 0");
    check_u32(mmu_l2_index(0x00000FFFu), 0u, "L2 index of the last byte of page 0 is still 0");
    check_u32(mmu_l2_index(0x000FF000u), 0xFFu, "L2 index of the last page in section 0");
    check_u32(mmu_l2_index(0x00123000u), 0x23u, "L2 index of 0x00123000");

    /* 索引与段基址必须自洽:索引 <- 地址 -> 段基址 往返不丢信息 */
    {
        const unsigned int addrs[] = {0x00000000u, 0x00020000u, 0x00100000u,
                                      0x001FFFFFu, 0xE0001000u, 0xF8F01000u};
        unsigned int i;

        for (i = 0; i < sizeof(addrs) / sizeof(addrs[0]); i++) {
            unsigned int idx = mmu_l1_index(addrs[i]);
            check_u32(mmu_section_base(addrs[i]), idx << 20, "section base must match L1 index");
            check(mmu_section_offset(addrs[i]) < MMU_SECTION_SIZE, "section offset in range");
        }
    }

    /* ============================================================== */
    /* 5. 描述符构造 <- -> 解析往返                                   */
    /* ============================================================== */
    {
        unsigned int desc;

        /* 内核所在的 DDR 段 */
        desc = mmu_section_descriptor(0x00100000u, MMU_ATTR_NORMAL_WB);
        check_u32(desc, 0x00115DE6u, "kernel section descriptor");
        check(MMU_IS_L1_SECTION(desc), "kernel descriptor is a section");
        check_u32(mmu_descriptor_pa(desc, 1), 0x00100000u, "kernel section PA round-trip");

        /* 1MB 对齐:传进来带偏移的地址必须被截断到段基址 */
        desc = mmu_section_descriptor(0x001FFFFFu, MMU_ATTR_NORMAL_WB);
        check_u32(mmu_descriptor_pa(desc, 1), 0x00100000u, "section PA must be 1MB aligned");

        /* 一级页表描述符:指向二级表,物理基址字段是 bits[31:10] */
        desc = mmu_page_table_descriptor(0x00020400u, 3u);
        check(MMU_IS_L1_PAGE_TABLE(desc), "page table descriptor type");
        check_u32(mmu_descriptor_pa(desc, 1), 0x00020400u, "L2 table PA round-trip");
        check_u32((desc >> MMU_L1_DOMAIN_SHIFT) & 0xFu, 3u, "domain must be encoded");

        /* 二级小页 */
        desc = mmu_small_page_descriptor(0x00123000u,
                                         mmu_l2_small_page_attr(MMU_AP_FULL, 0x5u, 0u, 1u, true, true));
        check(MMU_IS_L2_SMALL_PAGE(desc), "small page descriptor type");
        check_u32(mmu_descriptor_pa(desc, 2), 0x00123000u, "small page PA round-trip");

        /* fault 描述符没有有效地址 */
        check_u32(mmu_descriptor_pa(MMU_ATTR_RESERVED, 1), 0u, "fault descriptor has no PA");
        check_u32(mmu_descriptor_pa(MMU_ATTR_RESERVED, 2), 0u, "L2 fault has no PA");
    }

    /* ============================================================== */
    /* 6. 一级表的 16KB 对齐校验                                      */
    /* ============================================================== */
    {
        /*
         * 16KB 对齐不是建议而是硬性要求:TTBR0 低 14 位是属性字段,
         * 表没对齐的话硬件会从错位的地址取描述符,症状完全不可预测。
         *
         * 这里手工向上对齐而不是用 __attribute__((aligned(16384))) ——
         * clang 对超过 8192 字节的对齐属性会直接报错。
         * 手工对齐反而更贴近被测代码的实际判断逻辑。
         */
        static unsigned int raw[MMU_L1_ENTRY_COUNT + 64u];
        unsigned int *aligned = (unsigned int *)(((uintptr_t)raw + (MMU_L1_TABLE_ALIGN - 1u)) &
                                                 ~(uintptr_t)(MMU_L1_TABLE_ALIGN - 1u));

        check(((uintptr_t)aligned % MMU_L1_TABLE_ALIGN) == 0u, "manual alignment must hold");
        check(mmu_l1_table_check(aligned) == MMU_L1_CHECK_OK, "16KB aligned table must pass");
        check(mmu_l1_table_check(NULL) == MMU_L1_CHECK_NULL, "NULL table must be rejected");
        check(mmu_l1_table_check(aligned + 1) == MMU_L1_CHECK_MISALIGNED,
              "table+1 (4 byte offset) must be rejected");
    }

    /* ============================================================== */
    /* 7. 几何常量自洽                                                */
    /* ============================================================== */
    check_u32(MMU_L1_ENTRY_COUNT * 4u, MMU_L1_TABLE_SIZE, "L1 table is 4096 words");
    check_u32(MMU_L1_TABLE_SIZE, 16384u, "L1 table is 16KB");
    /*
     * "4096 项 × 1MB = 4GB" 在 32 位里会绕回 0,所以用 64 位算。
     * 这正是在验证"一级表恰好覆盖整个 32 位地址空间"这个前提。
     */
    check((unsigned long long)MMU_L1_ENTRY_COUNT * MMU_SECTION_SIZE == 4294967296ULL,
          "L1 table must cover exactly the whole 4GB space");
    check_u32(MMU_L2_ENTRY_COUNT * MMU_PAGE_SIZE, MMU_SECTION_SIZE,
              "an L2 table covers exactly one section");
    check_u32(MMU_L1_TABLE_ALIGN, MMU_L1_TABLE_SIZE, "L1 alignment equals its size");
    check_u32(MMU_L2_TABLE_ALIGN, MMU_L2_TABLE_SIZE, "L2 alignment equals its size");

    /* ============================================================== */
    /* 8. XN 常量必须分级,不能有一个"通用"版本                       */
    /* ============================================================== */
    {
        /*
         * Xilinx 只给了一个 EXECUTE_NEVER = (1<<4)|(1<<0)。
         * 它在二级小页上是对的(小页 0b1x,bit0 就是 XN),
         * 但或到一级段上会把类型位从 0b10 变成 0b11 = reserved,
         * 整段当场失效。这里把这个陷阱钉死。
         */
        unsigned int good = MMU_ATTR_NORMAL_NC | MMU_L1_ATTR_XN;
        unsigned int bad = MMU_ATTR_NORMAL_NC | MMU_L2_ATTR_XN;

        check(MMU_L1_ATTR_XN == (1u << 4), "L1 XN is bit4");
        check(MMU_L2_ATTR_XN == (1u << 0), "L2 XN is bit0");

        check(MMU_IS_L1_SECTION(good), "L1 XN keeps the section type intact");
        check(!MMU_IS_L1_SECTION(bad), "applying the L2 XN bit to an L1 section destroys it");
        check_u32(bad & MMU_DESC_TYPE_MASK, MMU_L1_TYPE_RESERVED,
                  "the L2 XN bit turns an L1 section into a reserved descriptor");
    }

    /* ============================================================== */
    /* 9. 区域表自检                                                  */
    /* ============================================================== */
    {
        u32 count = 0;
        const mmu_region_t *regions = mmu_regions(&count);

        check(mmu_regions_check() == MMU_REGIONS_OK, "region table must pass its own check");
        check(count > 0u, "region table must not be empty");
        check(regions != NULL, "region table pointer must not be NULL");

        /* 升序且不重叠:重叠会让后写的区域静默覆盖前一个 */
        {
            u32 i;
            for (i = 1; i < count; i++) {
                unsigned long long prev_end =
                    (unsigned long long)regions[i - 1].base + regions[i - 1].size;
                check((unsigned long long)regions[i].base >= prev_end,
                      "regions must be sorted and non-overlapping");
            }
        }

        check_u32(mmu_regions(NULL) == regions, 1u, "mmu_regions(NULL) must still return the table");
    }

    /* ============================================================== */
    /* 10. 建表结果:逐点核对关键地址                                  */
    /* ============================================================== */
    {
        static unsigned int table[MMU_L1_ENTRY_COUNT];

        mmu_build_l1_table(table);

        /* -- 低 1MB:OCM + 心跳,必须不可缓存,否则 JTAG 读到陈旧值 -- */
        check_u32(table[mmu_l1_index(0x00000000u)], 0x11DE2u, "OCM section must be Normal NC");
        check_u32(table[mmu_l1_index(0x00020000u)], 0x11DE2u, "heartbeat is inside the NC section");

        /* -- DDR:内核所在的段必须是写回可缓存 -- */
        check_u32(table[mmu_l1_index(0x00100000u)], 0x15DE6u | 0x00100000u, "kernel load section");
        check_u32(table[mmu_l1_index(0x3FF00000u)], 0x15DE6u | 0x3FF00000u, "last DDR section");
        check(mmu_descriptor_pa(table[mmu_l1_index(0x00100000u)], 1) == 0x00100000u,
              "identity mapping: VA == PA for the kernel");

        /* -- PL:AXI GPIO 在强序段里 -- */
        check_u32(table[mmu_l1_index(0x41200000u)] & 0xFFFFFu, 0xC02u,
                  "AXI GPIO section must be Strongly-Ordered");
        check_u32(mmu_descriptor_pa(table[mmu_l1_index(0x41200000u)], 1), 0x41200000u,
                  "AXI GPIO identity mapping");

        /* -- PS 外设:UART1 -- */
        check_u32(mmu_l1_index(0xE0001000u), mmu_l1_index(0xE0000000u),
                  "UART1 must share a section with the start of PS peripherals");
        check_u32(table[mmu_l1_index(0xE0001000u)] & 0xFFFFFu, 0xC06u,
                  "UART1 section must be Device");

        /* -- SLCR / SCU / GIC -- */
        check_u32(table[mmu_l1_index(0xF8000000u)] & 0xFFFFFu, 0xC06u, "SLCR must be Device");
        check_u32(table[mmu_l1_index(0xF8F00000u)] & 0xFFFFFu, 0xC06u, "SCU must be Device");
        check_u32(table[mmu_l1_index(0xF8F01000u)] & 0xFFFFFu, 0xC06u, "GIC distributor must be Device");
        check_u32(table[mmu_l1_index(0xF8F00100u)] & 0xFFFFFu, 0xC06u, "GIC CPU interface must be Device");

        /* -- 高位 OCM 别名:与低位属性一致 -- */
        check_u32(table[mmu_l1_index(0xFFF00000u)], 0x11DE2u | 0xFFF00000u,
                  "high OCM alias must use the same attribute as the low one");

        /* -- 未映射区必须是 fault,而不是悄悄落到设备上 -- */
        check_u32(table[mmu_l1_index(0xC0000000u)], 0u, "reserved 0xC0000000 must fault");
        check_u32(table[mmu_l1_index(0xE0300000u)], 0u, "the hole after PS peripherals must fault");
        check_u32(table[mmu_l1_index(0xE1000000u)], 0u, "absent NAND/NOR must fault");
        check_u32(table[mmu_l1_index(0xFC000000u)], 0u, "unused QSPI XIP window must fault");
        check_u32(table[mmu_l1_index(0xFFE00000u)], 0u, "the hole below the high OCM must fault");

        /* -- 全局不变量 1:不允许出现 reserved 类型(0b11) -- */
        /*
         * 一级描述符类型 0b11 是保留值,行为不可预测。
         * 它最可能的来源就是把二级的 XN 位或到一级段上(见第 8 节),
         * 所以整表扫一遍,把这类错误挡在开 MMU 之前。
         */
        {
            u32 i;
            u32 reserved_count = 0;
            u32 mapped_count = 0;

            for (i = 0; i < MMU_L1_ENTRY_COUNT; i++) {
                if ((table[i] & MMU_DESC_TYPE_MASK) == MMU_L1_TYPE_RESERVED) {
                    reserved_count++;
                }
                if (MMU_IS_L1_SECTION(table[i])) {
                    mapped_count++;
                }
            }

            check_u32(reserved_count, 0u, "no L1 entry may use the reserved type 0b11");

            /* -- 全局不变量 2:映射段数 == 区域表声明的段数总和 -- */
            {
                u32 count = 0;
                const mmu_region_t *regions = mmu_regions(&count);
                u32 expected = 0;
                u32 i2;

                for (i2 = 0; i2 < count; i2++) {
                    expected += regions[i2].size >> MMU_SECTION_SHIFT;
                }

                check_u32(mapped_count, expected,
                          "mapped section count must equal the sum declared by the region table");
                check(mapped_count < MMU_L1_ENTRY_COUNT,
                      "unmapped addresses must remain as faults, not be filled in");
            }
        }

        /* -- 恒等映射:每个已映射段的描述符地址必须等于段基址 -- */
        {
            u32 i;
            u32 mismatched = 0;

            for (i = 0; i < MMU_L1_ENTRY_COUNT; i++) {
                if (MMU_IS_L1_SECTION(table[i])) {
                    if (mmu_descriptor_pa(table[i], 1) != (i << MMU_SECTION_SHIFT)) {
                        mismatched++;
                    }
                }
            }

            check_u32(mismatched, 0u, "every mapped section must be an identity mapping");
        }
    }

    /* ============================================================== */
    /* 11. 区域名查询(诊断输出用)                                     */
    /* ============================================================== */
    {
        check_region(0x00020000u, "OCM + heartbeat", "heartbeat address");
        check_region(0x00000000u, "OCM + heartbeat", "start of the low 1MB");
        check_region(0x00100000u, "DDR", "kernel load address");
        check_region(0x3FFFFFFFu, "DDR", "last byte of DDR");
        check_region(0x41200000u, "PL (AXI GP", "AXI GPIO address");
        check_region(0xE0001000u, "PS peripherals", "UART1");
        check_region(0xF8F01000u, "SLCR", "GIC distributor");
        check_region(0xFFF00000u, "OCM high alias", "start of the high OCM alias");
        check_region(0xFFFFFFFFu, "OCM high alias", "very last byte of the address space");

        /* 未映射区必须报 unmapped,而不是被某个区域误覆盖 */
        check_region(0xC0000000u, "unmapped", "reserved 0xC0000000");
        check_region(0xE0300000u, "unmapped", "the hole after PS peripherals");
        check_region(0xFFE00000u, "unmapped", "the hole below the high OCM");
    }

    if (failures == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", failures);
    return 1;
}
"""


class Arm32MmuTests(unittest.TestCase):
    # 注意:本机 MinGW 的 gcc/g++ 是坏的(cc1.exe 静默退出),
    # 所以逐个探测可用编译器,与 test_arm32_uart_baud.py 保持一致。
    COMPILER_CANDIDATES = ("gcc", "cc", "clang")

    def _compile_and_run(self, source: str, extra_sources: list[Path]) -> str:
        """编译并运行测试程序,返回其 stdout。"""
        # 显式 UTF-8:项目注释是中文,Windows 默认按 GBK 解码会炸。
        capture = {"capture_output": True, "text": True, "encoding": "utf-8", "errors": "replace"}

        with tempfile.TemporaryDirectory() as tmp:
            harness = Path(tmp) / "mmu_test.c"
            binary = Path(tmp) / "mmu_test"
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
                f"MMU descriptor checks failed:\n{run_result.stdout}{run_result.stderr}",
            )
            return run_result.stdout

    def test_short_descriptor_layout(self) -> None:
        output = self._compile_and_run(HARNESS, [ROOT / "arch/arm32/src/mmu.c"])
        self.assertIn("ALL PASS", output)


if __name__ == "__main__":
    unittest.main()
