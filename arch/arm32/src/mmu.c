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
#include <arch/platform.h>

/*
 * 编译期确认区域表里的字面量与板级描述符一致。
 *
 * 这两处描述的是同一件事(DDR 在哪、多大),但一个在 platform.h、
 * 一个在本文件的区域表里。若只改一处,页表会安静地映射错范围 ——
 * 所以让它在编译期就报错,而不是等到板上跑飞。
 */
_Static_assert(PLAT_DDR_BASE == 0x00100000u, "region table disagrees with PLAT_DDR_BASE");
_Static_assert(PLAT_DDR_SIZE == 0x3FF00000u, "region table disagrees with PLAT_DDR_SIZE");
_Static_assert(PLAT_OCM_BASE == 0x00000000u, "region table disagrees with PLAT_OCM_BASE");

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

/* ------------------------------------------------------------------ */
/* 地址映射区域表                                                       */
/* ------------------------------------------------------------------ */

/* 区域表上限。真表只有 6 项,给足余量即可,超出说明写错了 */
#define MMU_REGION_LIMIT 32u

/* 单个 1MB 段。低 1MB 承载 OCM(256KB)与心跳 */
#define MMU_LOW_1MB_BASE 0x00000000u
#define MMU_LOW_1MB_SIZE 0x00100000u

/*
 * 映射区域表(恒等映射:虚拟地址 == 物理地址)。
 *
 * 与 Xilinx 原表的三处**有意偏离**,逐条说明理由:
 *
 * 1. 低 1MB 用 Normal Non-cacheable 而不是 WB。
 *    OCM 里放着 OCM 心跳(0x20000),那是 JTAG 唯一的观测通道。
 *    JTAG 走 DAP/AXI 直接读物理内存,**不经过 CPU 的 L1/L2** ——
 *    一旦这段被当成写回可缓存,心跳写进去只是躺在 cache 里,
 *    JTAG 读到的是陈旧值。偏偏 MMU 刚开的那几次调试最依赖它,
 *    所以这里宁可损失一点性能也要保住这条通道。
 *    (OCM 是片上存储,本身访问就快,不可缓存的代价很小。)
 *
 * 2. 高位 OCM 别名(0xFFF00000)用与低位**相同**的属性。
 *    两者是同一块物理内存的两种映射。以不同缓存属性映射同一物理位置
 *    在架构上是 UNPREDICTABLE,虽然当前不访问高位别名,
 *    但保持一致的代价为零。
 *
 * 3. 未列出的地址一律是 fault,而不是像 Xilinx 那样把大片保留区
 *    也填成有效映射。本板实际只有 6 处外设,Xilinx 表覆盖的
 *    NAND/NOR/QSPI-XIP 等在这块板子上都不存在。
 *    访问不存在的从设备会得到清晰的 translation fault,
 *    而不是一次可能挂死 AXI 总线的事务。
 *
 * 属性取值见头文件;每个都标注了位域解码。
 */
static const mmu_region_t g_regions[] = {
    /*
     * 低 1MB:OCM(实测 192KB)+ 保留空洞 + 心跳。
     *
     * **刻意不加 XN**,尽管这里几乎全是数据。理由:
     *   计划里的 SMP 方案是"CPU1 从 OCM 跳板启动"(见 ZYNQ7020_PORT_PLAN
     *   §SMP)。那段跳板代码将来就要放在 OCM 里执行,现在标成不可执行
     *   会在做 SMP 时炸掉,而且那个失败离今天的改动很远、很难联想到。
     *   OCM 就这么一块,等真要加固时应当用 4KB 页把跳板区单独划出来,
     *   而不是在 1MB 段粒度上做一个将来必须撤销的决定。
     *
     * 不可缓存同样是有意的:0x20000 是 JTAG 唯一的心跳观测通道,
     * JTAG 走 DAP/AXI 直接读物理内存、**不经过 CPU 的 L1/L2**。
     * 这段一旦是写回可缓存,心跳写进去只是躺在 cache 里,
     * JTAG 读到的是陈旧值 —— 偏偏 MMU 刚开的那几次调试最依赖它。
     */
    {MMU_LOW_1MB_BASE, MMU_LOW_1MB_SIZE, MMU_ATTR_NORMAL_NC, "OCM + heartbeat (JTAG-visible)"},
    {PLAT_DDR_BASE, PLAT_DDR_SIZE, MMU_ATTR_NORMAL_WB, "DDR"},
    /*
     * PL 用强序,与 Xilinx 一致,也**不加 XN**。
     *
     * 强序不做推测访问,正是这里想要的(见计划 §2.15 的 ARM 794073)。
     * 不加 XN 是因为 PL 里将来可能有需要取指的东西(软核的 BRAM、
     * 或从 PL 加载的代码段),而当前这块 PL 就是个占位的 AXI GPIO,
     * 没有任何理由替未来的设计做决定。
     */
    {0x40000000u, 0x80000000u, MMU_ATTR_STRONG_ORDERED, "PL (AXI GP0/GP1)"},
    /* PS 外设:UART0/1、I2C、SPI、CAN、GEM、GPIO、QSPI、SD 等。纯数据,加 XN */
    {0xE0000000u, 0x00300000u, MMU_ATTR_DEVICE_XN, "PS peripherals"},
    /* SLCR、SCU、GIC、全局/私有定时器、PL310。纯数据,加 XN */
    {0xF8000000u, 0x01000000u, MMU_ATTR_DEVICE_XN, "SLCR / SCU / GIC"},
    /*
     * 高位 OCM 别名。
     *
     * ⚠ 这里的地址与大小是**按 BSP 实测数据修正过的**:
     *     xparameters.h: XPAR_PS7_RAM_0 = 0x00000000..0x0002FFFF (192KB)
     *                    XPAR_PS7_RAM_1 = 0xFFFF0000..0xFFFFFDFF (~64KB)
     *   也就是说 OCM 的高位别名在 **0xFFFF0000**,而不是 0xFFF00000。
     *   本区域从 0xFFF00000 起、覆盖 1MB,是由于段粒度(1MB)无法再细分,
     *   把 OCM 别名、BootROM 与若干保留区一并圈了进来 ——
     *   Xilinx 的 translation_table.S 里也明确记录了这个同样的限制。
     *
     * 属性必须与低位 OCM **完全一致**(不可缓存、可执行):
     * 两者是同一块物理内存的两种映射,以不同属性映射同一物理位置
     * 在架构上是 UNPREDICTABLE。整个 1MB 里没有东西需要被缓存,
     * 而误缓存 BootROM 会有麻烦,所以整体按不可缓存处理。
     */
    {0xFFF00000u, 0x00100000u, MMU_ATTR_NORMAL_NC, "OCM high alias / BootROM"},
};

