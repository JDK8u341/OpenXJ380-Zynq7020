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

/*
 * 自旋等待提示。
 *
 * ★ `yield` 正是 x86 `pause` 的 ARM 对应物 ★ —— 两者都是**提示**指令
 *   (让出流水线 / 给 SMT 兄弟线程),**都不会让核心停下来**。
 *   源 OS 的 idle 用的就是 `pause`(见下面 `arch_wfi` 的说明)。
 */
static inline void cpu_relax(void)
{
    __asm__ volatile("yield" ::: "memory");
}

/*
 * 停下来等中断。**不是 idle 用的。**
 *
 * ## ★ 这个注释原先写错了,已按源 OS 改正 ★
 *
 * 原文是:"低功耗等待(**x86 的 hlt 对应物**)。…… **M4-8 的 idle 线程需要它**"。
 * 两句都不成立,而且第二句里的那个 idle 线程**已经被按源 OS 拆掉了**:
 *
 *   1. **源 OS 的 idle 不是 `hlt`,是 `pause` 忙等** ——
 *      BSP:`kernel/main.cpp:622-631` 的收尾循环体是 `__asm__ volatile("pause")`;
 *      AP :`kernel/smp/smp.cpp:178-183` 同样是 `while (true) asm volatile("pause")`
 *          (它下面那句 `hlt` 是不可达的防御:注释写着"AP 若意外返回启动路径才停")。
 *      ⇒ **源 OS 里没有任何"让 CPU 停下来等中断"的机制**,WFI 在那儿没有对应物。
 *
 *   2. `hlt` 在源 OS 里的含义是**"永久停住"**,全部出现在终结路径上:
 *      `driver/power.cpp:250`/`:310`(关机/重启兜底)、`kernel/memory/page.cpp:477`(OOM)、
 *      `kernel/smp/smp.cpp:98`/`:310`(启动失败)、`kernel/main.cpp:524`(idle 分配失败)、
 *      `kernel/task/pcb.cpp:503-504`(**内核线程退出**后的兜底)。
 *
 * ⇒ 所以 `wfi` 的**正当用途只有一个**:一条线程真的结束了、要永久停在那里 ——
 *   那对应的是源 OS 的 `pcb.cpp:503-504`(`kill_thread` 之后 `while (true) hlt`),
 *   **不是 idle**。
 *
 * ⚠ 至于"要不要让 idle 进 WFI":那不是"加一条指令",而是**引入一个源 OS 没有的
 *   低功耗机制**。而且 BSP 的 idle **没有实体循环**(idle 就是启动流程自己,
 *   `ctx.pc = 0`,只被切走、不被切进去),在那儿加 WFI 等于**改 idle 的模型** ——
 *   正是 M4-8 造过、后来按源 OS 拆掉的那个东西。
 *   唯一"不改模型"的落点是**每核 idle 的实体循环**(ARM 侧 CPU1 已有:
 *   `smp.c:219-223` 的 `for(;;){ loops++; …; cpu_relax(); }`),把它换成 `wfi`
 *   是一次局部、可 A/B 的偏离 —— 要做得单独拍板并记进偏离清单。
 */
static inline void arch_wfi(void)
{
    __asm__ volatile("wfi" ::: "memory");
}

static inline void arch_wfe(void)
{
    __asm__ volatile("wfe" ::: "memory");
}

/*
 * 等事件:进入低功耗直到 SEV/中断。
 *
 * ⚠ 原文写的是"等价于 x86 的 hlt" —— 也不准。`hlt` 在源 OS 里是**永久停住**
 *   (终结路径),而 `wfe` 是**等到有人发事件就继续**,两者语义相反。
 *   `wfe` 在源 OS 里的真正对应物是**启动握手时的忙等轮询**
 *   (`kernel/main.cpp:581-585` 的 `while (true) { pause; if (scheduler_is_ready ==
 *   xsi->cpu_count) break; }`)—— 那边用 `pause` 反复查,这边用 `wfe` 被 SEV 叫醒。
 */
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

