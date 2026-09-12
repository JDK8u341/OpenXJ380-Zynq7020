#pragma once

/*
 * ARMv7-A / Cortex-A9 CPU 原语 —— 架构抽象层的一部分
 *
 * 对应移植计划 §2.1 的架构接缝。x86_64 侧的对应物是
 * include/cpu/lock.h、include/cpu/regio.h、kernel/cpu/common.cpp,
 * 那些都直接用了 pushfq/lock btsq/mfence 等 x86 指令。
 *
 * 这里的每个原语都只依赖 ARMv7-A 架构特性:
 *   - 中断屏蔽   : CPSR 的 I/F 位         (x86: cli/sti/pushfq)
 *   - 内存屏障   : DMB/DSB/ISB            (x86: mfence/sfence/lfence)
 *   - 低功耗等待 : WFE/WFI                (x86: hlt/pause)
 *   - 系统寄存器 : CP15 MRC/MCR           (x86: CRn / rdmsr)
 *   - 缓存维护   : CP15 c7 操作           (x86: wbinvd/clflush)
 */

#include <arch/io.h>
#include <arch/types.h>

/* ------------------------------------------------------------------ */
/* 低功耗与让出                                                         */
/* ------------------------------------------------------------------ */

/* 自旋等待提示。Cortex-A9 无 pause 指令,用 yield 提示流水线 */
static inline void cpu_relax(void)
{
    __asm__ volatile("yield" ::: "memory");
}

/* 等待事件:进入低功耗直到 SEV/中断。等价于 x86 的 hlt */
static inline void cpu_wfe(void)
{
    __asm__ volatile("wfe" ::: "memory");
}

/* 等待中断 */
static inline void cpu_wfi(void)
{
    __asm__ volatile("wfi" ::: "memory");
}

/* 向其它核发送事件(SMP 唤醒用,对应 x86 的 IPI) */
static inline void cpu_sev(void)
{
    __asm__ volatile("sev" ::: "memory");
}

/* ------------------------------------------------------------------ */
/* 中断屏蔽                                                             */
/* ------------------------------------------------------------------ */

static inline u32 arch_read_cpsr(void)
{
    u32 value;
    __asm__ volatile("mrs %0, cpsr" : "=r"(value));
    return value;
}

static inline void arch_write_cpsr(u32 value)
{
    __asm__ volatile("msr cpsr_c, %0" ::"r"(value) : "memory");
}

static inline void arch_irq_disable(void)
{
    __asm__ volatile("cpsid i" ::: "memory");
}

static inline void arch_irq_enable(void)
{
    __asm__ volatile("cpsie i" ::: "memory");
}

/*
 * 保存中断状态并关闭中断,返回可传给 arch_irq_restore 的 CPSR 值。
 * 对应 x86 的 pushfq + cli。
 */
static inline u32 arch_irq_save(void)
{
    u32 flags;
    __asm__ volatile("mrs %0, cpsr\n\t"
                     "cpsid i"
                     : "=r"(flags)
                     :
                     : "memory");
    return flags;
}

/* 恢复 arch_irq_save 保存的 CPSR */
static inline void arch_irq_restore(u32 flags)
{
    __asm__ volatile("msr cpsr_c, %0" ::"r"(flags) : "memory");
}

static inline bool arch_irq_enabled(void)
{
    return (arch_read_cpsr() & (1u << 7)) == 0;
}

/* ------------------------------------------------------------------ */
/* CP15 系统寄存器                                                      */
/* ------------------------------------------------------------------ */

static inline u32 arch_read_sctlr(void)
{
    u32 value;
    __asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(value));
    return value;
}

static inline void arch_write_sctlr(u32 value)
{
    __asm__ volatile("mcr p15, 0, %0, c1, c0, 0" ::"r"(value) : "memory");
    arch_isb();
}

static inline u32 arch_read_ttbr0(void)
{
    u32 value;
    __asm__ volatile("mrc p15, 0, %0, c2, c0, 0" : "=r"(value));
    return value;
}

static inline void arch_write_ttbr0(u32 value)
{
    __asm__ volatile("mcr p15, 0, %0, c2, c0, 0" ::"r"(value) : "memory");
}

static inline u32 arch_read_ttbr1(void)
{
    u32 value;
    __asm__ volatile("mrc p15, 0, %0, c2, c0, 1" : "=r"(value));
    return value;
}

static inline void arch_write_ttbr1(u32 value)
{
    __asm__ volatile("mcr p15, 0, %0, c2, c0, 1" ::"r"(value) : "memory");
}

static inline u32 arch_read_dacr(void)
{
    u32 value;
    __asm__ volatile("mrc p15, 0, %0, c3, c0, 0" : "=r"(value));
    return value;
}

static inline void arch_write_dacr(u32 value)
{
    __asm__ volatile("mcr p15, 0, %0, c3, c0, 0" ::"r"(value) : "memory");
}

/* 故障地址:Cortex-A9 用 c6 而不是 x86 的 CR2 */
static inline u32 arch_read_dfar(void)
{
    u32 value;
    __asm__ volatile("mrc p15, 0, %0, c6, c0, 0" : "=r"(value));
    return value;
}

static inline u32 arch_read_dfsr(void)
{
    u32 value;
    __asm__ volatile("mrc p15, 0, %0, c5, c0, 0" : "=r"(value));
    return value;
}

/* 每 CPU 标识:MPIDR 低两位是 CPU 索引(Cortex-A9 最多 4 核) */
static inline u32 arch_read_mpidr(void)
{
    u32 value;
    __asm__ volatile("mrc p15, 0, %0, c0, c0, 5" : "=r"(value));
    return value;
}

