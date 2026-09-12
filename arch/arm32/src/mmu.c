/*
 * ARMv7-A 短描述符编解码 —— 纯计算实现
 *
 * 本文件刻意**不含任何 CP15 或 MMIO 操作**,因此宿主 gcc 可以直接编译它,
 * 单测见 tests/test_arm32_mmu.py。真正的"打开 MMU"流程放在 M2-3。
 *
 * 为什么要单独写一层属性组合函数(mmu_l1_section_attr / mmu_l2_small_page_attr),
 * 而不是让调用方直接或 Xilinx 的现成常量:
 *
 *   一级段描述符和二级小页描述符的 AP / TEX / S 位**位置完全不同**。
 *   一级:AP[1:0] 在 bit11:10、TEX 在 bit14:12、AP[2] 在 bit15、S 在 bit16
 *   二级:AP[1:0] 在 bit5:4、  TEX 在 bit8:6、 AP[2] 在 bit9、 S 在 bit10
 *
 *   直接抄一个一级的 0x15DE6 到二级项上不会报错,只会让那一页
 *   变成"权限和缓存属性都莫名其妙"的状态 —— 这类错误在板上表现为
 *   偶发数据损坏,极难定位。
 *
 *   把位域知识收进两个函数之后,每个位只在一个地方出现,
 *   而且可以用 Xilinx 的既有常量做交叉验证(见单测)。
 */

#include <arch/mmu.h>

/* ------------------------------------------------------------------ */
/* 属性组合                                                             */
/* ------------------------------------------------------------------ */

u32 mmu_l1_section_attr(u32 ap3, u32 tex, u32 c, u32 b, u32 domain, bool shareable, bool xn)
{
    u32 attr = MMU_L1_TYPE_SECTION;

    /* AP[1:0] 在 bit11:10 */
    attr |= (ap3 & MMU_AP_LOW_MASK) << MMU_AP_SHIFT_L1;
    /* AP[2] 在 bit15 */
    if ((ap3 & 0x4u) != 0u) {
        attr |= MMU_L1_AP2_BIT;
    }

    attr |= (tex & 0x7u) << 12;
    if (c != 0u) {
        attr |= (1u << 3);
    }
    if (b != 0u) {
        attr |= (1u << 2);
    }

    attr |= (domain & MMU_L1_DOMAIN_MASK) << MMU_L1_DOMAIN_SHIFT;

    if (shareable) {
        attr |= MMU_ATTR_S_BIT;
    }
    if (xn) {
        /* 一级段的 XN 在 bit4 */
        attr |= (1u << 4);
    }

    return attr;
}

u32 mmu_l2_small_page_attr(u32 ap3, u32 tex, u32 c, u32 b, bool shareable, bool xn)
{
    /*
     * 二级小页:bit1 恒为 1,bit0 是 XN。
     * 所以"类型"这一位和 XN 挤在同一个字节里,不能像一级那样
     * 用一个 MMU_L1_TYPE_SECTION 常量打底。
     */
    u32 attr = 0x2u;

    if (xn) {
        attr |= 0x1u;
    }

    if (b != 0u) {
        attr |= (1u << 2);
    }
    if (c != 0u) {
        attr |= (1u << 3);
    }

    /* AP[1:0] 在 bit5:4 —— 与一级的 bit11:10 不同 */
    attr |= (ap3 & MMU_AP_LOW_MASK) << MMU_AP_SHIFT_L2;
    /* AP[2] 在 bit9 —— 与一级的 bit15 不同 */
    if ((ap3 & 0x4u) != 0u) {
        attr |= MMU_L2_AP2_BIT;
    }

    attr |= (tex & 0x7u) << 6;

    if (shareable) {
        attr |= (1u << 10);
    }

    return attr;
}

/* ------------------------------------------------------------------ */
/* 描述符构造                                                           */
/* ------------------------------------------------------------------ */

u32 mmu_section_descriptor(u32 pa, u32 attr)
{
    /* 段基址只取 bits[31:20];低 20 位全部留给属性 */
    return (pa & MMU_SECTION_MASK) | attr;
}

u32 mmu_page_table_descriptor(u32 l2_table_pa, u32 domain)
{
    /*
     * 一级页表描述符没有 AP 字段 —— 权限由二级项决定。
     * 这里只放类型位、域和二级表基址(bits[31:10])。
     */
    return (l2_table_pa & MMU_L2_TABLE_MASK) | ((domain & MMU_L1_DOMAIN_MASK) << MMU_L1_DOMAIN_SHIFT) |
           MMU_L1_TYPE_PAGE_TABLE;
}

u32 mmu_small_page_descriptor(u32 pa, u32 attr)
{
    return (pa & MMU_PAGE_MASK) | attr;
}

/* ------------------------------------------------------------------ */
/* 描述符解析                                                           */
/* ------------------------------------------------------------------ */

u32 mmu_descriptor_pa(u32 desc, u32 level)
{
    if (level == 1u) {
        if (MMU_IS_L1_SECTION(desc)) {
            return desc & MMU_SECTION_MASK;
        }
        if (MMU_IS_L1_PAGE_TABLE(desc)) {
            return desc & MMU_L2_TABLE_MASK;
        }
        return 0; /* fault / reserved 没有有效地址 */
    }

    if (MMU_IS_L2_SMALL_PAGE(desc)) {
        return desc & MMU_PAGE_MASK;
    }
    if (MMU_IS_L2_LARGE_PAGE(desc)) {
        return desc & 0xFFFF0000u; /* 64KB 大页基址在 bits[31:16] */
    }
    return 0;
}

const char *mmu_descriptor_kind_text(u32 desc, u32 level)
{
    if (level == 1u) {
        switch (desc & MMU_DESC_TYPE_MASK) {
        case MMU_L1_TYPE_FAULT:
            return "fault";
        case MMU_L1_TYPE_PAGE_TABLE:
            return "page table -> L2";
        case MMU_L1_TYPE_SECTION:
            return "section 1MB";
        default:
            return "reserved";
        }
    }

    /*
     * 二级不能按 bits[1:0] 做等值 switch:小页是 0b1x,
     * 其中 bit0 表示 XN。先判大页,剩下的 0b1x 都是小页。
     */
    if (MMU_IS_L2_FAULT(desc)) {
        return "fault";
    }
    if (MMU_IS_L2_LARGE_PAGE(desc)) {
        return "large page 64KB";
    }
    if (MMU_IS_L2_SMALL_PAGE(desc)) {
        return (desc & 0x1u) != 0u ? "small page 4KB (XN)" : "small page 4KB";
    }
    return "reserved";
}

/* ------------------------------------------------------------------ */
/* 校验                                                                 */
/* ------------------------------------------------------------------ */

mmu_l1_check_t mmu_l1_table_check(const u32 *table)
{
    if (table == NULL) {
        return MMU_L1_CHECK_NULL;
    }

    /*
     * 16KB 对齐是硬性要求:TTBR0 的低 14 位是属性字段而不是基址的一部分,
     * 表如果没对齐到 16KB,硬件会从错误的地址取描述符 ——
     * 表现为随机取到别的数据当地址转换项,症状完全不可预测。
     */
    if (((uintptr_t)table & (MMU_L1_TABLE_ALIGN - 1u)) != 0u) {
        return MMU_L1_CHECK_MISALIGNED;
    }

    return MMU_L1_CHECK_OK;
}
