#pragma once

/*
 * 每 CPU 数据(AM3-1)
 *
 * ── 为什么需要它 ──────────────────────────────────────────────────────
 * 单核阶段所有状态都是全局变量就够用。一旦有第二个核,**任何"本核私有"
 * 的东西都必须按核索引**,否则两核会互相覆盖。典型受害者:
 *   - 当前 tick 计数(两核各自 1kHz,合起来必须还是 1kHz 的墙钟)
 *   - 本核中断统计(EOI 必须配对自己 acknowledge 到的那个)
 *   - 当前任务/栈指针(将来调度器的基础)
 *
 * ── 为什么用 TPIDRPRW 而不是数组下标 ─────────────────────────────────
 * 也可以用 arch_cpu_id() + 全局数组,但那样每次访问都要一条 MPIDR 读
 * 加一次数组索引。TPIDRPRW 是 CP15 里的**每核寄存器**(banked),
 * 一次 mrc 就拿到自己的结构体指针 —— 这正是 ARM 相对 x86 的便利:
 * x86 要靠 swapgs + KERNEL_GS_BASE 才能达到同样效果。
 *
 * ⚠ TPIDRPRW 是 PL1 专属(名字里的 PRW = PL1 Read/Write),
 *   所以用户态动不了它,不需要额外保护。
 *
 * ── 宿主可测性 ────────────────────────────────────────────────────────
 * 本模块刻意分成两层,沿用 mmu.c/mmu_hw.c 的做法:
 *   src/percpu.c     纯逻辑(表、索引校验、统计),**不碰 CP15**,宿主可测
 *   src/percpu_hw.c  只做 CP15 读写与 MPIDR 取号
 * 这样"越界 id 不能写坏别人"这类最容易出错的地方能在宿主机上钉住。
 */

#include <arch/types.h>

/*
 * 最大核数。Cortex-A9 MPCore 最多 4 核,但本板是双核 Zynq-7020。
 * 留成可配的宏而不是硬编码 2,是因为数组大小要参与静态断言。
 */
#define PERCPU_MAX_CPUS 2u

typedef struct
{
    u32 cpu_id;      /* 本结构体所属的核号 */

    /*
     * 上线标志。由**本核自己**在完成全部初始化后置 1。
     * 由别的核代写是错的:那只能证明"CPU0 觉得 CPU1 该起来了",
     * 证明不了 CPU1 真的跑到了这里。
     */
    u32 online;

    u32 mpidr;       /* 本核读到的 MPIDR 原始值(诊断用) */
    u32 loops;       /* 本核主循环计数 */
    u32 irq_count;   /* 本核处理过的中断数 */
    u32 last_intid;  /* 本核最后处理的中断号 */
    u32 spin_retry;  /* 本核加锁重试次数(锁竞争诊断) */

    uintptr_t stack_top; /* 本核栈区顶部,start.S 用的同一值 */
} percpu_t;

extern percpu_t g_percpu[PERCPU_MAX_CPUS];

/* ------------------------------------------------------------------ */
/* 纯逻辑接口(宿主可测)                                               */
/* ------------------------------------------------------------------ */

/* 核号是否在表范围内 */
bool percpu_id_valid(u32 cpu_id);

/*
 * 取指定核的结构体。越界返回 NULL ——
 * 调用方必须处理 NULL,而不是拿到一个越界指针写下去。
 */
percpu_t *percpu_for(u32 cpu_id);

/* 只清表,不碰任何硬件。启动时由 CPU0 调用一次 */
void percpu_table_reset(void);

/* 已上线的核数 */
u32 percpu_online_count(void);

/* ------------------------------------------------------------------ */
/* 硬件相关(只能在目标板上跑)                                        */
/* ------------------------------------------------------------------ */

/*
 * 本核初始化:读 MPIDR 得到核号,把 g_percpu[cpu] 的地址写进 TPIDRPRW。
 * 返回本核的结构体;核号越界或已初始化过则返回 NULL。
 * 必须在使用任何 percpu_self() 之前调用。
 */
percpu_t *percpu_init_self(uintptr_t stack_top);

/* 取本核结构体(TPIDRPRW)。未初始化时返回 NULL */
percpu_t *percpu_self(void);

/*
 * 把本核的身份(cpu_id / mpidr)与 online 标志写进自己的表项。
 *
 * ⚠ 只能在**本核缓存已使能**之后调用 —— 缓存关着时的写入不参与一致性,
 *   会被别的核早先留下的脏行覆盖。见 src/percpu_hw.c 的说明。
 *
 * 与 percpu_init_self() 分开是有意的:后者只碰 CP15(TPIDRPRW),
 * 不写任何共享内存,所以可以在缓存还没开的时候安全调用。
 */
void percpu_publish_self(void);