/*
 * SCTLR(System Control Register)常用位。
 *
 * 住在这里而不是 mmu.h:它是 CPU 的系统控制寄存器,不只服务 MMU ——
 * M 位是地址转换使能,C/I 位是缓存使能,两边都要用。
 * 放在 cpu.h 可以让缓存模块不必为了一个位定义去包含 MMU 头文件。
 *
 * Xilinx boot.S 用的初值是 0b01000000000101 = 0x4005,
 * 即 M(MMU) + C(D-Cache) + RR,注意**不含 I(I-Cache)**。
 */
#define SCTLR_M   (1u << 0)   /* MMU 使能 */
#define SCTLR_A   (1u << 1)   /* 地址对齐检查 */
#define SCTLR_C   (1u << 2)   /* 数据缓存使能 */
#define SCTLR_Z   (1u << 11)  /* 分支预测使能 */
#define SCTLR_I   (1u << 12)  /* 指令缓存使能 */
#define SCTLR_V   (1u << 13)  /* 异常向量基址:0=VBAR,1=0xFFFF0000 */
#define SCTLR_RR  (1u << 14)  /* 缓存替换策略:1=轮转 */
#define SCTLR_U   (1u << 22)  /* 不对齐访问使能(ARMv7 应置 1) */
#define SCTLR_XP  (1u << 23)  /* 异常向量:0=ARM 态 */
#define SCTLR_EE  (1u << 25)  /* 异常字节序:0=小端 */

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

/*
 * ACTLR(Auxiliary Control Register, CP15 c1,c0,1)。
 *
 * ⚠ 这个寄存器在移植时极易被整个忘掉 —— 它不影响地址转换,
 *   漏了也不会报任何错,只表现为"缓存开了但完全没有加速"。
 *   本项目就踩过:见 cortexa9_actlr_init() 的说明。
 */
static inline u32 arch_read_actlr(void)
{
    u32 value;
    __asm__ volatile("mrc p15, 0, %0, c1, c0, 1" : "=r"(value));
    return value;
}

static inline void arch_write_actlr(u32 value)
{
    __asm__ volatile("mcr p15, 0, %0, c1, c0, 1" ::"r"(value) : "memory");
    arch_isb();
}

/*
 * ACTLR 的位。
 *
 * bit6(SMP)决定了本核是否参与 SCU 一致性 —— 它直接影响
 * **L1 能不能缓存 Shareable 内存**:DDR 在区域表里是 S=1,
 * 而 SMP 位没置时那些访问不会进 L1,表现为"缓存开着但没加速"。
 *
 * bit0(FW)是缓存/TLB 维护操作的广播。单核阶段置位看着没用,
 * 但它定义的是维护操作的行为;等 M4 加第二个核时少了它会出现
 * "清了缓存但别的核看不见",而那时很难联想到是启动阶段漏了一位。
 */
#define ACTLR_FW            (1u << 0) /* 缓存/TLB 维护广播 */
#define ACTLR_SMP           (1u << 6) /* 参与 SCU 一致性 */

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
    /* ICIALLU */
    __asm__ volatile("mcr p15, 0, %0, c7, c5, 0" ::"r"(0) : "memory");
    arch_dsb();
    arch_isb();
}

/* ------------------------------------------------------------------ */
/* 缓存几何(CLIDR / CCSIDR / CSSELR)                                   */
/* 注意这三个的 opcode2 与其它 CP15 寄存器不同,是 mrc p15, 1/2, ...  */
/* ------------------------------------------------------------------ */

/* Cache Level ID Register:各级缓存类型 + LoC/LoUIS */
static inline u32 arch_read_clidr(void)
{
    u32 value;
    __asm__ volatile("mrc p15, 1, %0, c0, c0, 1" : "=r"(value));
    return value;
}

/* Cache Size ID Register:当前 CSSELR 选中那一级的几何 */
static inline u32 arch_read_ccsidr(void)
{
    u32 value;
    __asm__ volatile("mrc p15, 1, %0, c0, c0, 0" : "=r"(value));
    return value;
}

/*
 * Cache Size Selection Register。
 * bit0 = 0 选数据缓存、1 选指令缓存;bits[3:1] = 级号(0 基)。
 */
static inline void arch_write_csselr(u32 value)
{
    __asm__ volatile("mcr p15, 2, %0, c0, c0, 0" ::"r"(value) : "memory");
    arch_isb();
}

