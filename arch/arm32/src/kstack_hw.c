/*
 * 内核栈池的 TLB 维护 —— CP15 部分(M4-5)
 *
 * 为什么单独一个文件:src/kstack.c 要能被宿主 gcc 直接编译来做单测,
 * 一旦引入 mcr/mrc 内联汇编就编不过了。这与 mmu.c/mmu_hw.c、
 * percpu.c/percpu_hw.c、cache.c/cache_hw.c 是同一条分层线。
 *
 * ★ 为什么 TLB 维护值得单独一个文件而不是"让调用方顺手做" ★
 *
 * 见 include/arch/kstack.h 顶部:改完页表忘了失效 TLB 的后果不是"慢一点",
 * 而是 **guard 静默失效**。拆段之前那一段是**段映射**,TLB 里留着段表项,
 * 于是 guard 页照样能访问,而页表里看上去完全正确 —— 没有任何下游
 * 错误可查。所以这件事由模块自己拥有(通过 kstack_flush_fn 钩子),
 * 调用方只需把本函数交给 kstack_pool_init。
 */

#include <arch/cpu.h>
#include <arch/kstack.h>

/*
 * 左闭右开区间逐页失效。
 *
 * 用 MVA 形式(`TLBIMVA`)而不是整表失效(`TLBIALL`),两个原因:
 *
 *   1. 两个核的 TLB 是各自的,整表失效也只清本核,并不能解决跨核问题
 *      (跨核要靠 IPI 或 TLBIMVAA 广播,见下);
 *   2. 整表失效会把启动以来建立的映射全部打掉,下一次访存要重走
 *      4 级表遍历 —— 在"只是拆了一段"这种局部改动上代价不成比例。
 *
 * ⚠ 已知限制(留给 M4-7):`arch_tlb_invalidate_page` 只作用于**本核**。
 *   现在栈只由 CPU0 使用,所以够用。等任务真的跑在 CPU1 上时,
 *   CPU1 那边可能还留着启动阶段形成的段表项,那时 guard 对它是失效的。
 *   届时的修法是 TLBIMVAA 广播或让对端自己刷一遍 —— **不是"到时候再说",
 *   而是"现在还没轮到"**,所以在这里写明。
 */
void kstack_tlb_flush_range(u32 va_begin, u32 va_end)
{
    u32 va;

    for (va = va_begin; va < va_end; va += KSTACK_PAGE_SIZE) {
        arch_tlb_invalidate_page((uintptr_t)va);
    }

    /*
     * arch_tlb_invalidate_page 内部已经各带一次 DSB/ISB,这里再补一次
     * 是为了覆盖**整个区间**作为一个整体:循环结束之后要保证所有
     * 表项改动对后续取指/访存可见,而不是"逐页各自可见"。
     */
    arch_dsb();
    arch_isb();
}