const mmu_region_t *mmu_regions(u32 *count_out)
{
    if (count_out != NULL) {
        *count_out = (u32)(sizeof(g_regions) / sizeof(g_regions[0]));
    }
    return g_regions;
}

mmu_regions_check_t mmu_regions_check(void)
{
    u32                count = 0;
    const mmu_region_t *regions = mmu_regions(&count);
    u32                i;

    if (count == 0u || count > MMU_REGION_LIMIT) {
        return MMU_REGIONS_TOO_MANY;
    }

    for (i = 0; i < count; i++) {
        u64 base = (u64)regions[i].base;
        u64 size = (u64)regions[i].size;

        if (size == 0u || (base % MMU_SECTION_SIZE) != 0u || (size % MMU_SECTION_SIZE) != 0u) {
            return MMU_REGIONS_MISALIGNED;
        }

        /*
         * 用 64 位算上界:0xFFF00000 + 0x100000 恰好是 0x100000000,
         * 在 32 位里会绕回 0 而被误判成"没越界"。
         */
        if (base + size > 0x100000000ULL) {
            return MMU_REGIONS_OUT_OF_RANGE;
        }

        if (i > 0u) {
            u64 prev_base = (u64)regions[i - 1u].base;
            u64 prev_end = prev_base + (u64)regions[i - 1u].size;

            if (base < prev_base) {
                return MMU_REGIONS_UNSORTED;
            }
            if (base < prev_end) {
                return MMU_REGIONS_OVERLAP;
            }
        }
    }

    return MMU_REGIONS_OK;
}

void mmu_build_l1_table(u32 *table)
{
    u32       count = 0;
    const mmu_region_t *regions = mmu_regions(&count);
    u32       i;
    u32       j;

    /*
     * 先全部填成 fault。
     *
     * fault 描述符的值就是 0(类型位 0b00),所以未列出的地址
     * 访问即产生 translation fault —— 这正是"没映射"应有的行为。
     */
    for (i = 0; i < MMU_L1_ENTRY_COUNT; i++) {
        table[i] = MMU_ATTR_RESERVED;
    }

    for (i = 0; i < count; i++) {
        u32 first_index = regions[i].base >> MMU_SECTION_SHIFT;
        u32 sections = regions[i].size >> MMU_SECTION_SHIFT;

        for (j = 0; j < sections; j++) {
            u32 pa = regions[i].base + (j << MMU_SECTION_SHIFT);

            table[first_index + j] = mmu_section_descriptor(pa, regions[i].attr);
        }
    }
}

const char *mmu_region_name_for(u32 addr)
{
    u32       count = 0;
    const mmu_region_t *regions = mmu_regions(&count);
    u32       i;

    for (i = 0; i < count; i++) {
        /* 上界用 64 位,理由同 mmu_regions_check */
        if ((u64)addr >= (u64)regions[i].base &&
            (u64)addr < (u64)regions[i].base + (u64)regions[i].size) {
            return regions[i].name;
        }
    }

    return "unmapped";
}
