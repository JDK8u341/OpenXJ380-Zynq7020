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

#include <arch/taskctx_asm.h>
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
    u32 ticks;       /* 本核私有定时器的 1kHz 中断次数 */
    u32 ipi_count;   /* 本核收到的 SGI(核间中断)次数 */
    u32 irq_count;   /* 本核处理过的中断总数 */
    u32 last_intid;  /* 本核最后处理的中断号 */
    u32 spin_retry;  /* 本核加锁重试次数(锁竞争诊断) */

    /*
     * ★ 地址字段一律用 `u32`,不用 `uintptr_t` / 指针 ★
     *
     * ARM 上地址就是 32 位,用 u32 是**如实**。但更重要的理由是:
     * 宿主的指针是 8 字节,只要这个结构体里出现一个指针宽度的字段,
     * 宿主上的布局就与目标板**不同** —— 而汇编按固定偏移访问它,
     * 于是"在宿主机上验证偏移"这件事就做不成了
     * (本项目已经在 heap_block_t 的 sizeof 上踩过一次同样的坑)。
     *
     * 用 u32 之后两个平台的布局一致,下面的 _Static_assert 在宿主上
     * 校验的就是**目标板的真实偏移**。
     */
    u32 stack_top; /* 本核栈区顶部,start.S 用的同一值 */

    /*
     * 本核当前正在跑的线程的**地址**(M4-6 加,M4-7 开始用)。
     *
     * 对应 x86 的 `PROCESSOR_INFO.current_task`(`include/smp/smp.h:43`)。
     * x86 按 `%gs:0x4c0` 访问它,而那个偏移**没有任何 static_assert**
     * (见计划 §4.7.6)。ARM 用 TPIDRPRW 拿到本结构体指针之后按
     * `ARM_PERCPU_OFF_CURRENT_TASK` 访问 —— 由下面的断言钉住,
     * 而且宿主上验的就是目标偏移。
     *
     * 类型是 u32(地址)而不是 `tcb_t`:见上面那条规则。
     * 取用处统一走 `percpu_task_ptr()` 做转换,不要到处写强制转换。
     */
    u32 current_task;

    /*
     * ---- 调度器状态(M4-8)----
     *
     * ⚠ 这三项也必须是**定宽或 u32 地址**,不能是指针:
     *   本结构体的宿主机布局必须与目标板一致,理由见上面那条规则。
     *   就绪队列的首节点于是存 **TCB 的地址**而不是指针。
     *
     * `scheduler_ticks` 是 64 位累加量(u64 在两个平台上都是 8 字节对齐),
     * 所以它不破坏这条不变式。
     */
    /*
     * ⚠ 这里**故意不存**就绪队列的头与长度。
     *
     * 一开始加了 `sched_head` / `sched_count`,后来去掉了:它们是队列本身的
     * **派生量**,存两份就等于给了两个真相来源 —— 本项目已经因为
     * "派生量忘了重算"吃过一次亏(heap 的 largest_free,见计划 §0.5.5 第 13 条),
     * 而那种错误的形态是**静默失真**:数字看着合理,只是不对。
     *
     * 就绪队列本体在 sched_kern.c 里(每核一个 `sched_queue_t`)。
     * 要在 JTAG 上看队列,读那个静态数组的地址即可。
     */
    /*
     * 本核的调度时间片计数。
     *
     * ⚠ 它**不是一个只增不减的累计量** —— 它就是源 OS 的
     *   `cpu->scheduler_ticks`(`scheduler.cpp:403`):current 每跑一个 tick
     *   加一,到 `TIME_SLICE` 就触发一次"该换人了",然后**被清零**;
     *   `sched_yield()` 也会把它直接设成 `TIME_SLICE` 来表达"这一片用完了"。
     *
     *   (这里原来写的是"本核已调度的 tick 数(诊断用)",那是 M4-8 的旧说法:
     *    当时它只被读取、从不参与判断。M4-9 起它是抢占的判据之一,
     *    累计量是 `percpu_t.ticks`。)
     */
    u64 scheduler_ticks;

    /*
     * ---- 当前线程的浮点现场在哪(M4-9.5)----
     *
     * ★ 为什么由 C 算好放进这里,而不是让汇编去查 TCB 的偏移 ★
     *
     * 存/取浮点现场必须在**任何 C 代码之前**与**所有 C 代码返回之后**做,
     * 因为 `sched_tick` 这类函数会不会碰 VFP 是**编译器说了算**的
     * (本内核里 GCC 已经用 `vldr d16,[pc]; vstr d16,[rN]` 做 64 位清零,
     *  见 arch/taskctx_asm.h 的说明)。于是那段汇编必须绕开 C 调用链,
     * 也就只能读**每核结构**的固定偏移。
     *
     * 而"TCB 里 `vfp`/`fpscr` 在哪"是本结构体**不允许**承担的知识
     * (TCB 有指针字段,宿主与目标布局不同)。两头一对,答案就是:
     * **C 侧在 `sched_set_current()` 里把地址算好写进来**,
     * 汇编只按下面两个固定偏移取。仍然是 u32 地址,不是指针。
     */
    u32 cur_vfp_d; /* → 当前线程 TCB 里的 vfp[ARM_VFP_D_REGS * 2],0 = 不处理 */
    u32 cur_vfp_f; /* → 同一 TCB 里的 fpscr */
} percpu_t;