static inline u32 arch_cpu_id(void)
{
    return arch_read_mpidr() & 0x3u;
}

/*
 * 每 CPU 数据的基址寄存器 TPIDRPRW。
 * 这是 ARM 侧特有的便利:x86 要靠 swapgs + KERNEL_GS_BASE 折腾,
 * 而 TPIDRPRW 是 banked 的,特权态读写不会影响用户态。
 */
static inline u32 arch_read_percpu(void)
{
    u32 value;
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 4" : "=r"(value));
    return value;
}

static inline void arch_write_percpu(u32 value)
{
    __asm__ volatile("mcr p15, 0, %0, c13, c0, 4" ::"r"(value) : "memory");
}

/* 用户态 TLS 指针(对应 x86 的 %fs base) */
static inline u32 arch_read_user_tls(void)
{
    u32 value;
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(value));
    return value;
}

static inline void arch_write_user_tls(u32 value)
{
    __asm__ volatile("mcr p15, 0, %0, c13, c0, 3" ::"r"(value) : "memory");
}

/* ------------------------------------------------------------------ */
/* TLB 与缓存维护                                                       */
/* 注意:这里是精简实现,仅覆盖单核/启动阶段的需求。                   */
/*      DMA 要用到的粗粒度缓存维护应改用经过验证的实现,            */
/*      因为 Cortex-A9 与 PL310 有一批勘误(775420/588369/727915)。  */
/* ------------------------------------------------------------------ */

/* 使整个 TLB 失效 */
static inline void arch_tlb_invalidate_all(void)
{
    __asm__ volatile("mcr p15, 0, %0, c8, c7, 0" ::"r"(0) : "memory");
    arch_dsb();
    arch_isb();
}

/* 使单个虚拟地址的 TLB 项失效 */
static inline void arch_tlb_invalidate_page(uintptr_t va)
{
    __asm__ volatile("mcr p15, 0, %0, c8, c7, 1" ::"r"(va) : "memory");
    arch_dsb();
    arch_isb();
}

/* 使整个指令缓存失效 */
static inline void arch_icache_invalidate_all(void)
{
    __asm__ volatile("mcr p15, 0, %0, c7, c5, 0" ::"r"(0) : "memory");
    arch_dsb();
    arch_isb();
}

/* 按地址清洗数据缓存到一致性点(DCCMVAC) */
static inline void arch_dcache_clean_range(uintptr_t start, uintptr_t end)
{
    for (uintptr_t addr = start; addr < end; addr += 32) {
        __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" ::"r"(addr) : "memory");
    }
    arch_dsb();
}

/* 按地址使数据缓存失效(DCIMVAC)。用于 DMA 接收缓冲 */
static inline void arch_dcache_invalidate_range(uintptr_t start, uintptr_t end)
{
    for (uintptr_t addr = start; addr < end; addr += 32) {
        __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" ::"r"(addr) : "memory");
    }
    arch_dsb();
}

/* 按地址清洗并使数据缓存失效(DCCIMVAC) */
static inline void arch_dcache_flush_range(uintptr_t start, uintptr_t end)
{
    for (uintptr_t addr = start; addr < end; addr += 32) {
        __asm__ volatile("mcr p15, 0, %0, c7, c14, 1" ::"r"(addr) : "memory");
    }
    arch_dsb();
}

/* ------------------------------------------------------------------ */
/* 自旋锁                                                               */
/* ------------------------------------------------------------------ */

/*
 * ARMv7 用 LDREX/STREX 独占访问实现,而 x86 用 lock 前缀。
 * Cortex-A9 是弱内存序,解锁后必须 DMB 才能保证临界区访存
 * 对下一个持锁者可见 —— 这是与 include/cpu/lock.h 最大的语义差异。
 */
typedef struct
{
    volatile u32 locked;
} spin_t;

#define SPIN_INIT {0}

/*
 * 独占访问原语。
 * 用内联汇编而不是 __builtin_arm_ldrex/clang 的 __ldrex:
 * 前者是 clang 专有,后者是 GCC 专有,内联汇编两边都能编。
 */
static inline u32 arch_ldrex(volatile u32 *addr)
{
    u32 value;
    __asm__ volatile("ldrex %0, [%1]" : "=r"(value) : "r"(addr) : "memory");
    return value;
}

/* 返回 0 表示独占存储成功;非 0 表示失败需要重试 */
static inline u32 arch_strex(u32 value, volatile u32 *addr)
{
    u32 result;
    __asm__ volatile("strex %0, %2, [%1]" : "=r"(result) : "r"(addr), "r"(value) : "memory");
    return result;
}

static inline void spin_lock(spin_t *lock)
{
    u32 expected;

    do {
        do {
            expected = arch_ldrex(&lock->locked);
            cpu_relax();
        } while (expected != 0);

        /* 尝试从 0 换到 1;strex 返回非 0 表示失败,需要重试 */
    } while (arch_strex(1, &lock->locked) != 0);

    /*
     * 获取屏障:保证加锁之后的所有访存都发生在加锁之后。
     * ARMv7 没有 x86 那种隐式获取语义,必须显式 DMB。
     */
    arch_dmb();
}

static inline void spin_unlock(spin_t *lock)
{
    /* 释放屏障:保证临界区访存先于解锁可见 */
    arch_dmb();
    lock->locked = 0;
    arch_dsb();
}

/* 关中断版自旋锁:用于中断上下文可能竞争的临界区 */
static inline u32 spin_lock_irqsave(spin_t *lock)
{
    u32 flags = arch_irq_save();
    spin_lock(lock);
    return flags;
}

static inline void spin_unlock_irqrestore(spin_t *lock, u32 flags)
{
    spin_unlock(lock);
    arch_irq_restore(flags);
}