static inline u32 arch_read_csselr(void)
{
    u32 value;
    __asm__ volatile("mrc p15, 2, %0, c0, c0, 0" : "=r"(value));
    return value;
}

/* ------------------------------------------------------------------ */
/* 按 set/way 的整块缓存操作                                           */
/* ------------------------------------------------------------------ */

/*
 * 这三个是把 cache_setway_value() 编码出来的值写进 CP15。
 *
 * "按 set/way"与"按地址(MVA)"是两套完全不同的操作,用途也不同:
 *   按地址 —— 针对某段缓冲区,DMA 前后用,粒度精确;
 *   按 set/way —— 针对整个缓存,只在**打开缓存之前**用,
 *                 目的是清掉复位后残留的未知内容。
 *
 * ⚠ 全志/全清整块缓存在多核下是不安全的(可能丢掉别的核的脏数据),
 *   所以本项目只在单核启动阶段用,见 src/cache_hw.c 的说明。
 */

/* DCISW:按 set/way 使数据缓存失效(不写回) */
static inline void arch_dcache_invalidate_setway(u32 value)
{
    __asm__ volatile("mcr p15, 0, %0, c7, c6, 2" ::"r"(value) : "memory");
}

/* DCCSW:按 set/way 把数据缓存清洗到下级(写回) */
static inline void arch_dcache_clean_setway(u32 value)
{
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 2" ::"r"(value) : "memory");
}

/* DCCISW:按 set/way 清洗并使数据缓存失效 */
static inline void arch_dcache_clean_invalidate_setway(u32 value)
{
    __asm__ volatile("mcr p15, 0, %0, c7, c14, 2" ::"r"(value) : "memory");
}

/* BPIALL:使整个分支预测器失效 */
static inline void arch_branch_predictor_invalidate_all(void)
{
    __asm__ volatile("mcr p15, 0, %0, c7, c5, 6" ::"r"(0) : "memory");
    arch_dsb();
    arch_isb();
}

/*
 * 按地址(MVA)的单行维护原语。
 *
 * 这里只提供**单个地址**的操作,不提供区间版本 ——
 * 区间版本必须做缓存行对齐(见 arch/cache.h 的 cache_align_range),
 * 而"对齐"这件事是有语义的、需要单测的逻辑,不适合塞在内联汇编旁边。
 * 区间接口在 src/cache_hw.c:cache_clean_range / invalidate_range /
 * clean_invalidate_range。
 *
 * 命名对照 CP15 助记符:
 *   DCCMVAC  (c7,c10,1)  清洗     —— 写回到一致性点,行保留
 *   DCIMVAC  (c7,c6,1)   失效     —— 丢弃,不写回
 *   DCCIMVAC (c7,c14,1)  清洗并使失效
 */

/* 把该行写回到一致性点 */
static inline void arch_dcache_clean_mva(uintptr_t va)
{
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" ::"r"(va) : "memory");
}

/* 丢弃该行(不写回) */
static inline void arch_dcache_invalidate_mva(uintptr_t va)
{
    __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" ::"r"(va) : "memory");
}

/* 写回后丢弃 */
static inline void arch_dcache_clean_invalidate_mva(uintptr_t va)
{
    __asm__ volatile("mcr p15, 0, %0, c7, c14, 1" ::"r"(va) : "memory");
}

/*
 * ★ 中断状态是**锁的一部分**,不是调用方另拿一个变量 ★
 *
 * x86 的 `spin_t`(`include/cpu/lock.h:7-10`)里有一个 `rflags` 字段:
 * `spin_lock()` 自己把中断状态存进去并关中断,`spin_unlock()` 自己恢复 ——
 * 调用方**不需要**记住任何东西。ARM 侧必须沿用同一个契约,
 * 否则所有调用点都要改(§4.7 的 A4/A5 两条)。
 *
 * ⚠ 与 x86 的实际差异只在"存什么":x86 存 RFLAGS(64 位,其中 IF 位才是关键),
 *   ARM 存 CPSR 的控制域(32 位,I/F 位才是关键)。契约相同,载体不同 ——
 *   所以字段名也叫 `cpsr` 而不是 `rflags`,别假装两边一样。
 */