/* 把 current_task 取成 tcb_t。集中在一处,避免散落的强制转换 */
struct arm_thread_control_block;
#define percpu_task_ptr(p) ((struct arm_thread_control_block *)(uintptr_t)((p)->current_task))

/* ------------------------------------------------------------------ */
/* ★ 把汇编用到的偏移钉死 ★                                           */
/* ------------------------------------------------------------------ */

/*
 * 这一条是补 x86 的短板:x86 的 per-CPU 偏移(handler.S:29-31)
 * 被汇编硬编码却零编译期保护。ARM 侧加一个字段忘了改汇编,在这里就会报错。
 */
_Static_assert(offsetof_arm(percpu_t, current_task) == ARM_PERCPU_OFF_CURRENT_TASK,
               "current_task 的偏移变了 —— 同步改 arch/taskctx_asm.h 的 ARM_PERCPU_OFF_CURRENT_TASK");
_Static_assert(offsetof_arm(percpu_t, cpu_id) == ARM_PERCPU_OFF_CPU_ID,
               "cpu_id 的偏移变了 —— 同步改 arch/taskctx_asm.h 的 ARM_PERCPU_OFF_CPU_ID");
_Static_assert(offsetof_arm(percpu_t, cur_vfp_d) == ARM_PERCPU_OFF_CUR_VFP_D,
               "cur_vfp_d 的偏移变了 —— 同步改 arch/taskctx_asm.h 的 ARM_PERCPU_OFF_CUR_VFP_D");
_Static_assert(offsetof_arm(percpu_t, cur_vfp_f) == ARM_PERCPU_OFF_CUR_VFP_F,
               "cur_vfp_f 的偏移变了 —— 同步改 arch/taskctx_asm.h 的 ARM_PERCPU_OFF_CUR_VFP_F");

/*
 * ★ 布局不变式:本结构体里**不能出现指针宽度的字段** ★
 *
 * 宿主的指针是 8 字节、目标是 4 字节;只要有一个,宿主上的偏移就与目标不同 ——
 * 而"在宿主机上验证汇编用的偏移"正是本项目对 M4-6 定下的做法。
 * 所以下面这几条断言不只是钉数值,它们钉的是**这条不变式仍然成立**:
 * sizeof 必须等于"全部字段按 u32 排下来"的结果。
 */
_Static_assert(offsetof_arm(percpu_t, scheduler_ticks) == 48u,
               "scheduler_ticks 应当 8 字节对齐到 48");
_Static_assert(sizeof(percpu_t) == 64u,
               "sizeof(percpu_t) 不是 64 —— 多半是有人加了指针/uintptr_t 字段,"
               "那会让宿主与目标的布局分叉");

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
percpu_t *percpu_init_self(u32 stack_top);

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
