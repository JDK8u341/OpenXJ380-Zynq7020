/*
 * ARMv7-A MMU 启用 —— 硬件侧(CP15)
 *
 * 与 src/mmu.c 的分工:
 *   mmu.c     纯逻辑:描述符编解码、区域表、建表。可在宿主机上单测。
 *   mmu_hw.c  硬件侧:页表实体、CP15 配置、真正打开 MMU。只能上板验证。
 *
 * 拆开的原因很实际:mmu.c 被 tests/test_arm32_mmu.py 直接编译,
 * 一旦它 include 了带 mcr/mrc 内联汇编的头文件,宿主 gcc/clang 就编不过,
 * 那部分逻辑也就没法单测了。这与 uart_baud.c / uart_ps.c 的拆法同源。
 */

#include <arch/cpu.h>
#include <arch/heartbeat.h>
#include <arch/io.h>
#include <arch/mmu.h>
#include <arch/platform.h>
#include <arch/types.h>

/*
 * 一级页表实体。
 *
 * 位置与对齐由 kernel.ld 的 .mmu_tbl 段保证(那里有两条 ASSERT),
 * 这里再写一次 aligned 是双保险。
 */
__attribute__((section(".mmu_tbl"), aligned(MMU_L1_TABLE_ALIGN)))
u32 g_mmu_l1_table[MMU_L1_ENTRY_COUNT];

bool mmu_is_enabled(void)
{
    return (arch_read_sctlr() & SCTLR_M) != 0u;
}

/*
 * 打开 MMU。
 *
 * 本阶段**只开地址转换(SCTLR.M),不开 D-Cache / I-Cache**。
 * 这是有意的风险切分:
 *
 *   打开 D-Cache 之前必须先让整个数据缓存失效,否则残留的脏行
 *   会在之后某个时刻被写回内存,覆盖掉正确数据 ——
 *   而"整块 D-Cache 失效"在 ARMv7 上不是一条指令,需要按
 *   set/way 遍历(依赖 CCSIDR 读出的缓存几何),属于缓存维护的范畴。
 *   仓库里 arch/cpu.h 现有的缓存原语都是按地址的(MVA 形式),
 *   恰好没有整块失效。所以缓存维护与开缓存一起放到 M2-5,
 *   本步只验证"地址转换本身是否正确"。
 *
 *   代价是这一步之后系统仍然很慢(所有访问都直通内存),
 *   但换来了 M2-3 的失败原因只有一种:页表错了。
 *
 * 序列参照 AMD/Xilinx standalone BSP 的 cortexa9/gcc/boot.S,
 * 那里是在 Zynq-7000 上跑了十几年的写法。
 */
void mmu_enable(void)
{
    u32 sctlr;

    HB[HB_SLOT_MMUSTAGE] = HB_MMU_STAGE_IDLE;

    /*
     * 1. 先做一次表基址自检。
     *    对齐错了要在写 TTBR0 之前拦住 —— 之后就没机会了。
     */
    if (mmu_l1_table_check(g_mmu_l1_table) != MMU_L1_CHECK_OK) {
        HB[HB_SLOT_MMUSTAGE] = HB_MMU_STAGE_FAILED;
        return;
    }

    /* 区域表自检:重叠或乱序会让后写的区域静默覆盖前一个 */
    if (mmu_regions_check() != MMU_REGIONS_OK) {
        HB[HB_SLOT_MMUSTAGE] = HB_MMU_STAGE_FAILED;
        return;
    }

    /*
     * 2. 建表。
     *    此时 MMU 还没开,这些写入直接落到物理内存。
     */
    mmu_build_l1_table(g_mmu_l1_table);

    /*
     * DSB 不可省:必须保证 4096 条描述符的写入在硬件开始表遍历之前
     * 已经对内存系统可见。少了它,MMU 打开后的第一次取指可能读到
     * 尚未落地的描述符 —— 而故障点是随机的,极难复现。
     */
    arch_dsb();
    HB[HB_SLOT_MMUSTAGE] = HB_MMU_STAGE_BUILT;

    /*
     * 3. 配置转换表基址。
     *    低位或上 Xilinx 的 0x5B(表遍历属性,Outer-cacheable WB),
     *    这些位全部落在 14 位对齐之外,不会破坏基址。
     */
    arch_write_ttbr0((u32)(uintptr_t)g_mmu_l1_table | TTBR0_ATTR_XILINX);

    /*
     * 4. DACR 设为全 manager(每个域 2 位,共 16 个域)。
     *
     *    manager 模式下描述符里的 AP 位**被完全忽略**,不会产生权限故障。
     *    这是有意的降级:本阶段还没有用户态,没有需要保护的边界,
     *    而 AP 写错会导致内核自己都访问不了内存,把"页表错"和
     *    "权限错"两种故障混在一起。等 M4 引入用户态再切 client 模式。
     *    见 arch/mmu.h 里 MMU_DACR_* 的说明。
     */
    arch_write_dacr(0xFFFFFFFFu);

    /*
     * 5. 失效 TLB 与分支预测器。
     *    TLB 里可能有 MMU 关闭期间形成的表项,不清掉会用到过期映射。
     */
    arch_tlb_invalidate_all();
    arch_icache_invalidate_all();

    arch_dsb();
    arch_isb();
    HB[HB_SLOT_MMUSTAGE] = HB_MMU_STAGE_TABLE_SET;

    /*
     * 6. 打开地址转换。
     *
     *    ⚠ 下一行执行之后,取指与访存就都要经过页表了。
     *    之所以还能继续往下跑,靠的是**恒等映射**:当前 PC、
     *    栈、以及串口/GIC 寄存器在页表里都映射到它们原本的物理地址。
     *    这也是为什么区域表里一个字面量错误都会表现为"打开就死"。
     */
    HB[HB_SLOT_MMUSTAGE] = HB_MMU_STAGE_ENABLING;

    sctlr = arch_read_sctlr();
    sctlr |= SCTLR_M;
    arch_write_sctlr(sctlr);

    arch_dsb();
    arch_isb();

    /*
     * 7. 到这里说明已经成功在地址转换下取指执行了。
     *    再往下还要做一次访存(HB 写在 OCM,被映射为不可缓存),
     *    所以这一句同时也验证了数据访问路径。
     */
    HB[HB_SLOT_MMUSTAGE] = HB_MMU_STAGE_ON;
}