typedef struct
{
    volatile u32 locked;
    u32          cpsr; /* 持锁期间应当恢复的中断状态(不透明令牌)*/
} spin_t;

/*
 * 静态初始化。**两个字段都要给** —— 只给 `locked` 会让 `cpsr` 是 0。
 *
 * ⚠ 这里原来写的是"0 在 CPSR 里表示'中断全开 + User 模式',恢复它会把
 *   核心拖进用户态"。**那是错的**,已按架构改正:
 *
 *   AArch32 的 CPSR.M[4:0] 里,**每一个已分配的模式编码,bit4 都是 1** ——
 *   USR 0b10000、FIQ 0b10001、IRQ 0b10010、SVC 0b10011、MON 0b10110、
 *   ABT 0b10111、HYP 0b11010、UND 0b11011、SYS 0b11111
 *   (本项目的表就在 arch/taskctx_asm.h,一眼可核)。
 *
 *   `0` 的 M[4:0] 是 **0b00000** —— bit4 = 0,所以它**不是任何一个模式**,
 *   而是一个未分配的编码。按架构,把这样一个值装回 CPSR 是
 *   **UNPREDICTABLE**。
 *
 * 于是真正的理由不是"它会变成用户态",而是:**它什么都不是**。
 * spin_lock/spin_unlock 的契约是"把持锁期间的中断状态原样恢复",
 * 而 0 不是一个可恢复的中断状态 —— 它是未定义行为。
 * 本项目**刻意没有去实测** Cortex-A9 遇到它会怎样:那是故意制造 UB,
 * 不是值得花上板时间的问题。
 */
#define SPIN_INIT {0, 0}

/* ← x86 `spin_init()` `lock.h:15` */
static inline void spin_init(spin_t *lock)
{
    lock->locked = 0u;
    lock->cpsr   = 0u;
}

/* ← x86 `barrier()` `lock.h:95`:两侧都要 —— 编译器屏障管重排,
 *   DSB 管硬件访存完成顺序(ARMv7 是弱内存序,x86 不是)*/
static inline void barrier(void)
{
    __asm__ volatile("" ::: "memory");
    arch_dsb();
}

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

/* 只自旋,不碰中断状态 —— 给"调用方已经关了中断"的场合用。
 * ← x86 `spin_lock_no_irqsave()` `lock.h:52` */
static inline void spin_lock_no_irqsave(spin_t *lock)
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

/* 只释放,不恢复中断状态 ← x86 `spin_unlock_no_irqstore()` `lock.h:85` */
static inline void spin_unlock_no_irqstore(spin_t *lock)
{
    /* 释放屏障:保证临界区访存先于解锁可见 */
    arch_dmb();
    lock->locked = 0u;
    arch_dsb();
}

/*
 * 加锁:(1) 先把当前中断状态存进锁里 (2) 关中断 (3) 再自旋。
 *
 * ⚠ 顺序不能反。"先关中断再自旋"看起来更自然,但那样自旋期间的中断状态
 *   就没被保存 —— 解锁时也就无从恢复,只能一律开中断,
 *   于是"调用方本来就关着中断"的嵌套情形会被破坏。
 */
static inline void spin_lock(spin_t *lock)
{
    lock->cpsr = arch_irq_save(); /* 保存并关中断 */
    spin_lock_no_irqsave(lock);
}

/*
 * 解锁:恢复加锁时保存的中断状态。
 *
 * ★ 必须在**释放锁之前**把 cpsr 读出来 ★
 *
 * 这是 x86 侧特意写了一句注释的地方(`include/cpu/lock.h:67-68`)。
 * 原因:一旦 `locked` 被清零,另一个核可以立刻拿到锁,并把它**自己的**
 * 中断状态写进同一个 `cpsr` 字段;这时再去读,恢复的就是别人的状态 ——
 * 本核的中断可能被错误地重新打开,而临界区其实还没退出干净。
 */
static inline void spin_unlock(spin_t *lock)
{
    u32 flags = lock->cpsr; /* ★ 先读,再放锁 ★ */

    spin_unlock_no_irqstore(lock);
    arch_irq_restore(flags);
}
