/*
 * ARMv7-A / Zynq-7020 内核入口(M0 骨架)
 *
 * 对应 x86_64 侧的 KernelMain(kernel/main.cpp)。
 * 当前是移植计划 M0 阶段的骨架:目标是证明
 *   工具链 -> 构建系统 -> 链接 -> JTAG 直载 -> 板级可见输出
 * 这条链路在 ARM 侧成立。
 *
 * 与 x86 侧 KernelMain 的差异:
 *   - 不接收 FrameBufferConfig / EFI_SYSTEM_TABLE / BOOT_CONFIG
 *     (Zynq 上没有 UEFI,资源来自静态板级描述符,见 arch/platform.h)
 *   - 启动顺序:定时器 -> 串口自标定 -> 控制台 -> 外设
 *     而不是 x86 的 CPU -> IDT/GDT -> HPET/APIC -> ...
 *   - 通过 OCM 心跳向 JTAG 汇报状态,不依赖串口
 */

#include <arch/cache.h>
#include <arch/axi_gpio.h>
#include <arch/board.h>
#include <arch/board_devices.h>
#include <arch/console.h>
#include <arch/cpu.h>
#include <arch/fault_test.h>
#include <arch/heartbeat.h>
#include <arch/heap.h>
#include <arch/io.h>
#include <arch/irq.h>
#include <arch/kstack.h>
#include <arch/led.h>
#include <arch/mmu.h>
#include <arch/mutex.h>
#include <arch/palloc.h>
#include <arch/percpu.h>
#include <arch/platform.h>
#include <arch/sched.h>
#include <arch/selftest.h>
#include <arch/shell.h>
#include <arch/tcb.h>
#include <arch/smp.h>
#include <arch/timer.h>
#include <arch/vmap.h>
#include <arch/types.h>
#include <arch/uart_ps.h>

/*
 * CPU0 栈区顶部的链接符号(boot/kernel.ld)。
 * start.S 用同一个符号建 CPU0 的栈;这里读它是为了在 percpu 表里
 * 留下一条可核对的记录 —— 两个核的栈区必须不同。
 */
/*
 * M4-7:CPU0 的 SVC 栈区上下界(boot/kernel.ld)。
 * 异常现场帧必须落在这段区间里 —— 判据与理由见 9.5 那段。
 * 这两段与 IRQ 栈**不相交**,所以"落错地方"是二值的,糊不过去。
 */
extern char __stack_svc_bottom[];
extern char __stack_svc_top[];
extern char __stack_top[];

/*
 * 等 CPU1 上线的上限。
 *
 * 取 200ms:CPU1 要做的事(TPIDRPRW、MMU、SCU、L1)在 666MHz 上是
 * 微秒级的,这个值宽松了三个数量级。超过就不该再等 ——
 * "无限等"会把一个可诊断的降级变成整机挂死,本项目在 UART 轮询上
 * 已经踩过一次同样的坑。
 */
#define CPU1_BOOT_TIMEOUT_US 200000u

/*
 * 物理页分配器实例与其 bitmap(M4-2)。
 *
 * 放在这里而不是模块内部:bitmap 是 64KB,必须是**静态分配**的 ——
 * 分配器不能给自己分配存储,那是先有鸡还是先有蛋。
 * 64KB 在 .bss 里,落在内核镜像之内,所以不会被池自己发出去。
 */
u32      g_palloc_bitmap[PALLOC_BITMAP_WORDS];
palloc_t g_palloc;

/*
 * 内核堆(M4-3)。
 *
 * HEAP_PAGES 取 8192 页 = 32MB。一次要足的原因见上面 use 处的说明 ——
 * 增长区必须与堆区紧邻,而 palloc 不保证这一点。
 */
#define HEAP_PAGES 8192u

heap_t g_heap;

/*
 * M4-4:细粒度映射用的 L2 表池与实例。
 * 池必须 1KB 对齐(L1 描述符的低 10 位是表遍历属性,不是地址)。
 */
static u32    g_l2_pool[4 * VMAP_L2_ENTRIES] __attribute__((aligned(1024)));
static vmap_t g_vmap;

static u32 g_vmap_selftest;
static u32 g_vmap_split_ok;
static u32 g_vmap_live_ok;

/*
 * M4-5:内核栈池。
 *
 * ====================================================================
 * 每个栈 1MB —— **照搬源 OS**,不是拍脑袋定的
 * ====================================================================
 *
 *   include/proto.hpp:  #define CONFIG_KERNEL_TASK_STACK_SIZE 1048576
 *   kernel/task/pcb.cpp: 每个任务成对分配 kernel_stack 与 syscall_stack,
 *                        两个栈都用这个大小。
 *
 * 所以一个任务实占 2MB 栈,32 个栈 = 16 个任务。
 * 任务真要用**两个**栈这件事在 M4-6/M4-7 落地(那时 kstack_alloc 会被
 * 连着调两次);这里先把池按这个口径开出来,免得池小到要用时才返工。
 * "等用到再说"在这里的代价是改一个常量,而代价不在常量本身 ——
 * 在于池的分区方式一旦被别的代码依赖,改起来就不是改常量了。
 *
 * ====================================================================
 * 为什么单独一个 vmap 实例
 * ====================================================================
 *
 * `vmap_t` 的 va_begin/va_end 是一个**连续区间**,而 M4-4 那个演示实例
 * 已经把它的区间(测试区)用掉了。栈池的区间来自 palloc,地址与它无关,
 * 所以必须另开一个实例、另给一份 L2 表池。
 *
 * L2 表需求:池 32.125MB,最多跨 34 个 1MB 段,每段一张表,留 40 张。
 */
#define KSTACK_SLOTS       32u
#define KSTACK_STACK_PAGES 256u /* 1MB */
#define KSTACK_POOL_PAGES  (KSTACK_SLOTS * (KSTACK_STACK_PAGES + 1u))
#define KSTACK_L2_TABLES   40u

static u32    g_l2_pool_stack[KSTACK_L2_TABLES * VMAP_L2_ENTRIES] __attribute__((aligned(1024)));
static vmap_t g_vmap_stack;
static u32    g_kstack_bitmap[KSTACK_BITMAP_WORDS];

static kstack_pool_t g_kstack;
static kstack_t      g_kstack_probe;

/*
 * 第二、个**小**栈池,guard 用 AP=0b000 实现。
 *
 * ====================================================================
 * 为什么值得单独建一个池
 * ====================================================================
 *
 * `guard_kind` 是**池级**配置,所以两种 guard 必然是两套页表配置。
 *
 * 这个池存在的唯一目的就是把两件事在**真硬件**上钉死:
 *
 *   1. `KSTACK_GUARD_AP_NONE` 这条实现路径成不成立;
 *   2. ★ **DACR = client 模式下 AP 有没有被硬件执行** ★
 *
 * 第 2 条是 M2-4 欠下的一笔账。当时把 DACR 从全 manager 切成全 client,
 * 理由是"所有区域的 AP 都是 0b011(全权限),所以行为应当完全不变" ——
 * 那句话是**推理**,不是证据:AP 到底有没有被强制执行,当时无从观测。
 * 一个 AP=0b000 的页把它变成可观测的:同一页、同样映射着,
 * 只把 AP 从 0b011 改成 0b000,访问就必须从"成功"变成"权限故障"。
 *
 * ====================================================================
 * 为什么栈可以开得很小
 * ====================================================================
 *
 * 每个栈只要 2 页。guard 机制与栈有多大**无关** ——
 * 它只关心"栈底下面那一页可不可访问"。用 1MB 只会白占内存,
 * 而这个池不承担任何真实任务。
 */
#define KSTACK_AP_SLOTS       2u
#define KSTACK_AP_STACK_PAGES 2u
#define KSTACK_AP_POOL_PAGES  (KSTACK_AP_SLOTS * (KSTACK_AP_STACK_PAGES + 1u)) /* 6 页 */
#define KSTACK_AP_L2_TABLES   2u

static u32    g_l2_pool_ap[KSTACK_AP_L2_TABLES * VMAP_L2_ENTRIES] __attribute__((aligned(1024)));
static vmap_t g_vmap_ap;
static u32    g_kstack_ap_bitmap[KSTACK_BITMAP_WORDS];

static kstack_pool_t g_kstack_ap;
static kstack_t      g_kstack_ap_probe;

static u32 g_kstack_ap_ok;

/* M4-6:异常帧布局的运行时自检结果(-1 = 没跑过) */
static int g_svc_frame_check = -1;

/* M4-6:TCB 里 ctx 偏移的运行时自检结果(-1 = 没跑过) */
static int g_tcb_ctx_check = -1;

/* ---- M4-9:抢占(帧搬迁)的自检状态 ---- */
static u32 g_yield_a;
static u32 g_yield_b;
static u32 g_yield_ok;
static u32 g_yield_switches;
static u32 g_yield_local_ok;
static u32 g_sched_idle_ok;
static u64 g_tick_acc_delta;
static u32 g_tick_acc_ok;
static u32 g_tick_switched;    /* tick 里真的搬了帧的次数 */
static u32 g_tick_preempted;   /* 其中被换下的是真实线程的次数 */
static u32 g_tick_invalid_ctx;
static u32 g_idle_pc_was_zero;   /* 注册时 idle->ctx.pc == 0(还没被切走过)*/
static u32 g_cpsr_kernel_ok;     /* 手写的 ARM_CPSR_KERNEL 与板上实测的 CPSR 一致 */
static u32 g_cpsr_seen;          /* 板上实测的 CPSR(被抢占时收进 idle 的 ctx)*/
static u32 g_idle_pc_harvested;  /* 跑完之后 != 0(启动现场真的被收进了 ctx)*/
static u32 g_spin_ok;            /* 两个见证都成立 */
static u32 g_spin_sp_isolated;   /* 两个线程的 sp 都没掉出自己栈区 */
static u32 g_spin_both_done;
static u32 g_preempt_switches_ok;
/* M4-9.5:浮点现场 */
static u32 g_fp_ok;
static u32 g_fp_done;
static u32 g_fp_bad;
static u32 g_fp_rm_bad;
static u32 g_vfp_ctl_bad;
static u32 g_vfp_ctl_detected;
/* M4-8.4:周期状态线程 */
static u32 g_sleep_ok;
static u32 g_sleep_wakes_at;
static u32 g_sleep_late_max_at;
static u32 g_sleep_printed_at;
static u32 g_sleep_load_ok;
static u32 g_sleep_load_count;
static u32 g_status_ctl_wakes;
static u32 g_status_ctl_printed;
static u32 g_wake_ctl_detected;
/* 对照组(搬帧关掉)*/
static u32 g_reloc_ctl_a;
static u32 g_reloc_ctl_b;
static u32 g_reloc_ctl_detected;
static u32 g_reloc_ctl_inv_delta; /* ★ 对照组窗口内 invalid 的增长量(有上界)*/
static u32 g_reloc_ctl_inv_ok;    /* ★ 它在"派发次数"这个上界之内吗 */

/* ---- M4-10:两核调度的自检状态 ---- */
static u32 g_cpu1_sched_ready;   /* CPU1 的 idle + 队列都登记好了 */
static u32 g_smp_sched_ok;       /* 相 6:两个核都真的在跑线程 */
static u32 g_smp_ran[PERCPU_MAX_CPUS]; /* 每个核上"跑完了"的探针数 */
static u32 g_smp_assign[PERCPU_MAX_CPUS]; /* 每个核被分配到的探针数(TCB.cpu_id)*/
static u32 g_smp_cpu1_switched_at;
static u32 g_smp_cpu1_preempted_at;
static u32 g_smp_cpu0_preempted_at;
static u32 g_smp_made;              /* 相 6 真的造出来的线程数(非空转的判据之一)*/
static u32 g_smp_place_bad;         /* ★ 落在"不是更短的那个队列"上的线程数(应当 0)*/
static u32 g_smp_app_cpu;           /* ★ 应用级线程落在哪个核(必须是 0)*/
static u32 g_smp_app_created;
static u32 g_smp_fp_bad;            /* 相 6 探针里浮点图案被破坏的总次数(应当 0)*/
static u32 g_smp_fp_bad_cpu1;       /* ★ 其中落在 **CPU1** 上的那些(应当 0)*/
static u32 g_smp_fp_ck_cpu1;        /* ★ CPU1 上真的核对过多少次(非空转:必须 > 0)*/
static u32 g_smp_ap_idle_harvested; /* CPU1 的 idle 现场被收过(ctx.pc != 0)*/
static u32 g_smp_ap_idle_back;      /* ★ 线程挂起之后 CPU1 **回到了 idle**(loops 还在涨)*/
static u32 g_smp_ctl_detect;        /* A/B:置 pick_cpu0 之后 CPU1 一次都没切 */
static u32 g_smp_ctl_inconclusive;  /* ★ 相 6 没证明 CPU1 会调度 ⇒ 对照组无意义 */
static u32 g_smp_ctl_cpu1_delta;    /* 对照组窗口里 CPU1 的 switched 增量 */
static u32 g_smp_ctl_cpu0_delta;
#define YIELD_ROUNDS 8u

/*
 * ★ 探针线程的"干完活了" ★
 *
 * ← 源 OS `pcb.cpp:501-504`:
 *       kill_thread(get_current_task());
 *       open_interrupt;
 *       while (true) __asm__ volatile("hlt");
 *   也就是"把线程回收掉,然后在 `hlt` 上永久停住"。
 *   ARM 侧的对应物就是 `park` + `wfi` 死循环 —— **`wfi` 在这里是对的**
 *   (它的正当用途是"永久停住",不是"idle 省电";见 arch/cpu.h 的说明)。
 *
 * ⚠ 但本函数**目前不可达**(退化清单 D14):
 *   M4-11 之前没有线程退出机制,`sched_park_self()` 对一条真实线程
 *   **不会返回**(状态置 WAIT、`wakeup_time = 0`,再也不会被挑中)。
 *   所以下面那个循环是"到不了的第二道保险",留着是为了**语义完整**
 *   (线程不能从入口返回),而不是因为它有用。
 *
 *   ⇒ 它看起来像一项策略,其实不是。等 M4-11 有了 `kill_thread` 的对应物,
 *     这个函数才真的会被执行到。
 */
static void thread_finish(void)
{
    sched_park_self();
    for (;;) {
        arch_wfi();
    }
}

/*
 * 让出探针:各累加一个计数,然后主动让出。
 *
 * ★ M4-9 起"让出"不再是自己实现的一套切换 ★
 *
 * 旧版跑完之后用 `arch_ctx_switch(&g_sched_throw, &g_sched_return)` 手动交回
 * 测试点 —— 那是 M4-8 的权宜写法。现在 `sched_yield()` 走 `svc` 陷阱、
 * 与抢占同一条路径,于是线程只要**挂起自己**就够了:调度器挑不到任何
 * 可运行的任务时会兜底挑 idle,而 idle 就是启动流程 —— kmain 从被中断的
 * 那条指令继续,自检报告照常打。
 *
 * 它们**故意不做别的事**:这一步验的是"陷阱式让出 + 帧路径"能不能工作,
 * 混进业务只会让失败时不好归因。
 */
static void yield_probe_a(void *arg)
{
    u32 i;

    (void)arg;
    for (i = 0; i < YIELD_ROUNDS; i++) {
        g_yield_a++;
        sched_yield();
    }

    thread_finish(); /* 不再可调度 —— 这是启动流程能回来的前提 */
}

static void yield_probe_b(void *arg)
{
    u32 i;

    (void)arg;
    for (i = 0; i < YIELD_ROUNDS; i++) {
        g_yield_b++;
        sched_yield();
    }

    thread_finish();
}

/* ------------------------------------------------------------------ */
/* M4-9:抢占探针 —— **绝不主动让出**                                   */
/* ------------------------------------------------------------------ */

/*
 * ## 判据为什么不能是"两个计数都大于零"
 *
 * 那一条**区分不出抢占有没有生效**:没有抢占时,先跑的那个线程会一路跑完
 * 再挂起,然后另一个线程才开始跑 —— 两个计数照样都是正的。
 * (本项目已经不止一次因为"判据太松"白跑一轮上板。)
 *
 * 真正的区分点是**交错**,而交错是可以被线程自己见证的:
 *
 *     我在跑的时候,对方是不是已经跑过了?
 *
 * 无抢占时**第一个跑的线程永远见证不到** —— 它跑完之前对方一次都没动过。
 * 而"谁是第一个"不影响结论:两个见证必须**同时**为 1,
 * 先跑的那个必然是 0。有抢占时第一次时间片到点就会换人,两个见证都会成立。
 *
 * ## 另外两条同源判据
 *
 *   - **sp 自检**:线程每一轮都核对"我的 sp 在我自己的栈区里吗"。
 *     "帧搭在目标任务栈上"这件事一旦失效(比如搭在了被换下那个的栈上),
 *     违反的正是这条 —— 而那种错**不会报任何东西**,只会静默踩坏别人的栈。
 *   - **idle 的 ctx.pc 从 0 变成非 0**:说明启动流程的现场真的被收进了 ctx
 *     (只被切走、没被收现场的话它永远是 0),而且 kmain 真的被切回来了。
 */
typedef struct
{
    volatile u32       count;     /* 本轮跑了多少次 */
    volatile u32       saw_other; /* ★ 我跑的时候,对方已经跑过了吗 ★ */
    volatile u32       sp_bad;    /* sp 掉出自己栈区的次数(正常恒为 0)*/
    volatile u32       done;      /* 循环跑完了 */
    const volatile u32 *other;    /* 对方的 count */
    u32                budget_us; /* 跑多久墙钟(可配 —— M4-8.4 要一个跑几秒的)*/
} spin_probe_t;

#define SPIN_BUDGET_US 20000u /* 每个线程跑 20ms 墙钟(不是 20ms CPU 时间)*/

static spin_probe_t g_spin[2];
static spin_probe_t g_spin_ctl[2]; /* 对照组的两个 */

/*
 * 用**墙钟**而不是固定迭代次数来结束循环,是有意的:
 * 迭代次数依赖编译出来的指令数与 CPU 频率,一旦估小,线程就永远跑不完,
 * 于是启动流程再也回不来 —— 那是一个"自检把系统挂住"的失败模式,
 * 而上板调试恰恰最怕这个。墙钟预算则与这些都无关。
 *
 * ⚠ 读的是全局定时器(自由运行),不是 tick 计数 —— 被抢占的时间也算进去,
 *   所以"20ms"是真的 20ms,不会因为被换下而拖长。
 */
static void spin_probe(void *arg)
{
    spin_probe_t *p    = (spin_probe_t *)arg;
    tcb_t         self = sched_current();
    u64           t0   = timer_read_us();

    while ((timer_read_us() - t0) < (u64)p->budget_us) {
        u32 sp;

        p->count++;
        if (*p->other != 0u) {
            p->saw_other = 1u;
        }

        /*
         * ★ "这个线程跑在自己的栈上" ★
         *
         * 用 <= 与 > 的不对称区间:`kernel_stack` 是栈顶上界,初始 sp
         * **正好等于**它(满递减栈第一次写落在 top-4),所以上界取闭、
         * 下界取开。写成双闭会把初始那一轮误判成越界。
         */
        sp = arch_read_sp();
        if (self != NULL && (sp <= self->kstack_base || sp > self->kernel_stack)) {
            p->sp_bad++;
        }
    }

    p->done = 1u;
    thread_finish();
}

static void spin_probe_a(void *arg)
{
    spin_probe((void *)&g_spin[0]);
    (void)arg;
}

static void spin_probe_b(void *arg)
{
    spin_probe((void *)&g_spin[1]);
    (void)arg;
}

static void spin_ctl_a(void *arg)
{
    spin_probe((void *)&g_spin_ctl[0]);
    (void)arg;
}

static void spin_ctl_b(void *arg)
{
    spin_probe((void *)&g_spin_ctl[1]);
    (void)arg;
}

/* 把一对探针清零并互相指认 */
static void spin_reset(spin_probe_t *pair)
{
    u32 i;

    for (i = 0u; i < 2u; i++) {
        pair[i].count     = 0u;
        pair[i].saw_other = 0u;
        pair[i].sp_bad    = 0u;
        pair[i].done      = 0u;
        pair[i].other     = &pair[1u - i].count;
        pair[i].budget_us = SPIN_BUDGET_US;
    }
}

/*
 * 计费探针:独占 CPU 跑 20ms,量自己的 vruntime 涨了多少。
 *
 * 用**自己**的 vruntime 而不是"把 current 硬指到某个线程上":
 * 后者在帧路径上线之后是错的 —— 那等于让调度器把正在跑的 kmain 的现场
 * 收进那个线程的 ctx。判据的实现方式必须跟着机制走。
 *
 * ⚠ `timer_delay_us` 用全局定时器(自由运行),所以这 20ms 是墙钟。
 */
static void acc_probe(void *arg)
{
    tcb_t self = sched_current();
    u64   t0;

    (void)arg;

    if (self == NULL) {
        thread_finish();
    }

    t0 = self->eevdf_vruntime;
    timer_delay_us(20000u);
    g_tick_acc_delta = self->eevdf_vruntime - t0;

    thread_finish();
}

/* ------------------------------------------------------------------ */
/* M4-9.5:浮点现场探针                                                 */
/* ------------------------------------------------------------------ */

/*
 * ## 判据为什么长这样
 *
 * 要证明的是"**一个线程被切走再切回来,它的 d0-d31 与 FPSCR 还是原样**"。
 * 最直接的做法不是找一个 `double` 局部变量来看着它(那要赌编译器把它
 * 分配在寄存器里而不是栈上 —— 一旦它被溢出到栈,寄存器被踩了也看不出来),
 * 而是**直接把整个 d0-d31 填成自己的图案,然后反复读回来核对**:
 *
 *     d0-d31 <- 我的图案(只做一次)
 *     循环 { 读回 d0-d31;逐个与我的图案比;记录不符次数 }
 *     还要核对 FPSCR 的舍入模式(我的值还在吗)
 *
 * ⚠ "只填一次"是**判据成立的关键**,不是随手写的:
 *   如果每一轮都重填,那么这个线程被切回来时会**先把自己的图案写回去**,
 *   于是"寄存器被对方改过"这件事会被它自己抹掉 —— 判据永远通过,
 *   等于没判。只填一次,别人的图案一旦进来就再也出不去,必被检出。
 *
 * ⚠ 两个线程因此**不是对称的**:被换下之后又换回来的那一个会发现自己的
 *   寄存器变成了对方的(它读到了对方的图案);而对方因为从没读过、
 *   只是"自己的图案还在",往往是干净的。
 *   ⇒ 判据是"**至少有一个检出**",不是"两个都检出"。这一点写清楚,
 *     免得后人看到"只有一个 bad"以为判据不对。
 *
 * 探针自己也用 `arch_vfp_save` / `arch_vfp_restore`(生产函数)来读写
 * d0-d31 —— 于是这段自检顺带把这两个汇编原语也跑了一遍。
 */
#define FP_TAG_WORDS (ARM_VFP_D_REGS * 2u) /* 64 个 u32 = 32 个双字 = d0-d31 */
#define FP_BUDGET_US 20000u

typedef struct
{
    u32          pat[FP_TAG_WORDS];  /* 我的图案(要写进 d0-d31 的)*/
    u32          snap[FP_TAG_WORDS]; /* 读回来的一次快照 */
    u32          rmode;              /* 我的舍入模式(两个线程取不同值)*/
    volatile u32 bad;                /* 快照与图案不符的次数(正常恒为 0)*/
    volatile u32 rmode_bad;          /* 舍入模式不对的次数(正常恒为 0)*/
    volatile u32 done;
} fp_probe_t;

static fp_probe_t g_fp[2];
static fp_probe_t g_fp_ctl[2]; /* 对照组的两个 */

static void fp_reset(fp_probe_t *pair)
{
    u32 i;
    u32 j;

    for (i = 0u; i < 2u; i++) {
        for (j = 0u; j < FP_TAG_WORDS; j++) {
            /* 两个线程的图案必须**一眼可分**,否则"读到对方的"看不出来 */
            pair[i].pat[j]  = ((i == 0u) ? 0xA5A50000u : 0x5A5A0000u) + j;
            pair[i].snap[j] = 0u;
        }
        pair[i].rmode     = (i == 0u) ? 0u : 3u; /* 0 = 就近舍入,3 = 向零舍入 */
        pair[i].bad       = 0u;
        pair[i].rmode_bad = 0u;
        pair[i].done      = 0u;
    }
}

static void fp_probe(void *arg)
{
    fp_probe_t *p  = (fp_probe_t *)arg;
    u64         t0;
    u32         fpscr_dummy;
    u32         j;

    /* d0-d31 <- 我的图案;FPSCR <- 干净值,再设成我的舍入模式 */
    arch_vfp_restore(p->pat, 0u);
    arch_vfp_set_rmode(p->rmode);

    t0 = timer_read_us();
    while ((timer_read_us() - t0) < (u64)FP_BUDGET_US) {
        arch_vfp_save(p->snap, &fpscr_dummy);

        for (j = 0u; j < FP_TAG_WORDS; j++) {
            if (p->snap[j] != p->pat[j]) {
                p->bad++;
                break;
            }
        }

        if (arch_vfp_get_rmode() != p->rmode) {
            p->rmode_bad++;
        }
    }

    p->done = 1u;
    thread_finish();
}

static void fp_probe_a(void *arg)
{
    fp_probe((void *)&g_fp[0]);
    (void)arg;
}

static void fp_probe_b(void *arg)
{
    fp_probe((void *)&g_fp[1]);
    (void)arg;
}

static void fp_ctl_a(void *arg)
{
    fp_probe((void *)&g_fp_ctl[0]);
    (void)arg;
}

static void fp_ctl_b(void *arg)
{
    fp_probe((void *)&g_fp_ctl[1]);
    (void)arg;
}

/* ------------------------------------------------------------------ */
/* ★ M4-8.4:周期状态行 —— 第一个会睡会醒的真实负载 ★                  */
/* ------------------------------------------------------------------ */

/*
 * ⚠ 这几个东西的定义在文件靠后处(或原本是 kmain 的局部变量),而状态线程
 *   要读它们 —— 所以在这里先**定义**。
 *   把 `loop_count` / `uart_present` 从 kmain 的局部变量提成文件级静态量
 *   不是图省事:线程函数没法访问另一个函数的局部变量,而"状态行要报什么"
 *   本来就该是能被告知的。它们只在 kmain 里读写,提升之后语义不变
 *   (kmain 是唯一写者,线程只读)。
 */
static volatile u32 g_tick_seen;

/* 中断延迟测量的两个量 —— 定义在下面 tick 探针那一段,这里先声明 */
static volatile u32 g_tick_max_gap;

/* kmain 的局部量,提升为文件级:状态线程要读它们 */
static u32  loop_count;
static bool uart_present;

/* 全局定时器 333333343Hz -> 拍数换算成微秒(状态线程要用,所以放在这里) */
#define GT_TICKS_TO_US(t) ((u32)(((u64)(t) * 1000000ull) / (u64)PLAT_GLOBAL_TIMER_FREQ_HZ))


/*
 * 原来这段在 kmain 的主循环里(`gt >> 29` 那个 1.6 秒的节拍)。
 * 现在它是一个**内核线程**,形状照源 OS 的用法:
 *
 *     do { scheduler_sleep_ns(1ms); …干活…; } while (!条件);
 *     —— `ipc.cpp:37,46`、`sys.cpp:618` 全是这个形状。
 *
 * ★ 它为什么是"第一个真实负载" ★
 *   前面所有探针线程都是"跑一段就挂起",没有一个会用 `sched_sleep_ns`。
 *   而这个线程会**睡下去、被扫描唤醒、拿到 CPU 再睡** —— 于是它同时压到:
 *     - `sched_sleep_ns` 本身;
 *     - 唤醒(必须发生在 `sched_select_next` 的**全表扫描**里,
 *       这是 M4-8 "取队首"那一版根本做不到的事);
 *     - 睡醒补偿 `sched_apply_wakeup_credit` 的**顺序**效果(不是比例);
 *     - 与一个**纯占用线程**共存时还能被及时服务。
 *
 * ★ 判据:唤醒延迟 ★
 *   `sched_sleep_ns(P)` 回来后立刻量"实际比 P 长了多少" —— 那就是
 *   **从到点到真正拿到 CPU** 的延迟。它是对"顺序"的直接测量:
 *   有个纯占用线程在旁边霸着 CPU 时,这个数仍然要小。
 *   (按 §4.5 拍板的分工:睡醒补偿**不判比例**(等权负载下任何策略都过),
 *    判的是**顺序** —— 而延迟就是顺序的可观测量。)
 */
#define STATUS_PERIOD_NS 1000000000ull /* 1 Hz */

static volatile u32 g_status_wakes;
static volatile u32 g_status_late_last_us;
static volatile u32 g_status_late_max_us;
static volatile u32 g_status_printed;
static u32          g_status_created; /* 状态线程造出来了吗(判据的适用条件)*/

/*
 * ★ M4-11.1:报告区间的两个起点 ★
 *
 * `g_report_off0`  :进排他窗口**之前**的"被人为关掉调度"的累计时长
 * `g_report_wakes0`:同一时刻状态线程醒过的次数
 *
 * 报告里那两条判据取的就是它们与本刻的差值 —— 于是"报告期间整个系统
 * 停摆了几毫秒""状态线程还有没有在跑"是直接量出来的,不是推出来的。
 */
static u64 g_report_off0;
static u32 g_report_wakes0;

static void status_thread(void *arg)
{
    (void)arg;

    for (;;) {
        u64 t0 = timer_read_ns();
        u32 late_us;
        u64 t1;

        sched_sleep_ns(STATUS_PERIOD_NS);

        t1      = timer_read_ns();
        late_us = (u32)((t1 - t0 - STATUS_PERIOD_NS) / 1000u);

        g_status_wakes++;
        g_status_late_last_us = late_us;
        if (late_us > g_status_late_max_us) {
            g_status_late_max_us = late_us;
        }

        /* 这一段就是原来主循环里那几行,一个字没改语义 */
        HB[HB_SLOT_TICKS]    = g_tick_seen;
        HB[HB_SLOT_IRQCOUNT] = irq_get_stats()->irq_count;
        HB[HB_SLOT_TICKGAP]  = GT_TICKS_TO_US(g_tick_max_gap);

        if (uart_present) {
            /*
             * ⚠ 状态行必须**整行排他**。
             *
             * M4-8.4 之后串口有两个写者(本线程 + kmain 的自检报告/shell),
             * 而 9600 波特下一行要 60~80ms —— 足够别的上下文插进来几十次。
             * 实测:状态行正好插进自检报告中间,把 `=== SELF-TEST END ===`
             * 劈成两半,于是**一条命令判定过不过**这件事被打断。
             */
            console_excl_begin();
            console_printf("[XJ380/arm32] alive loop=%u led=0x%02X ticks=%u irq=%u lag=%u ms maxgap=%u us "
                           "wake=%u late=%u us\n",
                           loop_count, (u32)(HB[HB_SLOT_LED] & 0xFFu), g_tick_seen,
                           irq_get_stats()->irq_count, (u32)(timer_read_us() / 1000u) - g_tick_seen,
                           GT_TICKS_TO_US(g_tick_max_gap), g_status_wakes, late_us);
            console_excl_end();
            g_status_printed++;
        }
    }
}

/* 对照组用的第二个状态线程(A/B 窗口里跑,窗口结束就挂起)*/
static void status_thread_ctl(void *arg)
{
    (void)arg;

    for (;;) {
        sched_sleep_ns(STATUS_PERIOD_NS);
        g_status_ctl_wakes++;
        g_status_ctl_printed++;
    }
}

/*
 * 纯占用负载:跑满一段墙钟,**绝不主动让出**。
 * 用它来回答"M4-8.4 的第二个判据":状态线程睡觉的时候,CPU 有没有真的
 * 被别的线程用起来(而不是它在忙等),以及它醒来之后能不能被及时服务。
 */
static void load_probe(void *arg)
{
    spin_probe((void *)&g_spin[0]);
    (void)arg;
}

/* ------------------------------------------------------------------ */
/* ★ M4-8.5:无饥饿判据 —— K 个绝不睡眠的线程 + 一个睡眠式监视器 ★      */
/* ------------------------------------------------------------------ */

/*
 * ## 判据(与宿主那一半**同一个 N**)
 *
 *     一个**可运行**的线程,永远不会被无限期地漏掉。
 *
 * 落成可测的形式:把时间切成 N tick 的窗口,**每个窗口里,每个"被监视且
 * 仍然可运行"的线程,它自己的计数器都必须至少涨过一次**。
 *
 * ⚠ 它**不是**公平性/比率判据:计划里明写"任何策略在等权 + 纯占用负载下
 *   都会通过",所以份额类判据**没有区分能力**,这一条有(见下面的对照组)。
 *
 * ## 四件定死的事(计划 §0.5.7)
 *
 *   1. **N = 64 tick** —— 由 `SCHED_TIME_SLICE = 4` 推:K 个等权纯占用线程
 *      每个约每 `4K` tick 轮到一次;K=4 ⇒ 16,取 **4 倍余量**。
 *      ⚠ 写成"刚好够"不会更严,只会**更脆**(负载抖一下就误报)。
 *   2. **只盯"从不睡、从不挂起"的线程** —— 只有它们"应当一直在推进"。
 *      实现上落成:kmain 造线程时**显式登记**给监视器,监视器只认名单;
 *      而且每次结算都要复查 `status` 与 `task_level`(见下)。
 *   3. **监视器自己用 `sched_sleep_ns` 睡**(正是 M4-8.4 建好的机制)。
 *      它睡眠时不参与竞争,于是不会挤占被观察者的 CPU。
 *   4. **"推进过"取线程自己的计数器**,不是"被选中过" ——
 *      后者漏掉"选中了但没干活就被换下"这个方向。
 *
 * ## 监视器必须能区分三种"没推进"
 *
 *   | 情况 | 处置 |
 *   |---|---|
 *   | 那个线程**本来就不该跑**(WAIT / 挂起 / idle)| 跳过,而且**重新起算** checkpoint |
 *   | 有人**故意关了调度**(`console_excl` 打自检报告要好几秒,整个系统停摆)| 这个窗口**作废**,单独计数 |
 *   | 其余 | ★ **饥饿**:计数 + 打一行报警 ★ |
 *
 * ⚠ 最后那一条正是它存在的理由。M4-8.4 那一步里连续踩了两个**同一类**的错
 *   ("A/B 清理里调 `sched_kern_init()` 清空队列" / "对还在队列里的节点写
 *   `sched_next = NULL`"),两者都是"**一个永久可运行的线程静默消失**",
 *   定位代价是一次 JTAG 会话。常驻监视器会把它们变成**一行输出**。
 */
#define STARVE_K            4u
#define STARVE_WINDOW_TICKS 64u /* ★ 与宿主同一个 N ★ */
#define STARVE_WINDOW_NS    (STARVE_WINDOW_TICKS * SCHED_TICK_NS)
/* 窗口长度超过 4N ⇒ 连**监视器自己**都没被调度到:那是饥饿的另一种样子 */
#define STARVE_LATE_TICKS   (STARVE_WINDOW_TICKS * 4u)
/* 关调度超过这个时长,就认为这个窗口被人为打断了(作废,不算饥饿)*/
#define STARVE_FREEZE_NS 1000000ull /* 1 ms */
/* 报警最多打几行 —— 对照组会故意触发,不能刷屏 */
#define STARVE_ALARM_MAX 3u

/* 相 5 的观测时长与负载时长。负载**必须**活得比观测期长,否则最后一个
 * 窗口里它会挂起,`watched` 就不足 K,判据自己先失真。 */
#define STARVE_PHASE_MS  1200u
#define STARVE_BUDGET_US (1600u * 1000u)

/* 判据非空转的下限:1200ms / 64ms ≈ 18 个窗口,减去被排他输出作废的那几个 */
#define STARVE_MIN_FULL 8u

/* 对照组的时长与粘住次数。粘住 n 次 ≈ 每个时间片粘一次 ≈ 4n tick */
#define STARVE_CTL_MS     2400u
#define STARVE_CTL_BUDGET (600u * 1000u)   /* 受害者:粘住期间就到期 */
#define STARVE_CTL_HOG    (2000u * 1000u)  /* 粘住的那一个:必须活过整段粘住 */
#define STARVE_STICKY_N   300u

/* 一个"绝不睡眠"的负载线程。★ 计数器是**它自己的** ★ */
typedef struct
{
    volatile u32 count;     /* ← 判据取的就是这个量 */
    volatile u32 done;
    volatile u32 exit_ms;   /* 它退出时"离本相开始多久"(毫秒)—— 相的长度要和它对账 */
    u32          budget_us; /* 跑多久墙钟(不是固定迭代数 —— 估小了会挂住系统)*/
} starve_probe_t;

static starve_probe_t g_starve[STARVE_K];

/*
 * ★ 这一相是从**哪一刻**开始算的墙钟 ★
 *
 * ⚠ 这不是小事,它是被对照组逼出来的:如果每个负载线程按"**我被选中的那一刻**"
 *   起算墙钟(M4-9 的探针就是这么写的),那么**被饿着的线程在饿完之后才开始
 *   计预算** —— 它会接着跑满自己那一份,于是"谁被饿过"在读数上**看不出来**。
 *
 *   本判据要的恰恰是反过来:被饿着的线程应当在饿完之后**立刻到期**,
 *   让"它一次都没推进过"这件事留在计数上。所以墙钟从**本相开始**算。
 */
static u64 g_starve_t0_us;

/* 监视器的一个被观察对象 */
typedef struct
{
    tcb_t               task;       /* NULL = 这一格空着 */
    volatile const u32 *counter;    /* ★ 线程自己的计数器 ★ */
    u32                 checkpoint; /* 上一个窗口结算时的值 */
} starve_watch_t;

static starve_watch_t g_starve_watch[STARVE_K];

/* 监视器发布的量(全部单调递增,判据一律取**前后差值** —— 慢串口的教训)*/
static volatile u32 g_starve_checks;  /* 结算过的窗口数(含"被观察者不足 K"的)*/
static volatile u32 g_starve_full;    /* 其中 K 个**都还处于可运行状态**的窗口数 */
static volatile u32 g_starve_events;  /* ★ "可运行却没推进"的 (线程,窗口) 对数 ★ */
static volatile u32 g_starve_late;    /* ★ 窗口长度超过 4N 的次数 ★ */
static volatile u32 g_starve_skipped; /* 因"量不准"而作废的窗口数(见下)*/
static volatile u32 g_starve_win_max; /* 见过的最长窗口(tick)*/
static volatile u32 g_starve_ticks;   /* 结算过的窗口长度之和(tick)—— 覆盖率审计 */
static volatile u32 g_starve_alarms;  /* 已经打过的报警行数 */

/* 相 5 的结论(供自检报告使用)*/
static u32 g_starve_ok;
static u32 g_starve_full_at;
static u32 g_starve_events_at;
static u32 g_starve_late_at;
static u32 g_starve_skip_at;
static u32 g_starve_win_max_at;
static u32 g_starve_ticks_at;
static u32 g_starve_ms_at;     /* 相的**墙钟**时长(毫秒),与上面那个 tick 数对账 */
static u32 g_starve_adv_at;    /* 相 5 里真的推进过的负载线程数(应当 == K)*/
static u32 g_starve_ctl_adv;   /* 对照组里推进过的个数(应当很小)*/
static u32 g_starve_ctl_detect;/* 对照组里监视器报没报警 */

static void starve_reset(u32 budget_us)
{
    u32 i;

    g_starve_t0_us = timer_read_us();

    for (i = 0u; i < STARVE_K; i++) {
        g_starve[i].count   = 0u;
        g_starve[i].done    = 0u;
        g_starve[i].exit_ms = 0u;
        g_starve[i].budget_us = budget_us;
    }
}

/*
 * 把监视器的**观察名单**设成这几个线程,并把统计量清零。
 *
 * ⚠ 必须与"造这些线程"在**同一个关调度的窗口**里:监视器是另一个上下文,
 *   名单改到一半被它读到的话,它会拿着旧 checkpoint 去比新线程 ——
 *   那种错是静默的。
 *
 * ⚠ 名单是**显式登记**的,监视器不猜。理由:能"一直在推进"的线程是造它的
 *   那个人才知道的性质(见上面第 2 条)。
 */
static void starve_arm(tcb_t *tasks, u32 n)
{
    u32 i;

    for (i = 0u; i < STARVE_K; i++) {
        if (i < n && tasks != NULL && tasks[i] != NULL) {
            g_starve_watch[i].task       = tasks[i];
            g_starve_watch[i].counter    = &g_starve[i].count;
            g_starve_watch[i].checkpoint = 0u;
        } else {
            g_starve_watch[i].task    = NULL;
            g_starve_watch[i].counter = NULL;
        }
    }

    g_starve_checks  = 0u;
    g_starve_full    = 0u;
    g_starve_events  = 0u;
    g_starve_late    = 0u;
    g_starve_skipped = 0u;
    g_starve_win_max = 0u;
    g_starve_ticks   = 0u;
    g_starve_alarms  = 0u;
}

/*
 * 纯占用负载:跑满一段**墙钟**就结束。
 *
 * ⚠ 与 M4-9 的探针同一个理由:用墙钟而不是固定迭代次数 ——
 *   迭代次数依赖编译结果与 CPU 频率,估小了线程就永远跑不完,
 *   于是启动流程再也回不来,那是"自检把系统挂住"的失败模式。
 *
 * ⚠ 起算点是 `g_starve_t0_us`(**本相开始的那一刻**),不是"我被选中的那一刻" ——
 *   理由见那个变量的说明:否则被饿着的线程会在饿完之后才开始计预算。
 *
 * 循环体只有一件事:推进**自己的**计数器。判据要的正是它。
 */
static void starve_probe(void *arg)
{
    starve_probe_t *p  = (starve_probe_t *)arg;
    u64             t0 = g_starve_t0_us;

    while ((timer_read_us() - t0) < (u64)p->budget_us) {
        p->count++;
    }

    p->exit_ms = (u32)((timer_read_us() - t0) / 1000u);
    p->done    = 1u;
    thread_finish();
}

static void starve_w0(void *arg)
{
    starve_probe((void *)&g_starve[0]);
    (void)arg;
}

static void starve_w1(void *arg)
{
    starve_probe((void *)&g_starve[1]);
    (void)arg;
}

static void starve_w2(void *arg)
{
    starve_probe((void *)&g_starve[2]);
    (void)arg;
}

static void starve_w3(void *arg)
{
    starve_probe((void *)&g_starve[3]);
    (void)arg;
}

/*
 * ★ 常驻饥饿监视器 ★
 *
 * 形状与源 OS 的周期任务一样(`ipc.cpp:37,46` / `sys.cpp:618`:
 * `do { scheduler_sleep_ns(1ms); …干活…; } while (…)`):
 * **睡一个窗口 → 结算 → 再睡**。
 *
 * ⚠ 正常路径上它**一个字都不打印**:9600 波特下一行要 60~80ms,一个每秒
 *   都说话的监视器会把串口占满,而且会污染自检报告区间
 *   (`verify_board.py` 对区间内的非 CHECK 行是**拒绝**的)。
 *   只有真的判定为饥饿时才打一行,而且限量。
 *
 * ⚠ 窗口长度用 **tick 计数**(`g_tick_seen`)量,不用墙钟:
 *   这条判据本身就是以 tick 定义的。
 */
static void starve_monitor(void *arg)
{
    u32 i;

    (void)arg;

    for (;;) {
        u32 win0 = g_tick_seen;
        u64 off0 = sched_off_total_ns();

        sched_sleep_ns(STARVE_WINDOW_NS);

        {
            u32 win    = g_tick_seen - win0;
            u64 frozen = sched_off_total_ns() - off0;
            u32 watched = 0u;
            u32 missed  = 0u;

            /*
             * ★★★ 判据用的是"扣掉人为停摆之后**还剩下多少可执行时间**" ★★★
             *
             * 排他输出(`console_excl_begin/end`)是**关调度**实现的:自检报告那
             * 几秒钟里整个系统停摆,所有线程都醒不过来 —— 那不是饥饿。
             *
             * ⚠ 但**不能因此把整个窗口一扔了事**(第一版就是这么写的,上板立刻
             *   打回来):**饥饿本身也会让窗口变得很长**,而且两者会叠加在
             *   同一个窗口里 —— 扔掉就等于**漏报**,而那正是这条判据唯一
             *   存在的理由。上板实测:A/B 里监视器被粘住 1.2 秒的那个窗口
             *   因为"里面有 100ms 是关调度的"被整段作废,于是
             *   `events=0 late=0` —— 报"未检出",而线程确实被饿死了。
             *
             * 扣掉之后:
             *   自检报告窗口:窗口 5s、停摆 5s   -> 可执行 ≈ 0    -> 不算数(单列计数)
             *   粘住对照组  :窗口 1.2s、停摆 0.1s -> 可执行 ≈ 1.1s -> ★ 报警 ★
             *
             * `usable` 以 tick 计,而 tick 与毫秒在这块板上是 1:1(私有定时器
             * 1kHz,已用状态行的 `ticks`/`lag` 对过账)。
             */
            u32  frozen_ms = (u32)(frozen / 1000000ull);
            u32  usable    = (win > frozen_ms) ? (win - frozen_ms) : 0u;
            bool judge     = (usable >= STARVE_WINDOW_TICKS);

            g_starve_checks++;
            g_starve_ticks += win;
            if (!judge) {
                /*
                 * 这个窗口里"可执行时间"不足一个完整窗口 —— 量不出东西来。
                 * **单独计数**:"一个窗口都没作废"与"一半窗口都作废了"
                 * 是两种完全不同的可信度,读数的人有权知道是哪种。
                 */
                g_starve_skipped++;
            }
            if (win > g_starve_win_max) {
                g_starve_win_max = win;
            }
            if (usable > STARVE_LATE_TICKS) {
                g_starve_late++;
            }

            for (i = 0u; i < STARVE_K; i++) {
                starve_watch_t *w = &g_starve_watch[i];
                tcb_t           t = w->task;

                if (t == NULL || w->counter == NULL) {
                    continue;
                }

                if ((t->task_level == TASK_IDLE_LEVEL) || !sched_status_runnable(t->status)) {
                    /*
                     * 它**本来就不该在这段时间里推进**(睡着的 / 挂起的 / idle)。
                     * 跳过,并且**重新起算** —— 否则"它在我们等它的时候睡着了"
                     * 会被记成"它被饿着",而那是两件完全不同的事。
                     */
                    w->checkpoint = *w->counter;
                    continue;
                }

                watched++;
                /*
                 * ⚠ 只在 `judge` 成立时才判"没推进":窗口里可执行时间不足
                 *   一个完整窗口时,"没推进"是**停摆**的直接后果,不是饥饿。
                 *   checkpoint 照样更新 —— 否则下一个窗口会拿一个更早的
                 *   基准去比,把停摆那一段算到它头上。
                 */
                if (judge && (*w->counter == w->checkpoint)) {
                    missed++; /* ★ 整整一个窗口:可运行,却一次都没推进 ★ */
                }
                w->checkpoint = *w->counter;
            }

            if (watched == STARVE_K) {
                g_starve_full++;
            }
            if (missed != 0u) {
                g_starve_events += missed;
            }

            if (((missed != 0u) || (usable > STARVE_LATE_TICKS)) && (g_starve_alarms < STARVE_ALARM_MAX) &&
                uart_present) {
                g_starve_alarms++;
                /* 整行排他:串口有两个写者,而一行要 60~80ms */
                console_excl_begin();
                console_printf("[XJ380/arm32] STARVATION: win=%u tick frozen=%u ms usable=%u "
                               "missed=%u/%u\n",
                               win, frozen_ms, usable, missed, watched);
                console_excl_end();
            }
        }
    }
}

/*
 * ★ M4-10:两核调度相用的探针 ★
 *
 * 它比前面那些探针多两件事,因为这一相要证的事不在"线程内部":
 *
 *   1. **它自己观测到自己在哪个核上跑** —— 这是"两个核都真的在跑线程"
 *      唯一不依赖我推理的证据(`percpu_self()->cpu_id` 只有本核能填对,
 *      与 AM3 用 MPIDR 而不是 `cpu_id` 判"CPU1 真的起来了"是同一条规矩)。
 *
 *   2. ★ **它顺手核对 d0-d31 还是不是自己的图案** ★
 *      这一条补的是 M4-10 的一个**证据缺口**:10.6 把 VFP 的那个全局量
 *      (`g_vfp_save_f`)换成了每核字段,理由是"两核同时调度时它会互相覆盖",
 *      但 FP 探针(相 3 与 9.76)全都被钉在 CPU0 ——
 *      **CPU1 上的浮点现场从没被任何判据检查过**。
 *      而这一批探针按"最短队列"规则正好落在 CPU1 ⇒ 它们跑起来就把
 *      "CPU1 的存/取对不对"这件事验了。
 *
 * ⚠ "图案只填一次"(与 M4-9.5 的 fp_probe 同一条规矩):每轮重填的话,
 *   线程被切回来时会先把自己的图案写回去,"寄存器被对方改过"就被它自己
 *   抹掉了 —— 判据永远通过。
 *
 * 墙钟预算很短(40ms),跑完就挂起 —— 两个核上的线程都必须会退出,
 * 否则 kmain 拿不回 CPU,"自检把系统挂住"。
 */
#define SMP_SCHED_BUDGET_US 40000u
#define SMP_SCHED_THREADS   3u /* 落到 CPU1 的那一批(按"最短队列"规则)*/
#define SMP_APP_THREADS     1u /* ★ 应用级:必须落到 CPU0(哪怕 CPU1 更空)*/
#define SMP_CTL_THREADS     2u /* 选核对照组:全塞 CPU0 */
/* 对照组只要 2 个:一个占住 CPU0、一个备用。线程池只有 32 个槽(D14:没有退出路径)*/
#define SMP_PROBE_SLOTS (SMP_SCHED_THREADS + SMP_APP_THREADS + SMP_CTL_THREADS)

typedef struct
{
    u32          pat[FP_TAG_WORDS];  /* 我的浮点图案(只填一次)*/
    u32          snap[FP_TAG_WORDS]; /* 读回来的快照 */
    u32          rmode;              /* 我的舍入模式 */
    volatile u32 fp_bad;             /* 图案/舍入模式被破坏的次数(正常 0)*/
    volatile u32 fp_checks;          /* 真核对过多少次(非空转的判据)*/
    volatile u32 cpu_seen;           /* ★ 它**自己**观测到的核号(0xFFFFFFFF = 没跑)*/
    volatile u32 done;
} smp_probe_t;

static smp_probe_t g_smp_probe[SMP_PROBE_SLOTS];

static void smp_probe_reset(void)
{
    u32 i;
    u32 j;

    for (i = 0u; i < SMP_PROBE_SLOTS; i++) {
        for (j = 0u; j < FP_TAG_WORDS; j++) {
            /* 每个槽一个一眼可分的图案 —— 否则"读到别人的"看不出来 */
            g_smp_probe[i].pat[j]  = 0x10000000u * (i + 1u) + j;
            g_smp_probe[i].snap[j] = 0u;
        }
        g_smp_probe[i].rmode     = ((i & 1u) != 0u) ? 3u : 0u; /* 0 = 就近,3 = 向零 */
        g_smp_probe[i].fp_bad    = 0u;
        g_smp_probe[i].fp_checks = 0u;
        g_smp_probe[i].cpu_seen  = 0xFFFFFFFFu;
        g_smp_probe[i].done      = 0u;
    }
}

static void smp_sched_probe(void *arg)
{
    smp_probe_t *p  = (smp_probe_t *)arg;
    percpu_t    *me = percpu_self();
    u64          t0 = timer_read_us();
    u32          fpscr_dummy;
    u32          j;

    /* d0-d31 <- 我的图案;FPSCR <- 我的舍入模式。**只填一次** */
    arch_vfp_restore(p->pat, 0u);
    arch_vfp_set_rmode(p->rmode);

    while ((timer_read_us() - t0) < (u64)SMP_SCHED_BUDGET_US) {
        arch_vfp_save(p->snap, &fpscr_dummy);
        p->fp_checks++;

        for (j = 0u; j < FP_TAG_WORDS; j++) {
            if (p->snap[j] != p->pat[j]) {
                p->fp_bad++; /* ← 我的浮点现场被换掉了 */
                break;
            }
        }
        if (arch_vfp_get_rmode() != p->rmode) {
            p->fp_bad++;
        }
    }

    /* ★ 核号由**本核**自己写 ★ */
    p->cpu_seen = (me == NULL) ? 0xFFFFFFFFu : me->cpu_id;
    if (p->cpu_seen < PERCPU_MAX_CPUS) {
        g_smp_ran[p->cpu_seen]++;
    }

    p->done = 1u;
    thread_finish();
}

/* 相 5 与对照组的四个负载线程入口(同一个探针,两个 g_starve 槽)*/
static u32 starve_created(const tcb_t *out)
{
    u32 i;

    for (i = 0u; i < STARVE_K; i++) {
        if (out[i] == NULL) {
            return 0u;
        }
    }

    return 1u;
}

static u32 starve_advanced(void)
{
    u32 i;
    u32 n = 0u;

    for (i = 0u; i < STARVE_K; i++) {
        if (g_starve[i].count != 0u) {
            n++;
        }
    }

    return n;
}

/*
 * ★ 等一段**墙钟** —— 不是"我自己的 N 毫秒" ★
 *
 * ⚠ 这两个不是一回事,而 `timer_delay_ms(N)` 是**后者**:它的实现是
 *   `while (ms--) timer_delay_us(1000)`,即 N 次**串行**的 1 毫秒等待。
 *   当前线程被抢占多久,它就多花多久。
 *
 *   上板实测(坑 39):
 *     相 5 请求 1200ms、实际 **2880ms** —— 差额正好是被 4 个负载线程占住
 *     的那 1600ms;相 4 请求 3000ms、实际约 5.4s,状态线程于是"醒了 5 次"
 *     而不是 3 次(那个数从 M4-8.4 起就一直是 5,当时没人追问)。
 *
 *   **内核没有问题,是用错了函数。** 但它让"相的长度"变成一个猜不出来的
 *   数,而判据的时序(负载活多久、粘住多久)全押在它上面。
 *
 * ★ 所以本文件里**所有"等某个负载跑完"的等待都用它,不用 `timer_delay_ms`** ★
 *   (相 0/2/3/4、以及三组对照组的窗口)。换来的是两样东西:
 *     1. 相的长度可预测 —— `phase=XXXX ms` 现在是设计值,不是"看运气";
 *     2. 启动更快 —— 相 4 从 5.4 s 变回 3.0 s,整机少等好几秒。
 *
 * ⚠ 这里**故意不让出**:相的长度必须由墙钟决定,而不是由"谁愿意让我跑"
 *   决定。被抢占时它会在恢复后立刻检查截止时刻并返回。
 */
static void wait_ms_wall(u32 ms)
{
    u64 t0 = timer_read_us();
    u64 us = (u64)ms * 1000ull;

    while ((timer_read_us() - t0) < us) {
        /* 忙等 */
    }
}

/* M4-7:异常帧是否落在 SVC 栈区(1 = 在,0 = 不在)*/
static u32 g_exc_frame_on_svc_stack;

/* M4-7 后半段:协作式上下文切换的自检状态 */
#define SWITCH_REG_PATTERN 0x5EED0000u

static arm_task_ctx_t g_ctx_a;
static arm_task_ctx_t g_ctx_b;

/*
 * ★ 离开 A 时"存到哪里"用一个**丢弃**的槽 ★
 *
 * `arch_ctx_switch(from, to)` 会**把当前状态存进 from**。如果 from 就是
 * `g_ctx_a`,那它会把 `arch_ctx_save(&g_ctx_a)` 刚设好的"切回来的继续点"
 * 覆盖成"arch_ctx_switch 调用点的下一条" —— 于是切回来之后会跳过
 * `arch_ctx_save` 的返回值使用处,整个用例的判据就落空了。
 * (第一版就是这么写的:ctx_b=PASS、stack_isolated=PASS,唯独 ctx_a=FAIL,
 *  而 FAIL 的原因是用例自己写错,不是切换器。)
 */
static arm_task_ctx_t g_ctx_throwaway;

static u32 g_sw_a_ret;
static u32 g_sw_a_sp;
static u32 g_sw_b_sp;
static u32 g_sw_b_ran;
static u32 g_sw_b_ran_nosp;
static u32 g_sw_a_local;
static u32 g_sw_b_local;
static u32 g_sw_b_local_val;
static u32 g_sw_b_regs_ok;
static u32 g_sw_ctx_a_ok;
static u32 g_sw_ctx_b_ok;
static u32 g_sw_stack_ok;
static u32 g_sw_nosp_detected;
/*
 * 对照组会在跑的过程中踩坏 kmain 自己的栈(那正是它要证明的事),
 * 所以它的输入(期望的栈区间)必须在**跑之前**抄到全局量里 ——
 * 否则打印出来的是一个被自己踩坏的局部变量,看着像 bug 其实是"预期的破坏"。
 */
static u32 g_sw_ab_base;
static u32 g_sw_ab_top;

/*
 * 上下文 B 的入口(M4-7 后半段)。
 *
 * 它做三件事,每件都对应一条会被检查的性质:
 *   1. 核对 r4-r11 还是切换器恢复进来的那些图案 —— 证明切换器真的恢复了
 *      被调用者保存寄存器(AAPCS 的硬要求);
 *   2. 在自己的栈上放一个局部变量,并记下它的地址与 sp —— 证明它跑在
 *      **自己的栈**上,而不是别人的;
 *   3. 主动切回 A —— 证明"能切回来",也就是切换是双向的。
 *
 * ⚠ 这个函数**不再返回**:它最后会切回 A,而 A 不会再切过来。
 *   为了不让编译器把它优化成 fallthrough,末尾放一个死循环。
 */
static u32 switch_probe_read_reg(u32 n);

static void switch_probe_b(void)
{
    volatile u32 local_b = 0xB0B0B0B0u;
    u32          i;
    u32          ok = 1u;

    for (i = 4u; i <= 11u; i++) {
        /* 用函数调用把寄存器压力做出来:切换器恢复错了这里就会读到别的值 */
        if (switch_probe_read_reg(i) != (SWITCH_REG_PATTERN + i)) {
            ok = 0u;
        }
    }
    g_sw_b_regs_ok = ok;

    g_sw_b_local     = (u32)(uintptr_t)&local_b;
    g_sw_b_local_val = local_b;
    g_sw_b_sp        = arch_read_sp();
    g_sw_b_ran++;

    arch_ctx_switch(&g_ctx_b, &g_ctx_a);

    /* 不该回到这里 —— 真回来了说明 A 又被切过来了,那是用例设计之外的情况 */
    g_sw_b_ran++;
    for (;;) {
    }
}

/*
 * 取第 n 个被调用者保存寄存器(r4-r11)的当前值。
 *
 * n 是运行期参数,所以**必须**用一段小汇编按 n 选寄存器 ——
 * 这正是要验的东西:切换器把 r4-r11 恢复成了什么。
 */
static u32 switch_probe_read_reg(u32 n)
{
    u32 v = 0u;

    switch (n) {
    case 4u:  __asm__ volatile("mov %0, r4"  : "=r"(v)); break;
    case 5u:  __asm__ volatile("mov %0, r5"  : "=r"(v)); break;
    case 6u:  __asm__ volatile("mov %0, r6"  : "=r"(v)); break;
    case 7u:  __asm__ volatile("mov %0, r7"  : "=r"(v)); break;
    case 8u:  __asm__ volatile("mov %0, r8"  : "=r"(v)); break;
    case 9u:  __asm__ volatile("mov %0, r9"  : "=r"(v)); break;
    case 10u: __asm__ volatile("mov %0, r10" : "=r"(v)); break;
    case 11u: __asm__ volatile("mov %0, r11" : "=r"(v)); break;
    default:  v = 0u; break;
    }

    return v;
}

static u32 g_kstack_selftest;
static u32 g_kstack_slots_ok;
static u32 g_kstack_guard_ok;
static u32 g_kstack_usable_ok;
static u32 g_kstack_adjacent_ok;
static u32 g_kstack_reuse_ok;

/* 物理页分配器的自检与冒烟结果,供自检报告使用 */
static u32 g_palloc_selftest;
static u32 g_palloc_smoke;
static u32 g_heap_selftest;
static u32 g_heap_smoke;

/* SMP 压力测试的结果,供自检报告使用 */
static smp_stress_result_t g_smp_stress;

/*
 * 心跳槽位定义在 arch/heartbeat.h —— 它是跨模块契约:
 * 写这个区的除了本文件,还有那些"中途可能回不来、必须自己留标记"
 * 的模块(如 MMU 启动)。槽位定义只能有一处。
 */

/* ------------------------------------------------------------------ */
/* 周期 tick 处理函数                                                  */
/* ------------------------------------------------------------------ */

/*
 * 中断延迟测量。
 *
 * 为什么需要:私有定时器是**电平触发**的。如果 CPU 有一段时间没去响应,
 * 定时器连expire 多次也只会被合并成一次中断 —— 计数上表现为"丢了 N 个 tick",
 * 但寄存器、GIC 状态全都正常,光看现场分不出"丢中断"和"时钟变慢"。
 *
 * 直接记录相邻两次中断之间全局定时器走过的拍数,就能把这件事量化:
 * 正常是 1ms 对应的 333333 拍,一旦出现远大于它的值,
 * 说明确实有一段中断没有被及时响应,而且能直接读出停了多久。
 */
static volatile u32 g_tick_last_gt;
static volatile u32 g_tick_first;

/* 全局定时器 333333343Hz -> 拍数换算成微秒 */

/* 全局定时器 333333343Hz -> 拍数换算成微秒 */

static void tick_handler(u32 intid, void *arg)
{

    u32       now;
    percpu_t *pc;

    (void)intid;
    (void)arg;

    /*
     * 必须先清定时器的中断标志再返回。
     * 私有定时器是电平式输出,不清标志的话 GIC 会立刻再报一次,
     * 表现为"进了中断就再也出不来",而且从寄存器上看一切正常。
     *
     * 这一步同时也把 tick 记到**本核**的 percpu 上(a9_timer_clear_irq 内部做)。
     */
    a9_timer_clear_irq();

    /*
     * ⚠⚠ 下面这些全局量(g_tick_seen / g_tick_last_gt / g_tick_max_gap)
     *    是 **CPU0 的诊断状态**,而这个中断处理函数是两核共用的 ——
     *    CPU1 的 tick 也会走到这里。不隔离的话 CPU1 会把 CPU0 的计数
     *    覆盖掉,而症状是"ticks 和 irq_count 对不上"这种看起来像
     *    丢中断的现象(实测踩到,自检里的 irq_ticks_eq_irq 直接报 FAIL)。
     *
     *    本核自己的 tick 计数已经在 percpu 里,不受影响。
     */
    pc = percpu_self();
    if (pc != NULL && pc->cpu_id != 0u) {
        return;
    }

    now = timer_read_ticks_low();

    /*
     * ⚠ 第一次中断必须单独处理,不能拿它去更新 max_gap。
     * g_tick_last_gt 初值是 0,而此刻全局定时器早已跑了上千万拍,
     * 于是第一次算出来的"间隔"其实是"从定时器归零到现在"的绝对值 ——
     * 实测会读出 1013343us 这种数字,看上去像一次长达 1 秒的中断丢失,
     * 实际什么都没发生。这个伪影第一次就把我误导了一轮。
     */
    if (g_tick_first == 0u) {
        g_tick_first = 1u;
        g_tick_last_gt = now;
        g_tick_seen++;
        return;
    }

    {
        u32 gap = now - g_tick_last_gt;

        g_tick_last_gt = now;

        if (gap > g_tick_max_gap) {
            g_tick_max_gap = gap;
        }
    }

    g_tick_seen++;
}

static void fail_stop(void)
{
    /*
     * 不可恢复的早期失败:停在这里并闪烁 PS LED,
     * 让板上能看出"停住了"而不是"没跑"。
     */
    for (;;) {
        led_ps_set(true);
        timer_delay_ms(200);
        led_ps_set(false);
        timer_delay_ms(200);
    }
}

void kmain(void)
{
    u32      uart_clk = 0;
    u32      clock_source = 0;
    u32      last_step  = 0xFFFFFFFFu;
    u32      last_ps    = 0xFFFFFFFFu;
    u32      before_us  = 0; /* 使能缓存前的基准耗时 */
    bool     uart_loopback_ok = false;

    /* static: 由 BSS 自动清零。这样在"无串口"路径下打印诊断也不会读未初始化值 */
    static uart_baud_result_t baud_result;

    /* ---- 1. 先把板级反馈弄起来 ---- */
    /*
     * 串口依赖未知的参考时钟,而 LED 只依赖已经验证过的 GPIO 通路。
     * 所以先做 LED:即使后面串口标定失败,板上也一定有可见反馈。
     *
     * ⚠ M3 之后 led_init() **只初始化 PS 侧的 MIO7/MIO8**。
     *   PL 侧的 AXI GPIO 归设备描述层管 —— 它的方向寄存器要按描述
     *   给出的位宽来配,而那要等 board_probe_all() 跑完。
     *   这个拆分让 led_init() 能保持"足够早",不依赖任何发现机制。
     */
    led_init();
    HB[HB_SLOT_MAGIC] = PLAT_HEARTBEAT_MAGIC;

    /* ---- 2. 时间基准 ---- */
    /*
     * ⚠ 必须在任何 timer_delay_* 之前初始化。
     * 自检延时和 fail_stop() 都依赖全局定时器在跑;
     * 若顺序反了,计数器不递增,延时函数会永远等下去。
     */
    timer_init();

    /* PS LED 自检。PL LED 的自检在描述层跑完之后(见第 4 步) */
    led_ps_set(true);
    timer_delay_ms(150);
    led_ps_set(false);

    /* ---- 3. 控制台 ---- */
    /*
     * 先探测 UART 是否真的在响应。
     *
     * 本板实测:现有 AXI_GPIO_1 设计的 ps7_init 没有打开 UART 的 APER 外设
     * 时钟门控(APER_CLK_CTRL bit12/bit13 = 0),导致 UART 寄存器读回恒为 0。
     * 此时若直接轮询 TX FIFO,内核会在标定阶段死循环。
     * 所以"有没有串口"必须是可探测的,而不是靠轮询去撞。
     */
    uart_present = uart_probe(PLAT_CONSOLE_UART_BASE);

    if (uart_present) {
        /*
         * 闭环收敛参考时钟。
         *
         * 不用单点外推(见 uart_converge_ref_clk 注释):本板上那条外推
         * 偏低 7.033 倍,真值约 100.5MHz。闭环不依赖模型,直接在工作点
         * 反复"设定->实测->修正"直到实际波特率与目标相符。
         */
        uart_clk = uart_converge_ref_clk(PLAT_CONSOLE_UART_BASE, PLAT_CONSOLE_BAUD);

        if (uart_clk != 0) {
            clock_source = 1; /* 闭环收敛成功 */
        } else {
            uart_clk     = 100500000u; /* 本板实测值,收敛失败时的兜底 */
            clock_source = 2;
        }

        /*
         * 先用 uart_init 拿到波特率协商结果用于打印诊断,
         * 再让 console 绑定到同一个 UART(console_init 内部会再初始化一次,
         * 参数相同,是幂等的)。
         */
        uart_init(PLAT_CONSOLE_UART_BASE, uart_clk, PLAT_CONSOLE_BAUD, &baud_result);
        console_init(PLAT_CONSOLE_UART_BASE, uart_clk, PLAT_CONSOLE_BAUD);

        /*
         * ---- 收发通路自检(必须在这里做,不能挪到自检报告那一节)----
         *
         * 用它把"驱动/寄存器有问题"与"外部线缆有问题"分开:
         * 内部环回把发送端在芯片内直接接到接收端,不经过外部引脚。
         *
         * ⚠ 位置是有讲究的,放在这里有两个硬理由:
         *
         * (a) 它会临时把 MR 切成环回再切回来。放在报告打印中途做,
         *     会把已经排队但还没发出去的输出冲掉(实测:整条
         *     `CHECK uart_baud_ppm` 消失、上一行只剩半行)。
         * (b) 本地环回模式下 TX 引脚**仍然在输出**,探针字节会漏到线上。
         *     放在横幅之前,漏出来的那个字符落在报告区间之外,
         *     不会把 `CHECK` 行首污染成 `UCHECK` 而让解析脚本漏读一项。
         *
         * 结果缓存下来给后面的自检报告用。
         */
        uart_loopback_ok = uart_loopback_selftest(PLAT_CONSOLE_UART_BASE);
    }

    HB[HB_SLOT_UARTCLK] = uart_clk;
    HB[HB_SLOT_UARTOK]  = uart_present ? 1u : 0u;

    /*
     * 把"怎么得到这个 uart_clk 的"一并记录。
     * uart_clk 本身不足以判断可用性:收敛成功和兜底猜值可能都是同一个数字,
     * 而只有前者能保证线路上真的是 9600。
     */
    HB[HB_SLOT_CLKSRC]   = clock_source;
    HB[HB_SLOT_CONVITER] = uart_converge_last_iters();
    HB[HB_SLOT_BAUDGEN]  = baud_result.baudgen;
    HB[HB_SLOT_BAUDDIV]  = baud_result.bauddiv;

    /*
     * ---- 4. 启动横幅 ----
     * 到这里 uart_clk 已经过闭环验证:实际波特率与 PLAT_CONSOLE_BAUD 相符,
     * 所以下面这些文字应当是终端上可以直接读到的。
     */
    console_puts("\n");
    console_puts("================================================\n");
    console_puts(" OpenXJ380 / ARMv7-A (Zynq-7020)\n");
    console_puts(" M3 - device description layer + driver probe\n");
    console_puts("================================================\n");

    console_printf(" CPU          : %u Hz\n", PLAT_CPU_FREQ_HZ);
    console_printf(" Global timer : %u Hz\n", PLAT_GLOBAL_TIMER_FREQ_HZ);
    console_printf(" UART ref clk : %u Hz (source=%u iters=%u, %s)\n", uart_clk, clock_source,
                   uart_converge_last_iters(),
                   clock_source == 1 ? "self-calibrated" : "fallback");
    console_printf(" Baud         : requested=%u actual=%u (BAUDGEN=%u BAUDDIV=%u err=%u ppm)\n",
                   baud_result.requested, baud_result.actual, baud_result.baudgen, baud_result.bauddiv,
                   baud_result.error_ppm);
    console_printf(" DDR base     : 0x%08X size 0x%08X\n", PLAT_DDR_BASE, PLAT_DDR_SIZE);
    console_printf(" Kernel       : 0x%08X\n", PLAT_KERNEL_LOAD);
    console_printf(" PS GPIO DIRM0: 0x%08X OEN0: 0x%08X\n", led_get_dirm0(), led_get_oen0());
    console_puts("------------------------------------------------\n");
    console_printf(" Uptime at banner end: %u us\n", (u32)timer_read_us());
    console_puts("\n");

    if (baud_result.valid) {
        console_puts(" If you can read this, the serial console works.\n\n");
    } else {
        console_puts(" WARNING: baud error out of range, output may be garbled.\n\n");
    }

    HB[HB_SLOT_DIRM0] = led_get_dirm0();

    /* ---- 5. 设备描述层 ---- */
    /*
     * 位置:在控制台之后、中断与 MMU 之前。
     *
     *   - 必须在控制台之后:probe 的结果要能打出来。**没有枚举的 ARM 上,
     *     "打印全部节点"是唯一能在驱动没起来时区分"驱动写错了"和
     *     "描述表里根本没有这个节点"的手段**;
     *   - 必须在中断之前:驱动的 probe 目前都是纯 MMIO 轮询,
     *     不需要中断;放前面可以让"中断没配好"与"驱动没 probe 上"
     *     两类问题互不干扰;
     *   - 必须在 MMU 之前:probe 里访问的都是物理地址。
     *
     * 顺序本身也是有意义的:先 dump 再 probe,所以日志里能同时看到
     * "描述了哪些设备"和"哪些被认领了",而不是只看到结果。
     */
    console_puts(" Device description layer\n");
    board_dump_devices();
    HB[HB_SLOT_PROBED] = board_probe_all();
    console_puts("\n");

    /*
     * PL LED 自检。
     *
     * **挪到这里是有原因的**:PL 侧的 AXI GPIO 现在归描述层管,
     * 它的方向寄存器由驱动在 probe 时按描述里给出的位宽配置。
     * 在此之前往 ch2 写数据是无效的 —— 通道还是输入。
     * 这也正是 M3 想验证的事情之一:自检能跑,就说明
     * "描述表 -> 匹配 -> probe -> 驱动配置硬件"这条链路真的通了。
     */
    if (axi_gpio_ready()) {
        /*
         * 自检分两步,而且**第二步才是真正的验证**。
         *
         * 第一步(全亮再全灭)只是把肉眼可见的反馈做出来,能说明的信息
         * 很有限 —— 即使驱动把值写到了错误的地址,这一步也照样"跑完"了,
         * 只是板上什么都不亮。
         *
         * 第二步做写回读:把几个固定图案写进 LED 通道再读回来比对。
         * AXI GPIO 的输出通道 DATA 寄存器是可读的,所以这条链路
         * (驱动基址 -> 寄存器 -> 读回)能被直接观测。
         *
         * 这是唯一能区分"代码跑了"与"硬件真的动了"的手段。本项目在
         * SCU/ACTLR 上就因为少了这类观测而误判过一次:缓存使能位读回是 1,
         * 系统也照常跑,但实际上完全没有加速。
         */
        static const u8 patterns[] = {0x00u, 0xFFu, 0xA5u, 0x5Au, 0x01u, 0x80u};
        u32             i;
        u32             mismatches = 0;

        console_printf(" LED self-test: AXI GPIO at 0x%08X, %u-bit, dual-channel\n",
                       (u32)axi_gpio_get_base(), axi_gpio_get_width());

        led_pl_set(0xFFu);
        timer_delay_ms(150);
        led_pl_set(0x00u);

        for (i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++) {
            u8 readback;

            led_pl_set(patterns[i]);
            readback = axi_gpio_led_read();

            if (readback != patterns[i]) {
                mismatches++;
                console_printf("   write/readback mismatch: wrote 0x%02X, read 0x%02X\n", patterns[i],
                               readback);
            }
        }

        console_printf(" LED check    : write/readback %s (%u patterns)\n",
                       (mismatches == 0u) ? "PASS" : "FAIL",
                       (u32)(sizeof(patterns) / sizeof(patterns[0])));
        console_printf(" LED switches : ch1 = 0x%02X\n", axi_gpio_switch_read());

        HB[HB_SLOT_LEDCHECK] = mismatches;

        /* 留给主循环一个干净的起点 */
        led_pl_set(0x00u);
    } else {
        console_puts(" LED WARN     : AXI GPIO was not claimed - PL LEDs unavailable\n");
        HB[HB_SLOT_LEDCHECK] = 0xFFFFFFFFu;
    }
    console_puts("\n");

    /* ---- 6. 中断子系统(GIC + 周期 tick) ---- */
    /*
     * 顺序有讲究:
     *   1) gic_init() 在关中断状态下配置 Distributor/CPU Interface
     *   2) 注册处理函数(注册与使能分离,这是与 x86 侧最大的结构差异)
     *   3) 启动定时器并让 GIC 放行该 INTID
     *   4) 最后才打开 CPU 的中断响应
     * 反过来的话,中断可能在处理函数登记之前就打进来。
     */
    console_puts(" IRQ init: vectors installed, configuring GIC...\n");

    /*
     * ⚠ 这一段被刻意切成了四小段并各自打印一行。
     *
     * 为什么:M4-7 把异常入口改成"帧建在任务的栈上"之后,曾经在这段窗口里
     * **整个 PS 挂住**(连 JTAG 的 DAP 都读不到),而串口停在"configuring GIC..."
     * 那行后面 —— 光靠"最后一行输出"分不清是 gic_init 挂了、定时器启动挂了、
     * 还是第一条中断挂了。四行输出把这个窗口一次切开。
     */
    gic_init();
    console_puts(" IRQ init: gic_init() done\n");

    if (irq_register(GIC_INTID_A9_PRIVATE_TIMER, tick_handler, NULL) != 0) {
        console_puts(" WARN: 定时器 INTID 29 已被占用,周期 tick 未启用\n");
    }

    a9_timer_start_tick(1000u);                            /* 1 ms 一次 */
    gic_set_priority(GIC_INTID_A9_PRIVATE_TIMER, 0x80u);   /* 数值越小优先级越高 */
    gic_enable_irq(GIC_INTID_A9_PRIVATE_TIMER);
    console_puts(" IRQ init: timer started\n");

    /*
     * ★★★ 开中断之前:把"每核指针"显式清零 —— 它是一道**前提**,不能靠复位值 ★★★
     *
     * 每条 IRQ 都会走 `arch_vfp_save_current`(存浮点现场的那两条 `bl` 之一),
     * 它的第一条判据是:
     *
     *     TPIDRPRW == 0  ⇒  还没有每核结构,别碰浮点
     *
     * 而 **TPIDRPRW 是 CP15 的每核寄存器,复位值 UNKNOWN**:JTAG 的
     * `rst -system` / `rst -processor` 之后,它可能仍然留着**上一次运行**
     * 留下的地址。偏偏 `percpu_init_self()` 要等到**栈池之后**才跑(它要用
     * `__stack_top`),于是这中间的第一发 tick 会拿着一个过期地址去读
     * `[TPIDRPRW+56]`(也就是 `cur_vfp_d`)—— 只要那个值非 0(上一次运行
     * 留在 DDR 里的页表描述符、BSS 残影……),它就会把 d0-d15 **存进一个野地址**。
     *
     * 实测(坑 52,一次"看起来像变砖"的故障):
     *     DFAR = 0x82600C02   DFSR = 0x801(对齐故障,写)
     *     pc   = `arch_vfp_save_current` 里的 `vstmia r0!, {d0-d15}`
     *     sp   = 引导 SVC 栈,`svc_lr` 落在 `_vec_irq` 里
     * 串口停在 "IRQ init: timer started" 与 "interrupts enabled" **之间**,
     * 而且**每次重新加载都复现** —— 因为崩溃发生在写 TPIDRPRW 之前,
     * 那个过期值一直留着。换个地址布局不同的映像它就"好了",所以
     * 它的表现是"改一行代码就把板子改坏了",极难归因。
     *
     * ⇒ 所以这里**显式写 0**:哨兵的含义必须是"我们建立的",不是"我们假设的"。
     *   (之后 `percpu_init_self()` 会把它设成真的每核结构地址。)
     */
    arch_write_percpu(0u);

    irq_global_enable();
    console_puts(" IRQ init: interrupts enabled\n");

    /* 给中断一点时间跑起来,再报告结果 */
    timer_delay_ms(20);
    console_puts(" IRQ init: 20 ms elapsed (ticks should be > 0)\n");

    console_printf(" IRQ status  : ticks=%u irq_count=%u last_intid=%u spurious=%u\n",
                   g_tick_seen, irq_get_stats()->irq_count, irq_get_stats()->last_intid,
                   irq_get_stats()->spurious_count);

    if (g_tick_seen > 0) {
        console_puts(" Periodic tick is RUNNING (1 kHz, Cortex-A9 private timer).\n\n");
    } else {
        console_puts(" WARNING: no tick observed - GIC or timer not delivering.\n\n");
    }

    HB[HB_SLOT_TICKS] = g_tick_seen;

    /* ---- 7. MMU ---- */
    /*
     * 位置是有意选的:放在中断子系统验证**之后**。
     *
     * 这样串口上就有了明确的前后对照 ——
     * 横幅与 IRQ 状态行证明"开 MMU 之前一切正常",
     * 之后主循环的状态行证明"开 MMU 之后仍然正常"。
     * 若把 MMU 放在最前面,一旦出错就只能看到"什么都没输出",
     * 无法区分是页表错了还是别的地方本来就坏了。
     *
     * 真正的兜底是心跳槽 HB_SLOT_MMUSTAGE:
     * 打开地址转换后 C 代码未必还能跑,但那个槽是开 MMU 的代码
     * 自己在每一步之前写的,JTAG 一定能读到。
     */
    console_puts(" MMU: building page table (1MB sections, identity map)...\n");
    timer_delay_ms(20);

    mmu_enable();

    /*
     * 能执行到这里本身就说明了一件事:上面的 mmu_enable() 里
     * 打开 SCTLR.M 之后,**取指与访存都经过了页表**并且成功了。
     * 否则根本回不到这个函数。
     */
    console_printf(" MMU         : %s  stage=%u SCTLR=0x%08X TTBR0=0x%08X\n",
                   mmu_is_enabled() ? "ENABLED" : "disabled", HB[HB_SLOT_MMUSTAGE],
                   arch_read_sctlr(), arch_read_ttbr0());
    console_printf(" MMU regions : OCM/心跳=\"%s\"  DDR=\"%s\"  UART=\"%s\"  GIC=\"%s\"\n",
                   mmu_region_name_for(PLAT_HEARTBEAT_BASE), mmu_region_name_for(PLAT_KERNEL_LOAD),
                   mmu_region_name_for(PLAT_CONSOLE_UART_BASE), mmu_region_name_for(0xF8F01000u));

    if (!mmu_is_enabled()) {
        console_puts(" WARNING: MMU did not enable - see heartbeat slot 16\n");
    } else {
        /*
         * 自检:地址转换开着的情况下,读回一个已知常量。
         * 这是在验证"数据访问经页表后仍然取到正确的值",
         * 而不只是"没崩"。用区域名函数的返回值做样本 ——
         * 它来自 .rodata,落在 DDR 段里。
         */
        const char *probe = mmu_region_name_for(PLAT_KERNEL_LOAD);
        console_printf(" MMU selftest: .rodata readback \"%s\" (%s)\n", probe,
                       probe[0] == 'D' ? "OK" : "MISMATCH");
    }
    console_puts("\n");

    /* ---- 8. 缓存几何 ---- */
    /*
     * 这里只是**读取并打印**,不使能缓存 —— 使能是下一步(M2-5b)的事。
     *
     * 之所以要先单独做这一步:几何解码里三个字段全是"减一/减四"存储的
     * (见 arch/cache.h),少加一个 1 不会报错,只会让之后的整块失效
     * 少覆盖一行/一路,于是残留脏行在某个时刻被写回、静默覆盖正确数据。
     * 先把真实芯片的 CLIDR/CCSIDR 读出来对照,比等缓存开了之后
     * 再出问题去猜要容易得多。
     *
     * 已知答案:Cortex-A9 的 L1 是 32KB / 4 路 / 32 字节行。
     */
    {
        cache_geometry_t dgeo  = cache_discover(false, 0u);
        cache_geometry_t igeo  = cache_discover(true, 0u);
        u32              clidr = arch_read_clidr();
        bool             d_ok  = (dgeo.total_bytes == 32768u) && (dgeo.ways == 4u) && (dgeo.sets == 256u) &&
                                 (dgeo.line_bytes == 32u);
        bool             i_ok  = (igeo.total_bytes == 32768u) && (igeo.ways == 4u);

        console_printf(" Cache CLIDR : 0x%08X  (LoC=%u LoUIS=%u L1type=%u)\n", clidr, cache_loc(clidr),
                       cache_louis(clidr), cache_level_type(clidr, 0u));
        console_printf(" Cache L1 D  : %u B, %u-way, %u sets, %u B/line\n", dgeo.total_bytes, dgeo.ways,
                       dgeo.sets, dgeo.line_bytes);
        console_printf(" Cache L1 I  : %u B, %u-way, %u sets, %u B/line\n", igeo.total_bytes, igeo.ways,
                       igeo.sets, igeo.line_bytes);
        console_printf(" Cache check : %s (expect D and I both 32KB 4-way 32B/line 256 sets)\n",
                       (d_ok && i_ok) ? "MATCH" : "MISMATCH");

        /*
         * 使能前先跑一遍基准。
         *
         * 这一段本身在**无缓存**下执行(代码从 DDR 取指),所以耗时很长 ——
         * 这正是我们要拿来当对照的值。200 遍 × 4KB 读。
         */
        before_us = cache_benchmark_us(200u);
    }
    console_puts("\n");

    /* ---- 9. 使能 L1 缓存 ---- */
    /*
     * 顺序不能变:先做 coherency 前置条件(SCU + ACTLR),
     * 再使能缓存。
     *
     * ⚠ 漏掉 coherency_init 的症状极具迷惑性 —— 地址转换照常、系统照常跑、
     *   SCTLR 的 C/I 位读回来都是 1,唯独缓存完全不起作用。
     *   原因是 DDR 映射为 Shareable(S=1),而 SCU 未使能、ACTLR.SMP 未置位时
     *   Cortex-A9 不会把可共享访问放进 L1。本项目踩过一次,
     *   靠下面的耗时基准才发现 —— 光看寄存器是发现不了的。
     */
    console_puts(" Cache: enabling SCU + ACTLR (coherency preconditions)...\n");
    cortexa9_coherency_init();

    console_puts(" Cache: invalidating and enabling L1 D/I-cache...\n");
    timer_delay_ms(20);

    cache_enable_l1();

    /*
     * 能执行到这里就已经说明了一部分事情:cache_enable_l1() 里写 SCTLR
     * 之后,**取指与访存都经过了缓存**并且成功了。否则回不到这一行。
     */
    {
        u32 after_us = cache_benchmark_us(200u);

        console_printf(" Caches      : D=%s I=%s  SCTLR=0x%08X\n",
                       cache_dcache_enabled() ? "ON" : "off", cache_icache_enabled() ? "ON" : "off",
                       arch_read_sctlr());
        console_printf(" Coherency   : SCU=0x%08X ACTLR=0x%08X\n", cortexa9_scu_status(),
                       cortexa9_actlr_status());
        console_printf(" Cache bench : off=%u us  on=%u us  speedup=%ux\n", before_us, after_us,
                       (after_us > 0u) ? (before_us / after_us) : 0u);

        /*
         * 判断依据是**加速比**,不是寄存器位。
         *
         * 缓存使能后如果耗时几乎没变,说明缓存没有真正覆盖到这条路径
         * (最常见的原因是内存属性被写成了不可缓存)。这种情况下
         * SCTLR 的 C 位照样是 1,系统也照样跑 —— 只有这个比值能揭穿。
         */
        if (after_us > 0u && before_us > (after_us * 2u)) {
            console_puts(" Cache check : caches are demonstrably effective\n");
        } else {
            console_puts(" Cache WARN  : little speedup - caches may not be covering this memory\n");
        }

        HB[HB_SLOT_CACHEBENCH] = (after_us > 0u) ? (before_us / after_us) : 0u;
    }

    /*
     * L2(PL310)。
     *
     * 顺序与 Xilinx boot.S 一致:L1 先开,再初始化 L2。
     * 它不在 CLIDR 里,所以上面那套 CP15 几何循环完全覆盖不到它 ——
     * 必须走它自己的寄存器(0xF8F02000),配置也完全是另一套
     * (延迟参数、SLCR 里的 RAM 配置)。
     */
    {
        u32 l2_id = l2_cache_id();

        console_puts(" Cache: configuring PL310 L2...\n");
        timer_delay_ms(20);

        l2_cache_init();

        console_printf(" Cache L2    : %s  ID=0x%08X TYPE=0x%08X CTRL=0x%08X\n",
                       l2_cache_is_enabled() ? "ON" : "off", l2_id, l2_cache_type(), l2_cache_control());

        /*
         * L2 有效性实验 —— 受控 A/B。
         *
         * 为什么必须单独做这一步:上面那行只证明"使能位被置上了",
         * 而**不能证明 L2 真的在缓存任何东西**。本项目在 L1 上就吃过
         * 这个亏(SCU/ACTLR 没设,C 位读回是 1 但缓存完全无效),
         * 所以 L2 不能只靠寄存器交差。
         *
         * 工作集取 128KB:远超 32KB 的 L1,又远小于 512KB 的 L2。
         * 于是在 L2 真的工作时访问基本命中;L2 一关就退化成每次去 DDR,
         * 耗时差一个数量级,不可能看不出来。
         */
        {
            const volatile u32 *buf = cache_l2_bench_buf();
            u32                 words = cache_l2_bench_words();
            u32                 on_us;
            u32                 off_us;
            u32                 reon_us;

            /* 三遍:开 -> 关 -> 再开。中间那次是唯一的变量 */
            on_us = cache_bench_us_on(buf, words, 16u);

            l2_cache_disable();
            off_us = cache_bench_us_on(buf, words, 16u);
            l2_cache_enable();

            reon_us = cache_bench_us_on(buf, words, 16u);

            console_printf(" L2 bench    : 128KB working set  on=%u us  off=%u us  on-again=%u us\n",
                           on_us, off_us, reon_us);

            /*
             * 判据来自**实测**,不是估计。
             *
             * 本板实测:on=11211us  off=15987us  on-again=11214us
             * 也就是 L2 带来约 1.43 倍,而不是我最初想当然的"一个数量级" ——
             * 顺序访问会被 PL310 的预取掩盖掉相当一部分延迟,纯读循环
             * 又比真实负载更友好。第一版判据按 2 倍写,于是误报了 WARN。
             *
             * 真正能证明"L2 在缓存"的是这两条:
             *   1. 关掉之后明显更慢(不是噪声);
             *   2. 重新打开能精确回到原来的水平。
             * 第 2 条尤其关键 —— 它同时验证了 disable/enable 没有副作用
             * (直接清使能位会丢掉脏行,是最容易出错的地方),
             * 而加速比的**绝对大小**反而不是判据:它取决于工作集、
             * 访问模式与预取行为,拿来当阈值只会误导。
             */
            if (off_us > (on_us + (on_us / 5u)) && reon_us < (off_us - (off_us / 5u))) {
                console_printf(" L2 check    : effective (off/on = %u.%02ux)\n", off_us / (on_us ? on_us : 1u),
                               ((off_us * 100u) / (on_us ? on_us : 1u)) % 100u);
            } else {
                console_puts(" L2 WARN     : no measurable benefit - L2 may not be caching\n");
            }

            /*
             * 心跳槽里存**百分比**,不是比值。
             *
             * 实测比值是 1.42,取整后是 1 —— 而"1"这个数字写不出任何
             * 有意义的判据:自检里若写 ">= 1" 就永远成立,等于没判。
             * 存成 142 才能写出 ">= 120" 这种真正会失败的阈值。
             *
             * 这个不一致是被 tmp-test/verify_board.py 第一次跑就抓出来的:
             * 判据写 ">= 100" 而槽里存 1,直接报 FAIL。
             * 在此之前它不会以任何形式表现出来 —— 打印出来的
             * "effective (off/on = 1.42x)" 看着完全正常。
             */
            HB[HB_SLOT_L2BENCH] = (on_us > 0u) ? ((off_us * 100u) / on_us) : 0u;
        }
    }

    /*
     * 缓存维护自检。
     *
     * 这一步验证的是**DMA 路径所依赖的语义**,而不是缓存快不快:
     * clean 是否真的把数据写回了内存、invalidate 是否真的丢弃了缓存副本。
     * 两者任一不成立,DMA 就会出现"偶尔错几个字节"这类最难查的问题。
     *
     * 不需要任何外设参与 —— 靠的是"缓存与内存是两份副本"这个事实。
     * L2 使能后,这一套操作的语义要在**两级缓存**上都成立,
     * 而 cache_*_range() 正好同时维护 L1 与 L2,所以这个自检
     * 同时也是对两级联动的验证。
     */
    {
        u32 selftest = cache_selftest();

        console_printf(" Cache maint : line=%u B  clean/invalidate selftest=%s\n", cache_line_bytes(),
                       (selftest == 0u) ? "PASS" : "FAIL");
        if (selftest != 0u) {
            console_printf("               failed at check %u (see cache_hw.c)\n", selftest);
        }
        HB[HB_SLOT_CACHESELFTEST] = selftest;
    }
    console_puts("\n");

    /* ---- 9.4 物理页分配器(M4-2) ---- */
    /*
     * 池的范围 = [_kernel_end 向上对齐, DDR 末尾)。
     *
     * 为什么从 _kernel_end 开始而不是从 DDR 基址:内核镜像、页表、两个核的
     * 栈、L2 基准缓冲区全都在镜像之内(链接脚本定义的符号),所以镜像之后
     * 的才是真正没主的物理内存。**不需要**再逐个 palloc_reserve ——
     * 那些区域根本不在池范围内,reserve 它们只会是空操作。
     *
     * ⚠ 但这个"不需要"依赖一件事:**任何将来新增的静态占用都必须落在
     *   镜像之内**(即通过链接脚本而不是运行时分配)。将来若有人把某个
     *   大缓冲区放在镜像之外的固定物理地址上,必须在这里补一条 reserve。
     */
    {
        extern char         _kernel_end[];
        extern u32          g_palloc_bitmap[PALLOC_BITMAP_WORDS];
        extern palloc_t     g_palloc;

        uintptr_t   pool_base;
        size_t      pool_size;
        palloc_err_t e;

        pool_base = ((uintptr_t)_kernel_end + PALLOC_PAGE_SIZE - 1u) & ~(uintptr_t)(PALLOC_PAGE_SIZE - 1u);
        pool_size = (size_t)(PLAT_DDR_END - pool_base);
        pool_size &= ~(size_t)(PALLOC_PAGE_SIZE - 1u);

        e = palloc_init(&g_palloc, pool_base, pool_size, g_palloc_bitmap, PALLOC_BITMAP_WORDS);

        if (e == PALLOC_OK) {
            console_printf(" Page alloc  : %u pages (%u MB) at 0x%08X\n",
                           g_palloc.page_count,
                           (u32)(((uintptr_t)g_palloc.page_count * PALLOC_PAGE_SIZE) >> 20), (u32)pool_base);
        } else {
            console_printf(" Page alloc  : FAILED err=%u\n", (u32)e);
        }

        g_palloc_selftest = palloc_selftest();

        /*
         * 真实内存冒烟:分配一页、写一个图案、读回来、释放。
         *
         * 自检跑的是**合成实例**(基址 0x10000000,在宿主上也能跑),
         * 它证明分配器的逻辑对,但证明不了"这段物理内存真的能用"。
         * 这一步才是对真实 RAM 的读写 —— 与缓存那节"写回读"同一个道理。
         */
        g_palloc_smoke = 0u;
        if (e == PALLOC_OK) {
            uintptr_t page = 0;

            if (palloc_alloc(&g_palloc, &page) == PALLOC_OK) {
                volatile u32 *w = (volatile u32 *)page;
                u32           i;
                u32           ok = 1u;

                for (i = 0; i < (PALLOC_PAGE_SIZE / 4u); i++) {
                    w[i] = 0xA5A50000u ^ i;
                }
                for (i = 0; i < (PALLOC_PAGE_SIZE / 4u); i++) {
                    if (w[i] != (0xA5A50000u ^ i)) {
                        ok = 0u;
                        break;
                    }
                }

                if (palloc_free(&g_palloc, page) != PALLOC_OK) {
                    ok = 0u;
                }

                g_palloc_smoke = ok;
                console_printf(" Page smoke  : alloc/write/readback/free at 0x%08X = %s\n", (u32)page,
                               ok ? "PASS" : "FAIL");
            }
        }
    }

    /* ---- 9.44 细粒度映射(M4-4) ---- */
    g_vmap_selftest = vmap_selftest();

    {
        uintptr_t region = 0;

        /*
         * ⚠ 要 512 页(2MB)再**向上对齐到 1MB**,而不是直接要 256 页。
         *
         * 为什么:palloc 只保证 4KB 对齐,而"拆一个段覆盖整段"这个假设
         * 要求区间必须落在**单个 1MB 段**内。第一版直接要 256 页,
         * 拿到 0x00176000 —— 它跨了两个段(0x00100000 与 0x00200000),
         * 而只拆了第一个,于是后半段仍是段映射,逐页核对在第 138 页
         * 读到 VMAP_RESULT_SECTION 就失败了。
         *
         * (vmap_map 自己会按需拆段,所以映射本身不错;错的是
         *  "拆一次就覆盖整段"这个测试假设。)
         */
        if (palloc_alloc_pages(&g_palloc, 512u, &region) == PALLOC_OK) {
            region = (region + (1u << MMU_SECTION_SHIFT) - 1u) & ~((uintptr_t)(1u << MMU_SECTION_SHIFT) - 1u);
        }
        if (region != 0u) {
            vmap_attr_t  normal = vmap_attr_normal();
            vmap_err_t   e_init;
            vmap_err_t   e_split;
            u32          l1_before;
            u32          l1_after;
            u32          i;
            u32          split_ok = 1u;
            u32          live_ok  = 0u;
            u32          bad_page = 0xFFFFFFFFu;
            u32          bad_got  = 0u;

            l1_before = g_mmu_l1_table[vmap_l1_index((u32)region)];

            /*
             * ⚠ 这里保留**完整的错误码**,不再二值化。
             *   上一轮把它压成 split_ok 布尔值,结果是"失败了但不知道哪一步",
             *   白白多花一轮。验证代码的信息量本身就是产出的一部分。
             */
            /*
             * ⚠ l2_pool_pa 必须是**池自己的物理地址**(u32)g_l2_pool,
             *   不是 region —— region 是要被映射的那段数据。
             *
             *   上一轮这里填成了 (u32)region,于是 L1 描述符指向了数据区,
             *   硬件会把那块内存里的数据当成 L2 描述符读。症状是
             *   l1_after = 0x001761E1 的后半段恰好等于 region —— 一眼可辨,
             *   但当时把错误码二值化了,看不到这个数。
             */
            e_init  = vmap_init(&g_vmap, g_mmu_l1_table, g_l2_pool, (u32)(uintptr_t)g_l2_pool, 4u,
                                (u32)region, (u32)region + (1u << MMU_SECTION_SHIFT));
            e_split = (e_init == VMAP_OK) ? vmap_split_section(&g_vmap, (u32)region)
                                          : VMAP_ERR_NOT_INIT;

            l1_after = g_mmu_l1_table[vmap_l1_index((u32)region)];

            console_printf(" Vmap dbg    : region=0x%08X  l1_before=0x%08X l1_after=0x%08X\n",
                           (u32)region, l1_before, l1_after);
            console_printf(" Vmap dbg    : init_err=%u split_err=%u  l2_used=%u\n", (u32)e_init,
                           (u32)e_split, g_vmap.l2_used);

            if (e_init != VMAP_OK || e_split != VMAP_OK) {
                split_ok = 0u;
            } else {
                for (i = 0; i < VMAP_L2_ENTRIES; i++) {
                    u32 va  = (u32)region + (i << VMAP_PAGE_SHIFT);
                    u32 got = vmap_lookup(&g_vmap, va);

                    if (got == 0u || got == VMAP_RESULT_SECTION || (got & ~0xFFFu) != va) {
                        bad_page = i;
                        bad_got  = got;
                        split_ok = 0u;
                        break;
                    }
                }
            }

            if (split_ok == 0u && bad_page != 0xFFFFFFFFu) {
                console_printf(" Vmap dbg    : first bad page=%u got=0x%08X\n", bad_page, bad_got);
            }

            g_vmap_split_ok = split_ok;

            if (split_ok) {
                volatile u32 *page0 = (volatile u32 *)region;
                volatile u32 *page1 = (volatile u32 *)(region + VMAP_PAGE_SIZE);
                vmap_err_t    e_u;
                vmap_err_t    e_m;

                page0[0] = 0x11111111u;
                page1[0] = 0x22222222u;

                e_u = vmap_unmap(&g_vmap, (u32)region);
                e_m = vmap_map(&g_vmap, (u32)region, (u32)(region + VMAP_PAGE_SIZE), &normal);

                if (e_u == VMAP_OK && e_m == VMAP_OK) {
                    arch_tlb_invalidate_all();
                    arch_dsb();
                    arch_isb();

                    live_ok = (page0[0] == 0x22222222u) ? 1u : 0u;

                    (void)vmap_unmap(&g_vmap, (u32)region);
                    (void)vmap_split_section(&g_vmap, (u32)region);
                    (void)vmap_map(&g_vmap, (u32)region, (u32)region, &normal);
                    arch_tlb_invalidate_all();
                    arch_dsb();
                    arch_isb();
                }

                console_printf(" Vmap dbg    : unmap_err=%u map_err=%u readback=0x%08X\n", (u32)e_u,
                               (u32)e_m, page0[0]);
            }

            g_vmap_live_ok = live_ok;

            console_printf(" Vmap        : region=0x%08X split=%s live=%s\n", (u32)region,
                           g_vmap_split_ok ? "PASS" : "FAIL", g_vmap_live_ok ? "PASS" : "FAIL");
        } else {
            console_puts(" Vmap        : palloc FAILED\n");
        }
        }

    /* ---- 9.45 内核堆(M4-3) ---- */
    /*
     * 堆区**一次性拿一大块连续内存**,不依赖"用完再要一页接上"。
     *
     * 为什么:堆是一条贯穿整个区域的链表,增长区必须与堆区**紧邻**,
     * 而 palloc_alloc_pages() 返回的页不保证相邻 —— 那条路根本走不通。
     * 所以这里用 palloc_alloc_pages(HEAP_PAGES) 一次要足。
     *
     * 32MB 对 1GB 的板子不算什么,而且它换来的是"堆永不碎片化到无法增长"。
     */
    {
        uintptr_t heap_base = 0;
        palloc_err_t pe = palloc_alloc_pages(&g_palloc, HEAP_PAGES, &heap_base);

        if (pe == PALLOC_OK) {
            console_printf(" Kernel heap : %u KB at 0x%08X\n",
                           (u32)((HEAP_PAGES * PALLOC_PAGE_SIZE) >> 10), (u32)heap_base);

            if (heap_init(&g_heap, heap_base, (size_t)HEAP_PAGES * PALLOC_PAGE_SIZE) != HEAP_OK) {
                console_puts(" Heap        : init FAILED\n");
            }
        } else {
            console_printf(" Heap        : palloc FAILED err=%u\n", (u32)pe);
        }

        g_heap_selftest = heap_selftest();

        /*
         * 真实内存冒烟:自检跑的是合成实例,证明逻辑对;
         * 这一步才证明**这块物理内存真的能用**(与 palloc 的冒烟同理)。
         */
        g_heap_smoke = 0u;
        if (pe == PALLOC_OK) {
            u8  *p1 = (u8 *)heap_alloc(&g_heap, 1000u);
            u8  *p2 = (u8 *)heap_alloc(&g_heap, 4000u);
            u32  i;
            u32  ok = 1u;

            if (p1 == NULL || p2 == NULL || p1 == p2) {
                ok = 0u;
            } else {
                for (i = 0; i < 1000u; i++) { p1[i] = (u8)(i & 0xFFu); }
                for (i = 0; i < 4000u; i++) { p2[i] = (u8)((i * 7u) & 0xFFu); }
                for (i = 0; i < 1000u; i++) { if (p1[i] != (u8)(i & 0xFFu)) { ok = 0u; break; } }
                for (i = 0; i < 4000u; i++) { if (p2[i] != (u8)((i * 7u) & 0xFFu)) { ok = 0u; break; } }
                if (heap_check(&g_heap) != HEAP_OK) { ok = 0u; }
                if (heap_free(&g_heap, p1) != HEAP_OK) { ok = 0u; }
                if (heap_free(&g_heap, p2) != HEAP_OK) { ok = 0u; }
                if (heap_check(&g_heap) != HEAP_OK) { ok = 0u; }
            }

            g_heap_smoke = ok;
            console_printf(" Heap smoke  : alloc/write/readback/free 2 blocks = %s\n",
                           ok ? "PASS" : "FAIL");
        }
    }

    /* ---- 9.46 内核栈池 + guard page(M4-5) ---- */
    /*
     * 位置:在 palloc(M4-2)与 vmap(M4-4)之后。
     *
     * 这里做完的只是"栈池可用 + guard 页确实不可访问"这一级的证据
     * (读回页表)。**真正证明 guard 是承重的**要靠第三级证据 ——
     * 故意写溢出,见 fault_test 的两个新选择器与 tmp-test/guard_trip.py。
     * 分成两处是刻意的:溢出会让内核停在 Data Abort 现场,
     * 而"停在现场"这件事不该发生在每次上板都要跑的自检里。
     */
    g_kstack_selftest = kstack_selftest();

    {
        uintptr_t    pool   = 0;
        palloc_err_t pe     = palloc_alloc_pages(&g_palloc, KSTACK_POOL_PAGES, &pool);
        vmap_err_t   ve     = VMAP_ERR_NOT_INIT;
        kstack_err_t ke     = KSTACK_ERR_NOT_INIT;

        if (pe == PALLOC_OK) {
            /*
             * ⚠ L1 表是**共用**的那一张(g_mmu_l1_table,链接脚本保证 16KB 对齐),
             *   两个 vmap 实例只是各自负责其中一段地址范围。
             *   分成两个实例的理由见上面"为什么单独一个 vmap 实例"。
             */
            ve = vmap_init(&g_vmap_stack, g_mmu_l1_table, g_l2_pool_stack, (u32)(uintptr_t)g_l2_pool_stack,
                           KSTACK_L2_TABLES, (u32)pool,
                           (u32)pool + (u32)(KSTACK_POOL_PAGES * PALLOC_PAGE_SIZE));
        }

        if (ve == VMAP_OK) {
            ke = kstack_pool_init(&g_kstack, &g_vmap_stack, (u32)pool,
                                  (u32)(KSTACK_POOL_PAGES * PALLOC_PAGE_SIZE), KSTACK_STACK_PAGES,
                                  KSTACK_GUARD_UNMAPPED, g_kstack_bitmap, KSTACK_BITMAP_WORDS,
                                  kstack_tlb_flush_range);
        }

        if (ke == KSTACK_OK) {
            console_printf(" Kernel stack: %u slots x %u KB (+%u KB guard) at 0x%08X\n",
                           g_kstack.slot_count, (KSTACK_STACK_PAGES * KSTACK_PAGE_SIZE) >> 10,
                           KSTACK_PAGE_SIZE >> 10, (u32)pool);

            g_kstack_slots_ok = (g_kstack.slot_count == KSTACK_SLOTS) ? 1u : 0u;
        } else {
            console_printf(" Kernel stack: FAILED palloc=%u vmap=%u kstack=%u\n", (u32)pe, (u32)ve,
                           (u32)ke);
        }

        if (ke == KSTACK_OK) {
            kstack_t a;
            kstack_t b;

            if (kstack_alloc(&g_kstack, &a) == KSTACK_OK && kstack_alloc(&g_kstack, &b) == KSTACK_OK) {
                u32 i;

                /*
                 * 1. guard 页在页表里确实是"没有映射"。
                 *
                 * ⚠ 这是**第二级证据**(读回页表),它证明不了硬件真的会拦住
                 *   访问 —— 那要等 guard_trip。但反过来,这一项失败时
                 *   guard 一定不生效,所以它是必要不充分条件,值得留下。
                 */
                g_kstack_guard_ok =
                    ((vmap_lookup(&g_vmap_stack, a.guard) == 0u) &&
                     (vmap_lookup(&g_vmap_stack, b.guard) == 0u))
                        ? 1u
                        : 0u;

                /*
                 * 2. 栈区真的能读写。
                 *
                 * 抽三页来写:最底一页、中间一页、最顶一页 ——
                 * 少刷了 TLB 的话最可能先在这里炸,而不是等到任务真的跑起来。
                 */
                g_kstack_usable_ok = 1u;
                {
                    const u32 offsets[3] = {0u, (KSTACK_STACK_PAGES / 2u) * KSTACK_PAGE_SIZE,
                                            (KSTACK_STACK_PAGES - 1u) * KSTACK_PAGE_SIZE};

                    for (i = 0; i < 3u; i++) {
                        volatile u32 *w = (volatile u32 *)(uintptr_t)(a.base + offsets[i]);

                        *w = 0x5AA50000u ^ offsets[i];
                    }
                    for (i = 0; i < 3u; i++) {
                        volatile u32 *w = (volatile u32 *)(uintptr_t)(a.base + offsets[i]);

                        if (*w != (0x5AA50000u ^ offsets[i])) {
                            g_kstack_usable_ok = 0u;
                        }
                    }
                }
                /* 栈顶也写一下:那里是初始 SP 所在,必须可用 */
                {
                    volatile u32 *top = (volatile u32 *)(uintptr_t)(a.top - 4u);

                    *top = 0xC0DE0000u;
                    if (*top != 0xC0DE0000u) {
                        g_kstack_usable_ok = 0u;
                    }
                }

                /* 3. 两个槽无缝相邻:b 的 guard 正好是 a 的栈顶 */
                g_kstack_adjacent_ok = (b.guard == a.top && b.base == a.top + KSTACK_PAGE_SIZE) ? 1u : 0u;

                /*
                 * 4. 释放-再分配的真实往返。
                 *
                 * 自检是在合成实例上跑的;这一步才证明**真实的 palloc 页 +
                 * 真实的页表**上这条路径也对 —— 尤其是"释放之后 guard
                 * 重新生效"这一条:漏了它,下一个拿到这个槽的任务就没有 guard。
                 */
                if (kstack_free(&g_kstack, &b) == KSTACK_OK &&
                    kstack_alloc(&g_kstack, &g_kstack_probe) == KSTACK_OK) {
                    g_kstack_reuse_ok = ((g_kstack_probe.slot == b.slot) &&
                                         (g_kstack_probe.guard == b.guard) &&
                                         (vmap_lookup(&g_vmap_stack, g_kstack_probe.guard) == 0u))
                                            ? 1u
                                            : 0u;
                }

                console_printf(" Kernel stack: slot0 base=0x%08X top=0x%08X guard=0x%08X\n", a.base, a.top,
                               a.guard);
                console_printf(" Kernel stack: guard=%s usable=%s adjacent=%s reuse=%s\n",
                               g_kstack_guard_ok ? "PASS" : "FAIL", g_kstack_usable_ok ? "PASS" : "FAIL",
                               g_kstack_adjacent_ok ? "PASS" : "FAIL", g_kstack_reuse_ok ? "PASS" : "FAIL");
            }
        }

        /*
         * 登记破坏性验证的目标栈。
         *
         * 必须在主循环之前 —— fault_test_poll() 在主循环里,而它要能拿到
         * 一个有效的句柄。没登记时 kstack_probe_* 一律返回"未初始化",
         * 不会去解引用空指针。
         */
        if (g_kstack_reuse_ok != 0u) {
            kstack_probe_register(&g_kstack, &g_kstack_probe);
        }
    }

    /* ---- 9.47 AP=0b000 的 guard:小池(A/B 更紧,并补 M2-4 的账)---- */
    {
        uintptr_t    pool = 0;
        palloc_err_t pe   = palloc_alloc_pages(&g_palloc, KSTACK_AP_POOL_PAGES, &pool);
        vmap_err_t   ve   = VMAP_ERR_NOT_INIT;
        kstack_err_t ke   = KSTACK_ERR_NOT_INIT;

        if (pe == PALLOC_OK) {
            /* 这个池小得多,所以另给一份小的 L2 表池 —— 与栈池互不影响 */
            ve = vmap_init(&g_vmap_ap, g_mmu_l1_table, g_l2_pool_ap, (u32)(uintptr_t)g_l2_pool_ap,
                           KSTACK_AP_L2_TABLES, (u32)pool,
                           (u32)pool + (u32)(KSTACK_AP_POOL_PAGES * PALLOC_PAGE_SIZE));
        }

        if (ve == VMAP_OK) {
            ke = kstack_pool_init(&g_kstack_ap, &g_vmap_ap, (u32)pool,
                                  (u32)(KSTACK_AP_POOL_PAGES * PALLOC_PAGE_SIZE), KSTACK_AP_STACK_PAGES,
                                  KSTACK_GUARD_AP_NONE, g_kstack_ap_bitmap, KSTACK_BITMAP_WORDS,
                                  kstack_tlb_flush_range);
        }

        if (ke == KSTACK_OK && kstack_alloc(&g_kstack_ap, &g_kstack_ap_probe) == KSTACK_OK) {
            /*
             * 与"不映射"那一路不同,这一路的 guard 页在页表里**是映射着的** ——
             * 所以这里能做的读回只有"AP 位确实是 0b000"。
             * 真正"硬件会不会拦"要靠选择器 7 的破坏性验证。
             */
            u32 got = vmap_lookup(&g_vmap_ap, g_kstack_ap_probe.guard);

            g_kstack_ap_ok = ((got != 0u) && (got != VMAP_RESULT_SECTION) && (((got >> 4) & 0x3u) == 0u) &&
                              (((got >> 9) & 0x1u) == 0u))
                                 ? 1u
                                 : 0u;

            console_printf(" Ap guard    : pool at 0x%08X base=0x%08X guard=0x%08X AP=%u -> %s\n",
                           (u32)pool, g_kstack_ap_probe.base, g_kstack_ap_probe.guard,
                           (got >> 4) & 0x3u, g_kstack_ap_ok ? "PASS" : "FAIL");

            if (g_kstack_ap_ok != 0u) {
                kstack_probe_register_ap(&g_kstack_ap, &g_kstack_ap_probe);
            }
        } else {
            console_printf(" Ap guard    : FAILED palloc=%u vmap=%u kstack=%u\n", (u32)pe, (u32)ve,
                           (u32)ke);
        }
    }

    /* ---- 9.5 第二个核(AM3-1/2/3) ---- */
    /*
     * 位置:MMU 与缓存都已就绪之后。
     *
     * 为什么必须在这个位置:CPU1 要复用的正是 CPU0 刚建好的那份页表
     * (mmu_enable_secondary() 只写它自己的 TTBR0,不重建表)。
     * 放早了读到的是空表,放晚了无非多等一会儿。
     *
     * 顺序也是硬的:先 per-CPU 表就绪 -> 再放 CPU1 -> 再等它报到。
     * 反过来的话 CPU1 会在 percpu 表还没准备好时就去读自己的结构体。
     */
    {
        bool cpu1_ok;

        percpu_table_reset();

        /*
         * CPU0 自己的表项。传 __stack_top 让记录完整 ——
         * 这条信息之后会被打印出来,用于确认两个核的栈区确实不同。
         */
        /*
         * CPU0 这里可以直接 publish:它的缓存早就开了(见上面缓存那一节),
         * 所以这次写入走 SCU 一致性路径,CPU1 起来后看到的是干净的值。
         */
        if (percpu_init_self((u32)(uintptr_t)__stack_top) != NULL) {
            percpu_publish_self();
        }

        HB[HB_SLOT_CPU1_STAGE]  = HB_CPU1_STAGE_IDLE;
        HB[HB_SLOT_CPU1_ONLINE] = 0u;

        console_puts(" SMP: releasing CPU1 (write 0xFFFFFFF0 + SEV)...\n");

        smp_release_cpu1();
        cpu1_ok = smp_wait_online(1u, CPU1_BOOT_TIMEOUT_US);

        if (cpu1_ok) {
            console_printf(" SMP         : CPU1 online  id=%u mpidr=0x%08X stack=0x%08X\n",
                           g_percpu[1].cpu_id, g_percpu[1].mpidr, (u32)g_percpu[1].stack_top);

            /*
             * ★ M4-10.2:这一步顺带就是"调度器就绪"的握手 ★
             *
             * `online` 由 CPU1 自己置,而且从 M4-10 起它是在
             * `sched_register_ap_idle()` **之后**才置的(见 src/smp.c 第 7 步)——
             * 也就是说 `online == 1` 现在蕴含:
             *     本核 idle 已注册、本核就绪队列已建、current_task 已指向 idle。
             *
             * 这正是源 OS 的 `while (scheduler_is_ready == xsi->cpu_count);`
             * (`main.cpp:581-585`)要保证的事:CPU0 不会往一个还没初始化的
             * 队列里塞线程。所以这里**不需要**再加一次等待 —— 加一次反而
             * 会让两处判据分叉。
             */
            console_printf(" SMP         : CPU1 sched ready (runq=%u idle_pc_zero=%u)\n",
                           sched_cpu_runq_len(1u),
                           (sched_idle_of(1u) != NULL && sched_idle_of(1u)->ctx.pc == 0u) ? 1u : 0u);
            g_cpu1_sched_ready =
                ((sched_idle_of(1u) != NULL) && (sched_cpu_runq_len(1u) >= 1u)) ? 1u : 0u;
        } else {
            /*
             * 不等成功也要如实报出来,而且**不能就此停机** ——
             * 单核状态下其它功能都是好的,把整机拦在这里并不能多查出什么。
             * 心跳里的 stage 会告诉 JTAG 它停在哪一步。
             */
            console_printf(" SMP WARN    : CPU1 did not come online in %u us (stage=%u)\n",
                           CPU1_BOOT_TIMEOUT_US, HB[HB_SLOT_CPU1_STAGE]);
        }

        HB[HB_SLOT_CPU1_ONLINE] = cpu1_ok ? 1u : 0u;
        HB[HB_SLOT_CPU1_LOOPS]  = g_percpu[1].loops;

        /*
         * IPI 的处理函数注册在**共享**的中断表里,所以只需注册一次;
         * 但 SGI 的使能位是按核银行化的,CPU0 要自己开一次。
         * (CPU1 在 cpu1_main 里开它自己那份。)
         */
        if (smp_register_ipi() != 0) {
            console_puts(" SMP WARN    : SGI 0 已被占用,IPI 未启用\n");
        }
        smp_enable_ipi_this_cpu();

        if (cpu1_ok) {
            smp_stress_result_t r = smp_stress_run();

            console_printf(" SMP stress  : lock counter=%u/%u violations=%u cpu1_ran=%u\n", r.counter,
                           2u * SMP_STRESS_ROUNDS, r.violations, r.cpu1_ran);
            console_printf(" SMP IPI     : sent=%u seen=%u\n", r.ipi_sent, r.ipi_seen);

            g_smp_stress = r;
        }
    }

    /* ---- 9.48 异常帧布局的运行时自检(M4-6) ---- */
    /*
     * 位置:在自检报告**之前**(它要往报告里写一项),但要等中断已经开起来 ——
     * 选择器是通过 shell/主循环之外的路径触发的,这里直接调一次即可。
     *
     * 为什么必须真跑一遍:帧布局是汇编与 C 之间的 ABI,`_Static_assert`
     * 只能钉住 C 侧的宏。见 arch/taskctx.h 顶部。
     *
     * ⚠ 这一项依赖 `pc` 的偏移是对的 —— 处理函数靠读 pc 处那条指令的立即数
     *   来判断"这是不是一个布局自检"。偏移错了它就静默地什么都不做,
     *   于是"没跑过"(-1)和"跑过了"必须能被区分开:报告里 -1 会判 FAIL。
     */
    {
        int before = irq_svc_frame_check_result();

        fault_test_trigger(FAULT_SEL_SVC_FRAME);

        g_svc_frame_check = irq_svc_frame_check_result();

        if (before != -1 || g_svc_frame_check == -1) {
            console_printf(" Exc frame   : check did not run (before=%d after=%d)\n", before,
                           g_svc_frame_check);
        } else {
            console_printf(" Exc frame   : %s (result=%d)\n",
                           (g_svc_frame_check == 0) ? "PASS" : "FAIL", g_svc_frame_check);
        }
    }

    /* ---- 9.49 TCB 里 ctx 偏移的运行时自检(M4-6) ---- */
    /*
     * `_Static_assert` 钉住的是"C 结构体 == taskctx_asm.h 的宏";
     * 它钉不住"boot/context.S 里用的是那个宏"。这一步补上后者。
     *
     * TCB 从**内核堆**分配而不是开一个静态的:顺带验证
     *   - TCB 的大小堆得下;
     *   - `heap_alloc` 给的 8 字节对齐满足 TCB 的自然对齐
     *     (`_Alignof(...) == 8`,由 tcb.h 的绊线断言保证)。
     * 用静态实例会把这两件事都跳过。
     *
     * ⚠ 跨语言边界传的是 `&tcb->ctx`(**指针**),不是"TCB 加某个偏移" ——
     *   理由见 arch/tcb.h 里那一节:TCB 有指针字段,宿主与目标布局不同,
     *   而汇编本来也不需要知道 ctx 在 TCB 里的位置。
     */
    {
        struct arm_thread_control_block *tcb = (struct arm_thread_control_block *)heap_alloc(
            &g_heap, (size_t)sizeof(struct arm_thread_control_block));

        if (tcb == NULL) {
            console_puts(" Tcb ctx     : heap_alloc FAILED\n");
        } else {
            u32 i;

            /* C 侧写入已知图案 —— 图案里带下标,所以"读到了别的槽"能被认出 */
            for (i = 0; i < 13u; i++) {
                tcb->ctx.r[i] = ARM_FRAME_CHECK_PATTERN + i;
            }
            tcb->ctx.sp   = ARM_FRAME_CHECK_PATTERN + 100u;
            tcb->ctx.lr   = ARM_FRAME_CHECK_PATTERN + 101u;
            tcb->ctx.pc   = ARM_FRAME_CHECK_PATTERN + 102u;
            tcb->ctx.cpsr = ARM_FRAME_CHECK_PATTERN + 103u;

            g_tcb_ctx_check = (int)arch_ctx_layout_check(&tcb->ctx);

            console_printf(" Tcb ctx     : sizeof=%u align_ok=%u -> %s (result=%d)\n",
                           (u32)sizeof(struct arm_thread_control_block),
                           (((uintptr_t)tcb & 7u) == 0u) ? 1u : 0u,
                           (g_tcb_ctx_check == 0) ? "PASS" : "FAIL", g_tcb_ctx_check);

            (void)heap_free(&g_heap, tcb);
        }
    }

    /* ---- 9.5 异常帧落在**任务的栈**上(M4-7 的第一件事) ---- */
    /*
     * 在这之前,异常帧建在**异常模式自己的栈**上 —— IRQ 用全局的 IRQ 栈,
     * Data Abort 用 ABT 栈。单线程时看不出问题,一旦有了任务就是错的:
     * 两个任务会共用那一段全局栈、互相覆盖现场,而且**不报任何错**。
     *
     * 现在所有异常都先 `srsdb sp!, #MODE_SVC` 落到 SVC 栈(当前任务的栈)上。
     *
     * 判据用链接脚本导出的两段**不相交**的区间:
     *   SVC 栈  [__stack_svc_bottom, __stack_svc_top)
     *   IRQ 栈  [__stack_svc_top,    __stack_top)
     * 所以"帧落在哪一边"是二值的,不存在"看起来差不多"。
     */
    {
        u32 lo     = (u32)(uintptr_t)__stack_svc_bottom;
        u32 hi     = (u32)(uintptr_t)__stack_svc_top;
        u32 irq_hi = (u32)(uintptr_t)__stack_top;
        u32 frame;

        /*
         * 触发一次会返回的异常(SVC)并取回它的帧地址。
         * 选择器 9 顺带跑帧布局自检,所以这一步不额外花时间。
         */
        fault_test_trigger(FAULT_SEL_SVC_FRAME);
        frame = irq_last_svc_frame_addr();

        g_exc_frame_on_svc_stack = ((frame >= lo) && (frame < hi)) ? 1u : 0u;

        console_printf(" Exc stack   : frame=0x%08X  svc=[0x%08X,0x%08X)  irq=[0x%08X,0x%08X)\n",
                       frame, lo, hi, hi, irq_hi);
        console_printf(" Exc stack   : on SVC stack (task stack) = %s\n",
                       g_exc_frame_on_svc_stack ? "YES" : "NO");
    }

        /* ---- 9.6 协作式上下文切换(M4-7 后半段) ---- */
    /*
     * 验收方式:让**两个上下文来回切**,并且每一方都检查
     *   1. 自己是在**自己的栈**上跑的;
     *   2. 自己的局部变量(被调用者保存的那类)在两个栈上各有一份;
     *   3. 切回来之后,调用点的局部状态完好。
     *
     * 为什么要这么验:切换器最要紧的动作是**换栈**。不换栈的话,
     * 被恢复的上下文会跑在别人的栈上 —— 而那件事**不报任何错**,
     * 只在负载上来之后以随机踩栈的形式出现。所以除了"能切过去再切回来",
     * 还必须有一组对照:故意不换栈时,检查必须**失败**。
     */
    {
        kstack_t stk_b;

        if (kstack_alloc(&g_kstack, &stk_b) == KSTACK_OK) {
            volatile u32 local_a = 0xA0A0A0A0u;
            u32          i;

            g_sw_a_sp        = 0u;
            g_sw_b_sp        = 0u;
            g_sw_b_ran       = 0u;
            g_sw_a_local     = (u32)(uintptr_t)&local_a;
            g_sw_b_local     = 0u;
            g_sw_b_local_val = 0u;
            g_sw_a_ret       = 0u;

            /* ---- 给 B 造一个初始上下文 ---- */
            for (i = 0; i < 13u; i++) {
                g_ctx_b.r[i] = 0u;
            }
            /* r0 = B 首次运行时的返回值(切换器会恢复它)*/
            g_ctx_b.r[0]  = 0u;
            /* r4-r11 放图案:B 起来后第一件事就是核对它们还在不在 */
            for (i = 4u; i <= 11u; i++) {
                g_ctx_b.r[i] = SWITCH_REG_PATTERN + i;
            }
            g_ctx_b.sp    = stk_b.top;
            g_ctx_b.lr    = 0u; /* B 不该返回;真返回了会跳到 0,那是明确的错误 */
            g_ctx_b.pc    = (u32)(uintptr_t)switch_probe_b;
            g_ctx_b.cpsr  = 0u;

            g_sw_a_sp = arch_read_sp();

            /* ---- A:自己存一份,然后切过去 ---- */
            g_sw_a_ret = arch_ctx_save(&g_ctx_a);
            if (g_sw_a_ret == 0u) {
                /*
                 * 告诉"将来被切回来的自己":你是被切回来的,不是刚存完。
                 * 这一个字就是 setjmp 约定的全部 —— 见 context.S 的说明。
                 */
                g_ctx_a.r[0] = 1u;
                arch_ctx_switch(&g_ctx_throwaway, &g_ctx_b); /* ★ 不覆盖 g_ctx_a ★ */
            }

            /* ---- 到这说明 B 主动切回来了 ---- */
            g_sw_ctx_a_ok = ((g_sw_a_ret == 1u) && (local_a == 0xA0A0A0A0u)) ? 1u : 0u;
            g_sw_ctx_b_ok = ((g_sw_b_ran == 1u) && (g_sw_b_local_val == 0xB0B0B0B0u) &&
                             (g_sw_b_regs_ok == 1u))
                                ? 1u
                                : 0u;
            /* B 真的跑在**它自己的栈**上吗 —— 这一条才是"换栈"的判据 */
            g_sw_stack_ok = ((g_sw_b_sp >= stk_b.base) && (g_sw_b_sp <= stk_b.top) &&
                             (g_sw_a_sp < stk_b.base))
                                ? 1u
                                : 0u;

            console_printf(" Switch      : a_ret=%u a_sp=0x%08X b_sp=0x%08X b_stack=[0x%08X,0x%08X)\n",
                           g_sw_a_ret, g_sw_a_sp, g_sw_b_sp, stk_b.base, stk_b.top);
            console_printf(" Switch      : ctx_a=%s ctx_b=%s stack_isolated=%s\n",
                           g_sw_ctx_a_ok ? "PASS" : "FAIL", g_sw_ctx_b_ok ? "PASS" : "FAIL",
                           g_sw_stack_ok ? "PASS" : "FAIL");

            (void)kstack_free(&g_kstack, &stk_b);
        } else {
            console_puts(" Switch      : kstack_alloc FAILED\n");
        }
    }

    /* ---- 9.8 协作式调度(M4-8.3) ---- */
    /*
     * 位置:必须在**自检报告之前**,这样它的结论能进报告 ——
     * 而它本身又必须在所有"启动期基础设施"之后(palloc / heap / 栈池),
     * 因为线程要用它们。
     *
     * 做法:把当前(启动)上下文当成一个可回程的参与者 ——
     * `arch_ctx_save` 设一个回程点,然后把控制权交给调度器。
     * 两个线程各跑 8 轮、互相让出,最后切回回程点。
     *
     * 判据(全部是二值的):
     *   - 两个计数都正好到 8:说明两个线程都真的跑到了,
     *     而且**互相让出**都成功了(少一次让出后面这个数就到不了)
     *   - 切换次数明显大于轮数:说明切换真的在发生
     *   - 回程之后 kmain 的局部变量完好:说明切换没有踩坏别人的栈
     */
    {
        volatile u32 local_check = 0xC0FFEE00u;

        sched_kern_bind(&g_kstack, &g_heap);
        sched_kern_init();

        /*
         * ★ M4-10:从这里到相 5 结束,线程**全部落在 CPU0** ★
         *
         * 理由不是"省事",而是**这些判据都是单核判据**:
         *
         *   | 判据 | 一旦线程被分到 CPU1 会变成什么 |
         *   |---|---|
         *   | M4-9 的 `saw=(1,1)` 交错见证 | 两核各跑各的,**永远不会交错** ⇒ 判 FAIL |
         *   | M4-9.5 的 VFP 现场 | 两个核各有自己的 d0-d31 ⇒ "被对方改掉"根本不会发生 ⇒ 判据失去区分能力 |
         *   | M4-8.4 的唤醒延迟 | 纯占用线程在另一个核上 ⇒ 不再构成竞争 ⇒ 判据变得空转 |
         *   | M4-8.5 的无饥饿 | K 个线程被拆到两核上 ⇒ 量的不再是"同一个核上的轮转" |
         *
         * 所以它们必须**显式**钉在 CPU0 —— 这既保住了既有判据的含义,
         * 又顺手成了 M4-10 的对照组(见报告之后那一相):
         * **置 1 = M4-10 之前的行为**。
         *
         * M4-10 自己的判据是**相 6**(它才把开关打开)。
         */
        sched_set_pick_cpu0(1u);
        /*
         * ★ 把排他输出的钩子装上 —— M4-11.1 起它是一把**锁**,不是"关调度" ★
         *
         * `console_excl_begin/end` 的互斥由内核提供,但 `console.c`
         * **不认识调度器** —— 它只持一对函数指针。
         * 装在这里是因为从这一刻起才会有第二个写者(周期状态线程)。
         * 在此之前没装钩子,排他是空操作 —— 那正是对的:只有一个写者。
         *
         * ⚠ 换成 yield 型互斥(源 OS `mutex.cpp`)之后,自检报告那几秒
         *   **调度器照常跑**:状态线程照常醒、照常被调度,只是打印要等锁。
         *   旧做法(关调度)的代价与它为什么在单核假设下才成立,
         *   见 src/mutex_kern.c 与退化清单 D13。
         *
         * ⚠ 顺序:`sched_register_boot_idle()` 必须在**这一步之前**吗?
         *   不必 —— 本函数只创建锁对象;真正取"当前任务"是**第一次打印**时
         *   才发生的,而那一定在 idle 注册之后(下面几行)。
         *   但两者都在这段 `sched_disable()` 窗口里,所以顺序无关紧要。
         */
        console_mutex_init();

        g_yield_a        = 0u;
        g_yield_b        = 0u;
        g_tick_acc_delta = 0u;

        /*
         * ★ 造线程期间必须关掉调度 ★
         *
         * 只要 idle 注册过、队列里有一个可运行的线程,**下一发 tick**
         * 就会把启动流程切走(不是"过一会儿",是下一毫秒)。于是
         * "造第一个线程"与"造第二个线程"之间可能就被切走了 ——
         * 第二个线程根本没被造出来,而自检会以一个看起来毫无道理的方式失败。
         *
         * 这是个真实存在的竞态,不是理论担忧:CPU 667MHz,一次
         * `sched_kthread_create` 是微秒级,而 1kHz 的 tick 是毫秒级 ——
         * 属于"偶尔错一次"的那一类,最难查。
         *
         * ⇒ 用 `sched_disable()` 把造线程的窗口罩起来。
         *   (源 OS 也有这对开关,它用 `disable_scheduler()` /
         *    `enable_scheduler()` 把 `change_proccess` 本身罩住。)
         */
        sched_disable();

        /*
         * ★ 照源 OS:把**启动上下文**注册成 idle(current 从此刻起非空)★
         *   idle 不是另一个线程,而是跑启动流程的这个上下文自己;
         *   它的 ctx.pc = 0 是"上下文无效"的标记 —— 但**只到第一次被切走为止**:
         *   那一刻它的现场被收进 ctx,从此它就是一个可恢复的普通上下文,
         *   于是启动流程才回得来(这正是下面 `g_idle_pc_harvested` 验的事)。
         */
        sched_register_boot_idle();
        g_sched_idle_ok = ((sched_current() == sched_boot_idle()) &&
                           (sched_boot_idle()->ctx.pc == 0u) &&
                           (sched_boot_idle()->task_level == TASK_IDLE_LEVEL))
                              ? 1u
                              : 0u;
        g_idle_pc_was_zero = (sched_boot_idle()->ctx.pc == 0u) ? 1u : 0u;

        /* ---- 相 0:tick 里给 current 计费(照源 OS scheduler.cpp:437)---- */
        /*
         * 做法:造**一个**线程,让它独占着跑一段,量它自己的 vruntime 增长。
         *
         * ⚠ 不能像早先那样"把 current 硬指到某个没在跑的线程上" ——
         *   帧路径一上线,那等于让调度器把**正在跑的 kmain 的现场**
         *   收进那个线程的 ctx。自检的实现方式本身必须跟着机制改。
         *
         * 为什么用一个独跑的线程就够了:它是队列里唯一的一个,
         * `sched_pick_next` 挑不到别人,兜底 `current 可运行 ⇒ 继续跑它`,
         * 于是它确实连续占着 CPU 20ms —— 判据于是是干净的
         * "vruntime 涨了 ≈ 墙钟 20ms"。
         *
         * (早先失败过的那一版测的是 idle 的 vruntime,得到恒 0 ——
         *  而那是**对的**:`sched_account_run` 对 idle 早退,源 OS 的
         *  `charge_current_eevdf_runtime` 也一样,idle 不参与公平分配。
         *  判据写错了不是代码错了。)
         */
        if (sched_kthread_create(acc_probe, NULL, "acc") == NULL) {
            sched_enable();
            console_puts(" Sched acc   : kthread_create FAILED\n");
        } else {
            sched_enable();
            wait_ms_wall(100);
            g_tick_acc_ok = ((g_tick_acc_delta >= 10u * SCHED_TICK_NS) &&
                             (g_tick_acc_delta <= 40u * SCHED_TICK_NS))
                                ? 1u
                                : 0u;
            console_printf(" Sched tick  : vruntime += %u us over 20 ms wall (%s)\n",
                           (u32)(g_tick_acc_delta / 1000u), g_tick_acc_ok ? "PASS" : "FAIL");
        }

        /* ---- 相 1:陷阱式让出(svc #ARM_SVC_YIELD)---- */
        /*
         * ⚠ `sched_disable()` 与 `sched_enable()` **必须成对** ——
         *   包括创建失败那一支。第一版只在成功分支里 enable,于是
         *   一旦 kthread_create 失败(堆或栈池满了),调度器就被永久关掉:
         *   已经建好的线程永远不会被切到,这一相"什么也没测到",
         *   而输出看起来只是一条 FAIL。下一相又会把它打开,
         *   于是错误会以一个和根因毫无关系的样子往下传。
         */
        sched_disable();
        {
            tcb_t ya = sched_kthread_create(yield_probe_a, NULL, "ya");
            tcb_t yb = sched_kthread_create(yield_probe_b, NULL, "yb");

            sched_enable();

            if (ya == NULL || yb == NULL) {
                console_puts(" Sched       : kthread_create FAILED\n");
            } else {
                /*
                 * 两个线程各让出 8 轮。判据是**两边都正好到 8** ——
                 * 等号而不是"大于零":少一次说明让出丢了,多一次说明有重入。
                 *
                 * 跑完之后它们**挂起自己**(sched_park_self),队列于是变空,
                 * 调度器兜底挑 idle —— 而 idle 就是 kmain,于是控制权回到这里。
                 * 整个过程没有一句"手动切回测试点"的代码:这是 M4-9 与 M4-8
                 * 最本质的差别。
                 */
                wait_ms_wall(200);
            }
        }

        g_yield_switches = sched_switch_count();
        g_yield_local_ok = (local_check == 0xC0FFEE00u) ? 1u : 0u;
        g_yield_ok       = ((g_yield_a == YIELD_ROUNDS) && (g_yield_b == YIELD_ROUNDS) &&
                            (g_yield_switches > YIELD_ROUNDS))
                               ? 1u
                               : 0u;

        console_printf(" Sched       : yield a=%u/%u b=%u/%u switches=%u caller_stack=%s\n", g_yield_a,
                       YIELD_ROUNDS, g_yield_b, YIELD_ROUNDS, g_yield_switches,
                       g_yield_local_ok ? "PASS" : "FAIL");
        console_printf(" Sched       : trap yield (svc) = %s\n", g_yield_ok ? "PASS" : "FAIL");

        /* ---- 相 2:★ M4-9 的验收 —— 抢占(帧搬迁)★ ---- */
        /*
         * 两个**绝不主动让出**的线程。它们能不能都动起来,完全取决于
         * 时间片到点时"搬帧"这一步是不是真的发生。
         *
         * 判据见 spin_probe_t 的说明:核心是**两个线程互相见证**,
         * 而不是"两个计数都大于零"(那一条在没有抢占时也成立)。
         */
        sched_disable();
        spin_reset(g_spin);
        {
            tcb_t sa = sched_kthread_create(spin_probe_a, NULL, "sa");
            tcb_t sb = sched_kthread_create(spin_probe_b, NULL, "sb");

            sched_enable();

            if (sa == NULL || sb == NULL) {
                console_puts(" Sched preempt: kthread_create FAILED\n");
            } else {
                wait_ms_wall(SPIN_BUDGET_US / 1000u + 300u);

                g_tick_switched    = sched_tick_switched();
                g_tick_preempted   = sched_tick_preempted();
                g_tick_invalid_ctx = sched_tick_invalid_ctx();

                g_spin_both_done     = (g_spin[0].done && g_spin[1].done) ? 1u : 0u;
                g_spin_ok            = ((g_spin[0].count > 0u) && (g_spin[1].count > 0u) &&
                                        g_spin[0].saw_other && g_spin[1].saw_other)
                                           ? 1u
                                           : 0u;
                g_spin_sp_isolated   = ((g_spin[0].sp_bad == 0u) && (g_spin[1].sp_bad == 0u)) ? 1u : 0u;
                g_preempt_switches_ok = (g_tick_preempted >= 2u) ? 1u : 0u;
                g_idle_pc_harvested  = (sched_boot_idle()->ctx.pc != 0u) ? 1u : 0u;
                /*
                 * ★ 手写常量 vs 板上实测:对一次账 ★
                 *
                 * `idle->ctx.cpsr` 是**被抢占那一刻**从现场帧里收进来的
                 * 真实 CPSR(idle 的初值早已被它覆盖),所以拿它去问
                 * "ARM_CPSR_KERNEL 描述的是内核真正在跑的状态吗" 是一个
                 * 不依赖任何推理的判据。
                 *
                 * 这条判据的由来就是 M4-9 的那次故障:我把新线程的 CPSR
                 * 拼成 0x53,漏了 A 位(真值 0x153),切过去第一条指令
                 * 就吃了异步外部中止。见 arch/taskctx.h 的 ARM_CPSR_KERNEL。
                 */
                g_cpsr_kernel_ok = sched_cpsr_matches_kernel(sched_boot_idle()->ctx.cpsr) ? 1u : 0u;
                g_cpsr_seen      = sched_boot_idle()->ctx.cpsr;

                console_printf(" Sched preempt: a=%u b=%u saw=(%u,%u) sp_bad=(%u,%u) done=%u\n",
                               g_spin[0].count, g_spin[1].count, g_spin[0].saw_other,
                               g_spin[1].saw_other, g_spin[0].sp_bad, g_spin[1].sp_bad,
                               g_spin_both_done);
                console_printf(" Sched preempt: switched=%u preempted=%u invalid=%u idle_ctx=%s\n",
                               g_tick_switched, g_tick_preempted, g_tick_invalid_ctx,
                               g_idle_pc_harvested ? "harvested" : "STILL-INVALID");
                console_printf(" Sched preempt: cpsr_kernel=0x%08X vs real=0x%08X -> %s\n",
                               (u32)ARM_CPSR_KERNEL, g_cpsr_seen,
                               g_cpsr_kernel_ok ? "PASS" : "FAIL(常量与内核实际状态不符!)");
                console_printf(" Sched preempt: interleaving witness = %s\n",
                               g_spin_ok ? "PASS" : "FAIL");
            }
        }

        /* ---- 相 3:★ M4-9.5 的验收 —— 浮点现场 ★ ---- */
        /*
         * 两个线程各自把 d0-d31 填成自己的图案(只填一次),然后反复核对;
         * 还要核对 FPSCR 的舍入模式。切换时若不换浮点现场,后一个把寄存器
         * 改掉之后,先被换下的那个再回来就会读到对方的图案。
         *
         * 判据见 fp_probe_t 上面的说明(尤其是"只填一次"与"至少一个检出")。
         */
        sched_disable();
        fp_reset(g_fp);
        {
            tcb_t fa = sched_kthread_create(fp_probe_a, NULL, "fa");
            tcb_t fb = sched_kthread_create(fp_probe_b, NULL, "fb");

            sched_enable();

            if (fa == NULL || fb == NULL) {
                console_puts(" Sched fp    : kthread_create FAILED\n");
            } else {
                wait_ms_wall(FP_BUDGET_US / 1000u + 300u);

                g_fp_done   = (g_fp[0].done && g_fp[1].done) ? 1u : 0u;
                g_fp_bad    = g_fp[0].bad + g_fp[1].bad;
                g_fp_rm_bad = g_fp[0].rmode_bad + g_fp[1].rmode_bad;
                g_fp_ok     = ((g_fp_bad == 0u) && (g_fp_rm_bad == 0u) && g_fp_done) ? 1u : 0u;

                console_printf(" Sched fp    : bad=(%u,%u) rmode_bad=(%u,%u) done=%u\n", g_fp[0].bad,
                               g_fp[1].bad, g_fp[0].rmode_bad, g_fp[1].rmode_bad, g_fp_done);
                console_printf(" Sched fp    : VFP context across switches = %s\n",
                               g_fp_ok ? "PASS" : "FAIL");
            }
        }

        /* ---- 相 4:★ M4-8.4 —— 周期状态线程(第一个会睡会醒的真实负载)★ ---- */
        /*
         * 造两样东西:
         *   - **状态线程**:永久存在,每秒醒一次,打一行状态(就是原来主循环
         *     里那一段),并记录**唤醒延迟**;
         *   - **纯占用负载**:跑满 2.4 秒、绝不主动让出 —— 用来占住 CPU,
         *     这样"状态线程睡觉期间 CPU 有没有被别人用起来"和
         *     "它醒来之后能不能被及时服务"才是有内容的判据。
         *
         * 三个判据(都来自实测值,不是推理):
         *   wakes    至少醒过 2 次 —— 否则"延迟很小"只是因为压根没睡醒过
         *   late_max 唤醒延迟上限。有个纯占用线程霸着 CPU,它仍应当很小 ——
         *            这就是**顺序**的可观测量(睡醒补偿买的就是这个)
         *   load     负载线程确实推进了很多 —— 说明状态线程睡觉时**真的
         *            让出了 CPU**,而不是忙等
         */
        sched_disable();
        spin_reset(g_spin);
        g_spin[0].budget_us = 2400000u; /* 2.4 秒 —— 是状态周期的两倍多 */
        {
            tcb_t st = sched_kthread_create(status_thread, NULL, "stat");
            tcb_t ld = sched_kthread_create(load_probe, NULL, "load");

            sched_enable();

            if (st == NULL || ld == NULL) {
                console_puts(" Sched sleep : kthread_create FAILED\n");
            } else {
                g_status_created = 1u; /* 判据的适用条件 —— 见 M4-11.1 那两条 */
                /* 3 秒:够状态线程醒 2~3 次,也够负载线程跑完 2.4 秒 */
                wait_ms_wall(3000u);

                g_sleep_wakes_at   = g_status_wakes;
                g_sleep_late_max_at = g_status_late_max_us;
                g_sleep_printed_at = g_status_printed;
                g_sleep_load_count = g_spin[0].count;

                /*
                 * 延迟上限取 20ms。看上去宽松,但它区分的是完全不同的两件事:
                 * 正常路径上它在 1~5ms 量级(时间片 4 tick),而"醒不过来"
                 * 或"被饿着"会是几十上百毫秒甚至永远不醒。
                 * 定成一个"刚好够用"的数会让判据随负载波动而抖 —— 那不是更严,
                 * 只是更脆。
                 */
                g_sleep_ok = ((g_sleep_wakes_at >= 2u) && (g_sleep_late_max_at < 20000u) &&
                              (g_sleep_printed_at >= 2u))
                                 ? 1u
                                 : 0u;
                g_sleep_load_ok = ((g_spin[0].count > 100000u) && g_spin[0].done) ? 1u : 0u;

                console_printf(" Sched sleep : wakes=%u printed=%u late_max=%u us late_last=%u us\n",
                               g_sleep_wakes_at, g_sleep_printed_at, g_sleep_late_max_at,
                               g_status_late_last_us);
                console_printf(" Sched sleep : load thread ran %u iters done=%u\n", g_spin[0].count,
                               g_spin[0].done);
                console_printf(" Sched sleep : 周期睡眠 + 全表扫描唤醒 = %s\n",
                               g_sleep_ok ? "PASS" : "FAIL");
            }
        }
    }

    /* ---- 相 5:★ M4-8.5 —— 无饥饿判据(机制层)★ ---- */
    /*
     * K=4 个**绝不让出**的线程 + 一个睡眠式监视器,判据见 starve_monitor
     * 上面那段。这里只说**与 M4-9 的增量在哪**(不说清楚的话,这一相看起来
     * 只是把 20ms 的探针拉长了):
     *
     *   - K 从 **2 变成 4**:两个线程交错可能只是"你一次我一次";
     *   - 走的是 **avg_vruntime 闸门 + 全表扫描**那条路(不是"队首"),
     *     于是这条判据同时压到了 M4-8.4 改回源 OS 的那套选取;
     *   - 判据是**常驻监视器的逐窗口结算**,不是一次性检查 ——
     *     一次性检查抓不到"跑完那一下才出错"的形态。
     *
     * ⚠ 负载线程必须活得比观测期长(1600ms vs 1200ms):
     *   否则最后一个窗口里它们会挂起,`watched` 不足 K,判据自己先失真。
     */
    sched_disable();
    starve_reset(STARVE_BUDGET_US);
    {
        tcb_t sv[STARVE_K];
        tcb_t sm;

        sv[0] = sched_kthread_create(starve_w0, NULL, "sv0");
        sv[1] = sched_kthread_create(starve_w1, NULL, "sv1");
        sv[2] = sched_kthread_create(starve_w2, NULL, "sv2");
        sv[3] = sched_kthread_create(starve_w3, NULL, "sv3");
        starve_arm(sv, STARVE_K);
        sm = sched_kthread_create(starve_monitor, NULL, "smon");
        sched_enable();

        if (!starve_created(sv) || sm == NULL) {
            console_puts(" Sched nostarve: kthread_create FAILED\n");
        } else {
            u32 i;
            u32 sw0;
            u32 pre0;
            u64 t_wall = timer_read_us(); /* 从调度打开那一刻起算 —— 那才是"相" */

            sw0  = sched_tick_switched();
            pre0 = sched_tick_preempted();
            wait_ms_wall(STARVE_PHASE_MS);
            t_wall = timer_read_us() - t_wall;

            g_starve_full_at    = g_starve_full;
            g_starve_events_at  = g_starve_events;
            g_starve_late_at    = g_starve_late;
            g_starve_skip_at    = g_starve_skipped;
            g_starve_win_max_at = g_starve_win_max;
            g_starve_ticks_at   = g_starve_ticks;
            g_starve_ms_at      = (u32)(t_wall / 1000u);
            g_starve_adv_at     = starve_advanced();

            /*
             * 四项判据,缺一不可:
             *   events == 0  ★ 没有任何"可运行却没推进"的 (线程,窗口) 对
             *   late   == 0     监视器自己也没被饿着(窗口没长得离谱)
             *   full   >= 8     **判据非空转**:真有那么多个窗口里 4 个线程都
             *                   处于可运行状态 —— 否则"零违反"可能只是
             *                   "压根没观察"(那种判据等于没判)
             *   adv    == K     4 个线程都真的在推进(负载是真的)
             */
            g_starve_ok = ((g_starve_events_at == 0u) && (g_starve_late_at == 0u) &&
                           (g_starve_full_at >= STARVE_MIN_FULL) && (g_starve_adv_at == STARVE_K))
                              ? 1u
                              : 0u;

            console_excl_begin(); /* 串口有两个写者:整段排他,免得被状态行劈开 */
            console_printf(" Sched nostarve: K=%u windows=%u full=%u events=%u late=%u skipped=%u\n",
                           STARVE_K, g_starve_checks, g_starve_full_at, g_starve_events_at,
                           g_starve_late_at, g_starve_skip_at);
            /*
             * 第二行是**覆盖率审计**:相持续了多少毫秒(墙钟)*对比*监视器一共
             * 看住了多少 tick。两者应当接近(1 tick ≈ 1ms);差得多就说明
             * "零违反"是因为**没在看**,而不是因为没问题。
             */
            console_printf(" Sched nostarve: phase=%u ms observed=%u tick win_max=%u adv=%u/%u\n",
                           g_starve_ms_at, g_starve_ticks_at, g_starve_win_max_at, g_starve_adv_at,
                           STARVE_K);
            console_printf(" Sched nostarve: counts=%u,%u,%u,%u\n", g_starve[0].count,
                           g_starve[1].count, g_starve[2].count, g_starve[3].count);
            /*
             * 第三行是**时机对账**:每个负载线程"离本相开始多久"退出,以及
             * 这段时间里真的切换了多少次。相的长度(上面那一行)与它们之间的
             * 差额就是"没有负载在跑、启动流程却没回来"的那一段 ——
             * 上板实测它是 ~1.3 秒,而原因**尚未定位**(预先存在:相 4 也有)。
             */
            console_printf(" Sched nostarve: exit_ms=%u,%u,%u,%u done=%u,%u,%u,%u sw=%u pre=%u\n",
                           g_starve[0].exit_ms, g_starve[1].exit_ms, g_starve[2].exit_ms,
                           g_starve[3].exit_ms, g_starve[0].done, g_starve[1].done, g_starve[2].done,
                           g_starve[3].done, sched_tick_switched() - sw0,
                           sched_tick_preempted() - pre0);
            console_printf(" Sched nostarve: every runnable thread advanced in every window = %s\n",
                           g_starve_ok ? "PASS" : "FAIL");
            console_excl_end();

            /*
             * 观测期结束:把负载线程挂起,并**撤销观察名单**。
             *
             * ⚠ 不撤销也能过(监视器会因 status 不是可运行而跳过它们),
             *   但那是"碰巧安全":名单里留着已死线程,下一次改动的任何
             *   差池都会被读成饥饿。显式撤销,让"现在没有东西在被监视"
             *   这件事是**说出来的**,而不是推出来的。
             */
            for (i = 0u; i < STARVE_K; i++) {
                sv[i]->status      = WAIT;
                sv[i]->wakeup_time = 0u;
            }
            starve_arm(NULL, 0u);
        }
    }

    /* ---- 相 6:★ M4-10 —— 两个核都在跑线程 ★ ---- */
    /*
     * 与前面每一相的差别只有一个:**这一相把选核打开**
     * (`sched_set_pick_cpu0(0)`),于是线程按源 OS 的规矩落到
     * "队列最短的那个核"(`scheduler.cpp:537-549`)。
     *
     * 判据(全部是可读的数,不是"看起来在跑"):
     *   cpu1_switched   CPU1 的 `switched` 增量 > 0 —— 它在搬帧
     *   cpu1_preempted  CPU1 的 `preempted` 增量 > 0 —— 被换下的是**真实线程**
     *                   (只切 idle 的话这个数永远是 0,那不算"在调度")
     *   cpu0_preempted  CPU0 也在抢占真实线程(不是把活全推给 CPU1)
     *   ap_idle_ctx     CPU1 的 idle 现场被收过(`ctx.pc != 0`)—— 它被切走过
     *   ap_idle_back    线程挂起之后 CPU1 **回到了自己的 idle**(见下)
     *   placement       ★ 每个新线程都落在"造它之前更短的那个队列"上 ★
     *
     * ## ★ 为什么判据是 placement 而不是"两个核各分到一半" ★
     *
     * 第一版我要求"两个核都分到线程"—— 上板立刻证明那是**错的期望**:
     * 源 OS 的规则比的是**队列长度**,而队列是"全部线程的**名册**"
     * (M4-8.4 对齐源 OS 之后就是这样:睡着的、挂起的都还在里面)。
     * 启动到这一相时 CPU0 的名册上已经躺着十几个已挂起的探针,
     * 而 CPU1 只有它自己的 idle ⇒ **每一个新线程都会落到 CPU1**,
     * 直到 CPU1 也攒到同样多的名册项。
     *
     * 实测(那一轮):`assign=(0,6) ran=(0,6)`,而两个核**都在正常调度**。
     * ⇒ 判据要测的是"**规则有没有被正确执行**"(拿造线程之前的两个队列长度
     *   去对),而不是"负载有没有均分" —— 后者是这套规则的已知性质,
     *   不是它的判据。**这也说明"判据得对着机制写,不能对着愿望写"。**
     *
     * ⚠ 负载必须**会自己挂起**(跑完就 park):两个核上的线程都不退出的话,
     *   kmain 拿不回 CPU —— 那是"自检把系统挂住",本项目最怕的失败模式。
     */
    {
        tcb_t st[SMP_SCHED_THREADS];
        tcb_t at = NULL;
        u32   i;
        u32   sw0;
        u32   sw1;
        u32   pre0;
        u32   pre1;
        u32   made = 0u;
        u32   place_bad = 0u;

        sched_set_pick_cpu0(0u); /* ★ 打开选核 —— 这才是 M4-10 的行为 ★ */

        g_smp_ran[0] = 0u;
        g_smp_ran[1] = 0u;
        g_smp_assign[0] = 0u;
        g_smp_assign[1] = 0u;
        smp_probe_reset();

        sw0  = g_percpu[0].switched;
        sw1  = g_percpu[1].switched;
        pre0 = g_percpu[0].preempted;
        pre1 = g_percpu[1].preempted;

        for (i = 0u; i < SMP_SCHED_THREADS; i++) {
            u32 len0 = sched_cpu_runq_len(0u);
            u32 len1 = sched_cpu_runq_len(1u);
            u32 expect = (len1 < len0) ? 1u : 0u; /* 严格小于 ⇒ 平局给 CPU0 */

            sched_disable(); /* 每个线程各罩一次:不要把整个系统冻住一整段 */
            st[i] = sched_kthread_create(smp_sched_probe, &g_smp_probe[i], "smp");
            sched_enable();

            if (st[i] == NULL) {
                continue;
            }
            made++;
            if (st[i]->cpu_id < PERCPU_MAX_CPUS) {
                g_smp_assign[st[i]->cpu_id]++;
            }
            /* ★ 落在"造它之前更短的那个队列"上了吗 ★ */
            if (st[i]->cpu_id != expect) {
                place_bad++;
            }
        }

        /*
         * ★ 应用级线程:必须落到 CPU0 —— 哪怕 CPU1 的队列更短 ★
         *
         * 这一条是 M4-10.5 那条规则的**可执行判据**。没有它,那个 `if`
         * 就是一段"编译过、今天不可能被执行"的代码 —— 而按本项目的规矩,
         * 那不算实现(要在这台板上被跑过)。
         *
         * ⚠ 它落在 CPU0 之后会**正常跑起来**(应用级不是 idle,是可调度的),
         *   所以它同时补上了另一件事:`ran[0] > 0`(CPU0 上真的跑过**我的**
         *   探针,而不只是那个 1Hz 状态线程)。
         */
        {
            tcb_t a;

            sched_disable();
            a = sched_kthread_create_level(smp_sched_probe, &g_smp_probe[SMP_SCHED_THREADS], "app",
                                           TASK_APPLICATION_LEVEL);
            sched_enable();

            at = a;
            if (a != NULL) {
                made++;
                g_smp_app_cpu = a->cpu_id;
                if (a->cpu_id < PERCPU_MAX_CPUS) {
                    g_smp_assign[a->cpu_id]++;
                }
            }
        }

        {
            if (made == 0u) {
                console_puts(" Sched smp   : kthread_create FAILED\n");
            } else {
                /* 预算 40ms,给足 10 倍余量 —— 两个核都在抢 CPU,慢一点正常 */
                wait_ms_wall(400u);

                /*
                 * ★ "CPU1 回到自己的 idle 了吗" ★
                 *
                 * idle 的循环体就是 `pc->loops++` —— 所以这个计数器**只在
                 * idle 真的在跑的时候才涨**。相 6 的探针都已经挂起了,
                 * 于是再等一小段,CPU1 的 `loops` 必须继续涨:
                 *
                 *   涨  ⇒ 它从"最后一个线程挂起"回到了 idle 的循环里
                 *   不涨 ⇒ 它卡在那个已挂起的线程上(该核永久停摆,
                 *          而另一个核看起来一切正常 —— 计划里那句
                 *          "AP 的 idle 只被切走、不被切回"如果照字面实现,
                 *          得到的就是这个形态)
                 *
                 * 这一条正是 M4-10 调研时**纠正**的那个断言的可执行版本。
                 */
                {
                    u32 l1a = g_percpu[1].loops;

                    wait_ms_wall(100u);
                    g_smp_ap_idle_back = (g_percpu[1].loops > l1a) ? 1u : 0u;
                }

                g_smp_cpu1_switched_at  = g_percpu[1].switched - sw1;
                g_smp_cpu1_preempted_at = g_percpu[1].preempted - pre1;
                g_smp_cpu0_preempted_at = g_percpu[0].preempted - pre0;
                g_smp_place_bad         = place_bad;
                g_smp_made              = made;
                g_smp_app_created       = (at != NULL) ? 1u : 0u;
                g_smp_ap_idle_harvested =
                    ((sched_idle_of(1u) != NULL) && (sched_idle_of(1u)->ctx.pc != 0u)) ? 1u : 0u;

                /*
                 * ★ 浮点现场:按"探针自己观测到的核号"分类汇总 ★
                 *
                 * 落在 **CPU1** 上的那些探针核对过 `fp_checks` 次,`fp_bad`
                 * 必须为 0 —— 这一条补的正是"CPU1 的 VFP 存/取从没被验过"
                 * 那个缺口(10.6 删掉全局 `g_vfp_save_f` 的理由就是它两核会互踩)。
                 *
                 * ⚠ `fp_checks_cpu1 > 0` 是**非空转**条件:没有它,
                 *   "CPU1 上一个探针都没落"与"落了且全对"会长得一样。
                 */
                {
                    u32 fp_bad_all = 0u;
                    u32 fp_checks_cpu1 = 0u;
                    u32 fp_bad_cpu1 = 0u;

                    for (i = 0u; i < SMP_SCHED_THREADS + SMP_APP_THREADS; i++) {
                        fp_bad_all += g_smp_probe[i].fp_bad;
                        if (g_smp_probe[i].cpu_seen == 1u) {
                            fp_bad_cpu1 += g_smp_probe[i].fp_bad;
                            fp_checks_cpu1 += g_smp_probe[i].fp_checks;
                        }
                    }

                    g_smp_fp_bad       = fp_bad_all;
                    g_smp_fp_bad_cpu1  = fp_bad_cpu1;
                    g_smp_fp_ck_cpu1   = fp_checks_cpu1;
                }

                g_smp_sched_ok =
                    ((g_smp_cpu1_switched_at > 0u) && (g_smp_cpu1_preempted_at > 0u) &&
                     (g_smp_cpu0_preempted_at > 0u) && (g_smp_ap_idle_harvested != 0u) &&
                     (g_smp_ap_idle_back != 0u) && (g_smp_ran[0] > 0u) && (g_smp_ran[1] > 0u) &&
                     (made == SMP_SCHED_THREADS + SMP_APP_THREADS))
                        ? 1u
                        : 0u;

                console_excl_begin();
                console_printf(" Sched smp   : threads=%u/%u assign=(%u,%u) ran=(%u,%u) place_bad=%u\n",
                               made, SMP_SCHED_THREADS + SMP_APP_THREADS, g_smp_assign[0],
                               g_smp_assign[1], g_smp_ran[0], g_smp_ran[1], place_bad);
                console_printf(" Sched smp   : cpu1 switched=+%u preempted=+%u idle_harvested=%u back=%u\n",
                               g_smp_cpu1_switched_at, g_smp_cpu1_preempted_at,
                               g_smp_ap_idle_harvested, g_smp_ap_idle_back);
                console_printf(" Sched smp   : cpu0 switched=+%u preempted=+%u  runq=(%u,%u)\n",
                               g_percpu[0].switched - sw0, g_smp_cpu0_preempted_at,
                               sched_cpu_runq_len(0u), sched_cpu_runq_len(1u));
                console_printf(" Sched smp   : app_level cpu=%u (must be 0)  fp: bad_all=%u "
                               "cpu1_bad=%u cpu1_checks=%u\n",
                               g_smp_app_cpu, g_smp_fp_bad, g_smp_fp_bad_cpu1, g_smp_fp_ck_cpu1);
                console_printf(" Sched smp   : two cores scheduling = %s\n",
                               g_smp_sched_ok ? "PASS" : "FAIL");
                console_excl_end();
            }
        }

        /*
         * ⚠ 选核开关**留着不动**(仍为 0):报告之后的对照组要从这个状态出发。
         *   ⚠ 但报告之后的**每一组对照都是单核判据**,所以 9.75 之前会把它
         *     再钉回 1 —— 上板实测漏了这一步时 VFP 对照组直接失去区分能力。
         *     见 9.75 之前那段说明。
         */
    }

    /* ---- 10. 启动自检总账 ---- */
    /*
     * 位置:所有自检都跑完之后、主循环之前。
     *
     * 在此之前,"上板验证"靠人读日志判断"看起来没问题"——
     * 既不可自动化(改一行就要重看一遍),判据也模糊
     * ("speedup=6x 算不算通过"没有写在任何地方)。
     * 这一段把判据写成代码并以固定格式回传,由 tmp-test/verify_board.py
     * 解析并给出退出码。人只需要看最后那行 SUMMARY。
     *
     * 注意这里**只报告、不停机**:自检的意义是给出信息,
     * 而不是把一个本来能跑的系统拦在启动阶段。
     * 失败项数会写进心跳,挂死时用 JTAG 也能读到结论。
     */
    /*
     * ⚠ `irq_frame_unaligned8` **故意不作为自检项**,而且**必须打在报告区间之外**。
     *
     * 它不是"错了多少次",而是"有多少次被中断的代码 SP 是 4 mod 8"——
     * 而那完全合法(AAPCS 只要求调用点 8 字节对齐)。把它写成
     * `expect 0` 会让一个正常现象变成一条 FAIL,而 FAIL 多了就没人看了。
     *
     * 位置放在 `selftest_begin()` **之前**:验证脚本对报告区间内的非 CHECK 行
     * 是**拒绝**的(它宁可报错也不肯漏检),所以信息性输出不能夹在里面 ——
     * 这一点第一次就被它当场抓到了。
     * (心跳区 32 个槽已经用满,所以只打印、不占槽。)
     */
    console_printf(" Irq frame   : 4-mod-8 SP seen %u times (legal, informational)\n",
                   irq_frame_unaligned8());
    /*
     * ★ D7:被中断的 SP 是 4 mod 8 是**合法**的(所以上面那条只统计、不判失败),
     *   但**调 C 处理函数那个边界**必须 8 字节对齐 —— 那是硬的,由
     *   `_vec_irq`/`_vec_svc` 里的 `bic sp, sp, #7` 保证。
     *   这里打出来是为了让"修之前/之后"在日志上直接可比;
     *   真正的判据是报告里的 `c_handler_sp_aligned`。
     */
    console_printf(" Irq frame   : C handler entry SP misaligned %u times (修复前它应当等于上面那个数)\n",
                   c_handler_sp_violations());

    selftest_begin();
    /*
     * ★ 整个报告区间排他 ★
     *
     * 理由见 console.h:报告是**机器可读的判定通道**,而 M4-8.4 之后
     * 状态行线程随时可能插进来把某一行劈成两半 —— 上板实测过一次,
     * `=== SELF-TEST END ===` 被劈开,`verify_board.py` 直接判"报告不完整"。
     *
     * ★ M4-11.1:这个排他从"关调度"换成了**一把 yield 型互斥**(D13 结案)★
     *
     * 换之前:报告打几秒钟,这几秒里**整个系统停摆** —— 所有线程都醒不过来,
     * 于是饥饿监视器必须把"停摆"从"饥饿"里减掉(`sched_off_total_ns`)。
     * 换之后:调度器照常跑,状态线程照常醒、照常被调度,只是**打印要等锁**。
     *
     * 两件事因此可以直接量出来,而且都写进了报告:
     *   `console_excl_sched_off_ms` —— 报告区间里"调度被人为关掉"的毫秒数
     *                                 (旧做法 = 整个报告长度,现在应当 ≈ 0);
     *   `console_excl_alive_wakes`  —— 报告区间里状态线程**醒过几次**
     *                                 (旧做法恒为 0 —— 它压根没机会跑)。
     *
     * ⚠ 一个必须写清的边界:报告通道是排他的,但 kmain 里那些**没有包在
     *   `console_excl_*` 里的打印**(还有 100 多处)是"尽力而为"的 ——
     *   状态行有可能插进它们中间。旧做法下它们**顺带**受保护(那时排他 =
     *   关调度,而关着调度就只有本核一个写者)。
     *   ⇒ 机器判定的通道不受影响(它包住了),人读的诊断行可能被劈开。
     *     这是有意的取舍:把整段 `console_printf` 都做成取锁的,会让
     *     **异常处理路径上的打印**(它不能睡眠)与持锁者撞成死锁。
     */
    g_report_off0    = sched_off_total_ns();
    g_report_wakes0  = g_status_wakes;
    console_excl_begin();

    selftest_report("uart_present", uart_present ? 1u : 0u, 1u, SELFTEST_EQ);
    selftest_report("uart_clock_source", clock_source, 1u, SELFTEST_EQ);
    selftest_report("uart_baud_ppm", baud_result.error_ppm, 50u, SELFTEST_LE);
    /*
     * 收发通路自检的结果在串口初始化之后就已经拿到(见前面的说明):
     * 它必须在没有任何待发输出、且报告区间之外的时刻执行。
     */
    selftest_report("uart_loopback", uart_loopback_ok ? 1u : 0u, 1u, SELFTEST_EQ);

    selftest_report("mmu_stage", HB[HB_SLOT_MMUSTAGE], HB_MMU_STAGE_ON, SELFTEST_EQ);
    selftest_report("mmu_enabled", mmu_is_enabled() ? 1u : 0u, 1u, SELFTEST_EQ);

    selftest_report("cache_dcache_on", cache_dcache_enabled() ? 1u : 0u, 1u, SELFTEST_EQ);
    selftest_report("cache_icache_on", cache_icache_enabled() ? 1u : 0u, 1u, SELFTEST_EQ);
    selftest_report("cache_speedup", HB[HB_SLOT_CACHEBENCH], 2u, SELFTEST_GE);
    selftest_report("cache_maint_fail", HB[HB_SLOT_CACHESELFTEST], 0u, SELFTEST_EQ);
    selftest_report("l2_enabled", l2_cache_is_enabled() ? 1u : 0u, 1u, SELFTEST_EQ);
    selftest_report("l2_effect_pct", HB[HB_SLOT_L2BENCH], 120u, SELFTEST_GE);

    /*
     * 设备描述层。
     *
     * 判据是"**至少有一个设备被认领**",而不是硬编码的设备总数 ——
     * 总数会随 XSA 变,写死它等于把一次硬件改动变成一次测试失败,
     * 而那种失败没有任何信息量。
     */
    selftest_report("board_devices", g_board_device_count, 1u, SELFTEST_GE);
    selftest_report("probe_probed", HB[HB_SLOT_PROBED], 1u, SELFTEST_GE);
    selftest_report("led_writeback_fail", HB[HB_SLOT_LEDCHECK], 0u, SELFTEST_EQ);

    /*
     * 中断子系统。
     *
     * ticks 与 irq_count 必须相等:前者是处理函数里自增的,
     * 后者是 GIC 实际转发次数。两者不符说明有中断被吞或未被 EOI,
     * 而那种情况从单个计数器上看一切正常。
     */
    selftest_report("irq_ticks_eq_irq", (g_tick_seen == irq_get_stats()->irq_count) ? 1u : 0u, 1u,
                    SELFTEST_EQ);
    selftest_report("irq_spurious", irq_get_stats()->spurious_count, 0u, SELFTEST_EQ);

    /*
     * ---- 第二个核(AM3) ----
     *
     * 这几项刻意分开报,而不是合成一个 "smp_ok":
     * CPU1 起不来有若干种截然不同的原因(没被唤醒 / 卡在 MMU / 卡在缓存 /
     * 起来了但没置 online),而心跳里的 stage 槽正好区分它们。
     * 合成一项会把这条线索丢掉。
     */
    selftest_report("vmap_selftest", g_vmap_selftest, 0u, SELFTEST_EQ);
    selftest_report("vmap_board_split", g_vmap_split_ok, 1u, SELFTEST_EQ);
    selftest_report("vmap_board_live", g_vmap_live_ok, 1u, SELFTEST_EQ);

    selftest_report("heap_selftest", g_heap_selftest, 0u, SELFTEST_EQ);
    selftest_report("heap_smoke", g_heap_smoke, 1u, SELFTEST_EQ);
    /* 至少 32MB 可用 —— 判据写小了等于没判 */
    selftest_report("heap_size_ok", (g_heap.total_bytes >= (32u * 1024u * 1024u)) ? 1u : 0u, 1u, SELFTEST_EQ);

    selftest_report("palloc_selftest", g_palloc_selftest, 0u, SELFTEST_EQ);
    selftest_report("palloc_smoke", g_palloc_smoke, 1u, SELFTEST_EQ);
    /* 池至少要有 100000 页(约 390MB)—— 数字写小了等于没判 */
    selftest_report("palloc_pages_ok", (g_palloc.page_count >= 100000u) ? 1u : 0u, 1u, SELFTEST_EQ);

    /*
     * ---- 内核栈与 guard page(M4-5)----
     *
     * ⚠ 这几项都是**第二级证据**(读回页表 / 写回读),它们证明不了
     *   "guard 真的会拦住越界访问"。那要第三级证据:故意写溢出,
     *   看 DFAR 是否落在 guard 页里 —— 见 fault_test 的选择器 6/7/8
     *   与 tmp-test/guard_trip.py。
     *
     *   "guard 页在页表里是 0"与"访问它会产生 Data Abort"是两件事:
     *   前者在 TLB 里还留着拆段之前的段表项时照样成立,
     *   而那正是最可能出的错(改完页表忘了失效 TLB)。
     */
    selftest_report("kstack_selftest", g_kstack_selftest, 0u, SELFTEST_EQ);
    selftest_report("kstack_slots", g_kstack_slots_ok, 1u, SELFTEST_EQ);
    selftest_report("kstack_guard_unmapped", g_kstack_guard_ok, 1u, SELFTEST_EQ);
    selftest_report("kstack_usable", g_kstack_usable_ok, 1u, SELFTEST_EQ);
    selftest_report("kstack_adjacent", g_kstack_adjacent_ok, 1u, SELFTEST_EQ);
    selftest_report("kstack_free_reuse", g_kstack_reuse_ok, 1u, SELFTEST_EQ);
    /* AP=0b000 那一路:guard 页是**映射着的**,拦住访问的是 AP 而不是"没映射" */
    selftest_report("kstack_ap_guard_ap0", g_kstack_ap_ok, 1u, SELFTEST_EQ);

    /*
     * ---- 异常帧布局(M4-6)----
     *
     * 判据是 == 0,而不是 ">= 0":没跑过(-1)必须算失败 ——
     * 否则"汇编没按宏存"与"自检根本没触发"在报告上长得一样。
     */
    selftest_report("exc_frame_layout", (u32)((g_svc_frame_check < 0) ? 0xFFFFFFFFu
                                                                     : (u32)g_svc_frame_check),
                    0u, SELFTEST_EQ);

    /*
     * ---- TCB 里 ctx 的偏移(M4-6)----
     *
     * 同样是 == 0 而不是 >= 0:-1(没跑过)必须算失败。
     */
    selftest_report("tcb_ctx_layout", (u32)((g_tcb_ctx_check < 0) ? 0xFFFFFFFFu
                                                                 : (u32)g_tcb_ctx_check),
                    0u, SELFTEST_EQ);

    /*
     * ---- 异常帧的落点(M4-7)----
     *
     * 这是 M4-7"把异常帧搬到任务的栈上"这一条的验收判据。
     * 旧实现(帧建在 IRQ 模式自己的栈上)会让它判 FAIL ——
     * 而且那个失败是**可复现**的,不是概率性的:两段栈区在链接脚本里不相交。
     */
    selftest_report("exc_frame_on_task_stack", g_exc_frame_on_svc_stack, 1u, SELFTEST_EQ);

    /*
     * ---- IRQ 返回地址的结构自检(M4-7)----
     *
     * 判据:每一个 IRQ 帧都必须满足 `ret == pc + 4`。漏掉"按异常类型修正 LR"
     * 那一步时两者会取到同一个原始值,于是**每条中断跳过一条指令** ——
     * 而那个 bug 的表现是随机的 Data Abort,光看现场根本猜不到根因。
     *
     * 到自检这一刻已经过去了上千个 tick,所以这个计数是"每帧都查"的累积结果,
     * 不是抽样。必须恒为 0。
     */
    selftest_report("irq_frame_violations", irq_frame_violations(), 0u, SELFTEST_EQ);

    /*
     * ---- ★ D7:C 处理函数入口的 SP 必须 8 字节对齐 ★ ----
     *
     * AAPCS 要求"公开接口处 SP 8 字节对齐",而异常帧的位置是 `S - 64`,
     * 会继承被中断栈的对齐 —— 实测一轮启动里有 20~28 次 S 是 4 mod 8
     * (被中断的代码在 libgcc 的 Thumb 函数 `__udivmoddi4` 里)。
     * 修法是在 `bl` 之前把 SP 对齐(帧不动),这一条直接量它。
     */
    selftest_report("c_handler_sp_aligned", c_handler_sp_violations(), 0u, SELFTEST_EQ);

    /*
     * ---- 协作式上下文切换原语(M4-7 后半段)----
     *
     * 这一项验的是 `arch_ctx_save` / `arch_ctx_switch` **这两个原语本身**,
     * 与调度器无关(调度器 M4-9 之后不再用它 —— 它走帧路径)。
     * 保留它是因为"保存/恢复被调用者保存寄存器与栈"这件事仍然是
     * 后续可睡眠原语(M4-11)的基础。
     *
     * 三项分开报,因为它们失败的原因完全不同:
     *   ctx_a      切回来之后 A 的调用点状态没保住(r4-r11 / sp 恢复错了)
     *   ctx_b      B 没跑起来,或它看到的 r4-r11 不是切换器恢复的那些
     *   stack      最要紧的一项:B 跑在**别人的栈**上(不换栈不会报任何错)
     */

    /*
     * ---- idle 的注册方式(照源 OS)----
     *
     * idle 是**启动上下文自己**,不是另一个线程。两项判据:
     *   - 注册后 `current_task` 就是它;
     *   - 它的 `ctx.pc == 0` —— 源 OS 里 `context0.rip == 0` 的对应物,
     *     含义是"**还没被切走过**,现场在 CPU 里不在内存里,不许切进来"。
     */
    selftest_report("sched_boot_idle", g_sched_idle_ok, 1u, SELFTEST_EQ);

    /*
     * ---- tick 里给 current 计费(← `scheduler.cpp:437`)----
     *
     * 一个线程独占 CPU 跑 20ms 墙钟,它自己的 vruntime 应当涨 ≈ 20ms。
     * 判据留了余量(10..40ms):取样期间 tick 数是变化的,精确值由宿主单测保证。
     */
    selftest_report("sched_tick_account", g_tick_acc_ok, 1u, SELFTEST_EQ);

    /* ---- 陷阱式让出(M4-9):svc 进调度器,与抢占同一条路径 ---- */
    selftest_report("sched_yield_rounds", g_yield_ok, 1u, SELFTEST_EQ);
    selftest_report("sched_switches", (g_yield_switches > YIELD_ROUNDS) ? 1u : 0u, 1u, SELFTEST_EQ);
    selftest_report("sched_caller_intact", g_yield_local_ok, 1u, SELFTEST_EQ);

    /*
     * ---- ★ M4-9 的验收:抢占(搬帧)★ ----
     *
     * 五项分开报,因为它们的失败原因完全不同、排查方向也完全不同:
     *
     *   interleave   两个**互不见证**都成立 —— 这是唯一能区分
     *                "真的交错"与"两个线程先后各跑一遍"的判据。
     *   preempted    被换下的是真实线程(不是 idle)的次数 >= 2:
     *                说明时间片到点真的把线程换下来了。
     *   sp_isolated  两个线程的 sp **一次都没有**掉出自己那块栈区 ——
     *                "帧搭在目标任务栈上"这件事的直接判据。
     *   both_done    两个线程都跑完了各自那 20ms 墙钟预算。
     *   idle_ctx     启动流程的现场**真的被收进了 idle 的 ctx**
     *                (pc 从 0 变成非 0)。它同时是"kmain 被切走了"
     *                与"kmain 又被切回来了"这两件事的证据:
     *                没有前者这个值不会变,没有后者这一行根本打不出来。
     */
    selftest_report("sched_preempt_interleave", g_spin_ok, 1u, SELFTEST_EQ);
    selftest_report("sched_preempt_switched", g_preempt_switches_ok, 1u, SELFTEST_EQ);
    selftest_report("sched_preempt_sp_isolated", g_spin_sp_isolated, 1u, SELFTEST_EQ);
    selftest_report("sched_preempt_both_done", g_spin_both_done, 1u, SELFTEST_EQ);
    selftest_report("sched_idle_ctx_harvested", g_idle_pc_harvested, 1u, SELFTEST_EQ);
    selftest_report("sched_idle_pc_was_zero", g_idle_pc_was_zero, 1u, SELFTEST_EQ);
    /*
     * ---- 手写的 CPSR 常量 vs 板上实测 ----
     *
     * 这一条防的不是"代码写错了",而是"**我推理出来的那几位不全**"。
     * M4-9 就栽在这里:新线程的 CPSR 我拼成 0x53,真值是 0x153 ——
     * 差的 A 位一被清掉,异步外部中止立刻在随机位置投递。
     * 完整经过见 arch/taskctx.h 的 ARM_CPSR_KERNEL。
     */
    selftest_report("sched_cpsr_kernel", g_cpsr_kernel_ok, 1u, SELFTEST_EQ);

    /*
     * ---- ★ M4-9.5:浮点现场随切换保存/恢复 ★ ----
     *
     * 三项分开:
     *   fp_vfp_context  两个线程的 d0-d31 与 FPSCR 舍入模式都没被对方改过
     *   fp_done         两个线程都真的跑完了(否则上面那条可能只是因为
     *                   "根本没被抢占过" —— 那就什么也证明不了)
     *   fp_switch       期间真的发生过切换(同上,防"没切所以没错")
     */
    selftest_report("sched_fp_vfp_context", g_fp_ok, 1u, SELFTEST_EQ);
    selftest_report("sched_fp_done", g_fp_done, 1u, SELFTEST_EQ);
    selftest_report("sched_fp_switched", (g_tick_preempted >= 2u) ? 1u : 0u, 1u, SELFTEST_EQ);

    /*
     * ---- ★ M4-8.4:周期睡眠 + 全表扫描唤醒 ★ ----
     *
     * 三项分开,因为它们失败的含义完全不同:
     *   sched_sleep_wakes   睡下去之后**被唤醒过**至少两次
     *   sched_sleep_latency 唤醒延迟在界内 —— 有个纯占用线程霸着 CPU 时
     *                       它仍要小,这是"顺序"的直接测量
     *   sched_sleep_yields  状态线程睡觉期间 CPU **真的被负载线程用起来了**
     *                       (否则它是在忙等,而不是在睡)
     *
     * ⚠ 唤醒这件事**只能靠宿主测 + 这一条板级判据**:
     *   "扫描里带唤醒"是个副作用,宿主测能穷尽它的语义(见 tests 的第 11b 节),
     *   而"板上真的会醒"必须实测。
     */
    selftest_report("sched_sleep_wakes", (g_sleep_wakes_at >= 2u) ? 1u : 0u, 1u, SELFTEST_EQ);
    selftest_report("sched_sleep_latency", (g_sleep_late_max_at < 20000u) ? 1u : 0u, 1u, SELFTEST_EQ);
    selftest_report("sched_sleep_yields", g_sleep_load_ok, 1u, SELFTEST_EQ);
    selftest_report("sched_sleep_printed", (g_sleep_printed_at >= 2u) ? 1u : 0u, 1u, SELFTEST_EQ);

    /*
     * ---- ★ M4-8.5:无饥饿判据 ★ ----
     *
     * 两项分开报,因为它们的失败含义完全不同:
     *   sched_no_starvation   判据本身:没有任何"可运行却没推进"的窗口
     *   sched_no_starve_windows  **判据非空转**:有那么多个窗口里 4 个线程
     *                        都还处于可运行状态
     *
     * ⚠ 第二项不是装饰。一个"什么都没观察到"的检查永远通过 ——
     *   本项目已经不止一次因为判据太松白跑一轮上板(见计划 §0.5.5)。
     */
    selftest_report("sched_no_starvation", g_starve_ok, 1u, SELFTEST_EQ);
    selftest_report("sched_no_starve_windows", g_starve_full_at, STARVE_MIN_FULL, SELFTEST_GE);

    /*
     * ---- ★ M4-10.1:调度计数器按核分开 ★ ----
     *
     * ⚠ 这一条的强度要说清楚:它证明的是"**访问器读的就是 tick 在写的那个字段**"
     *   —— 字段搬到 `percpu_t` 之后接线没错、也没漏加。
     *
     *   它**证明不了**"两个核各写各的":那需要一个真的会在 CPU1 上跑线程的
     *   调度器,而那是 10.3/10.4 的事。按 §0.5.6b 的硬规矩,10.1 因此**不算
     *   一个阶段** —— 它的独立判据要到 10.4 才成立(那时这一行会升级成
     *   "两核的计数都 > 0 且互不相等")。
     */
    selftest_report("sched_percpu_switched",
                    ((g_percpu[0].switched > 0u) && (sched_tick_switched() == g_percpu[0].switched)) ? 1u
                                                                                                    : 0u,
                    1u, SELFTEST_EQ);

    /*
     * ---- ★ M4-10:两个核都在跑线程 ★ ----
     *
     * 五项分开报 —— 它们的失败原因完全不同:
     *   smp_sched_two_cores  合起来的判据(见相 6 的说明)
     *   smp_sched_cpu1_ran   **CPU1 上真的有探针跑完过**(核号是线程自己写的)
     *   smp_sched_spread     两个核**都**分到了线程(按 TCB 的 `cpu_id`)
     *   smp_ap_idle          CPU1 的 idle 登记好了,而且它的现场被收过
     *                        —— 也就是"切进过 CPU1 的 idle、又切走了"
     */
    selftest_report("smp_sched_two_cores", g_smp_sched_ok, 1u, SELFTEST_EQ);
    selftest_report("smp_sched_created", g_smp_made, SMP_SCHED_THREADS + SMP_APP_THREADS, SELFTEST_EQ);
    selftest_report("smp_sched_placement", (g_smp_place_bad == 0u) ? 1u : 0u, 1u, SELFTEST_EQ);
    selftest_report("smp_sched_cpu1_ran", (g_smp_ran[1] > 0u) ? 1u : 0u, 1u, SELFTEST_EQ);
    selftest_report("smp_sched_cpu0_ran", (g_smp_ran[0] > 0u) ? 1u : 0u, 1u, SELFTEST_EQ);
    /*
     * ★ 应用级线程必须落在 CPU0(哪怕 CPU1 的队列更短)★
     *
     * M4-10.5 那条规则的**可执行判据**。判据取 1 而不是 0 是有意的:
     *   `g_smp_app_cpu = 0` 才是对;写成 `== 1` 之类的反向断点会让人误读成
     *   "落到 CPU1 才算过"。(所以这里显式比较 0,并且单独报"造出来了吗"。)
     */
    selftest_report("smp_app_level_cpu0",
                    ((g_smp_app_created != 0u) && (g_smp_app_cpu == 0u)) ? 1u : 0u, 1u, SELFTEST_EQ);
    /*
     * ★ CPU1 上的浮点现场 ★ —— 这一条补的是 10.6 的证据缺口。
     *
     * `fp_ck_cpu1 > 0` 是非空转条件;`fp_bad_cpu1 == 0` 才是判据本身。
     * 两个分开报,因为它们的失败含义完全不同。
     */
    selftest_report("smp_fp_cpu1_checked", (g_smp_fp_ck_cpu1 > 0u) ? 1u : 0u, 1u, SELFTEST_EQ);
    selftest_report("smp_fp_cpu1_ok", (g_smp_fp_bad_cpu1 == 0u) ? 1u : 0u, 1u, SELFTEST_EQ);
    selftest_report("smp_fp_all_ok", (g_smp_fp_bad == 0u) ? 1u : 0u, 1u, SELFTEST_EQ);
    selftest_report("smp_ap_idle", g_smp_ap_idle_harvested, 1u, SELFTEST_EQ);
    selftest_report("smp_ap_idle_back", g_smp_ap_idle_back, 1u, SELFTEST_EQ);
    selftest_report("smp_cpu1_sched_ready", g_cpu1_sched_ready, 1u, SELFTEST_EQ);

    /*
     * ---- ★ 线程栈池的余量(给 D14 当哨兵)★ ----
     *
     * D14:线程**没有退出路径**,于是每造一个线程就永久占掉一个栈槽
     * (`KSTACK_SLOTS = 32`)。这一相之后已经用掉二十几个 —— 再加两个探针
     * 就会开始**静默地创建失败**(`sched_kthread_create` 返回 NULL),
     * 而"创建失败"在各处的表现是"那个自检项看起来没跑"。
     *
     * 判据留 4 个槽的余量:不是"越多越好",而是"还够下一次改动"。
     * 它红了就说明该做 M4-11(线程退出)或者扩池了 —— 两条都写在待办里。
     */
    selftest_report("kstack_headroom",
                    (g_kstack.slot_count > g_kstack.peak_used &&
                     (g_kstack.slot_count - g_kstack.peak_used) >= 4u)
                        ? 1u
                        : 0u,
                    1u, SELFTEST_EQ);

    /*
     * ---- ★ M4-11.1:排他输出用的那把锁(D13 结案)★ ----
     *
     * 三条判据 + 两条绊线,各自失败的含义完全不同:
     *
     *   `console_excl_wait`      这把锁**真的被竞争过**吗(让出次数 > 0)。
     *                            恒为 0 的话,"互斥成立"就只是一句空话 ——
     *                            一把没人争的锁,谁都能"通过"。
     *                            ⚠ 适用条件:第二个写者(周期状态线程)存在。
     *                            它没造出来时这条判据不成立,所以一起判。
     *   `console_excl_sched_off` 报告区间里"调度被人为关掉"的毫秒数。
     *                            旧做法(关调度)等于整个报告长度(几秒);
     *                            现在应当 ≈ 0。阈值 50ms 区分的是"几毫秒"与
     *                            "几千毫秒",不是一个需要精调的边界。
     *   `console_excl_alive`     报告区间里状态线程**醒过几次**。旧做法恒为 0:
     *                            那几秒整个系统是停摆的。这一条与上一条一起,
     *                            就是"换成锁之后系统没有停摆"的直接证据。
     */
    selftest_report("console_excl_wait",
                    ((g_status_created != 0u) && (console_mutex_yields() > 0u)) ? 1u : 0u, 1u,
                    SELFTEST_EQ);
    selftest_report("console_excl_sched_off",
                    (u32)((sched_off_total_ns() - g_report_off0) / 1000000ull), 50u, SELFTEST_LE);
    selftest_report("console_excl_alive",
                    ((g_status_created != 0u) && ((g_status_wakes - g_report_wakes0) >= 1u)) ? 1u : 0u,
                    1u, SELFTEST_EQ);
    /*
     * 两条绊线(都必须恒为 0):
     *   `console_mutex_off_wait`  "在关调度的窗口里等锁" —— 那时让出与睡眠都
     *                            不发生,同核上的持锁者拿不到 CPU ⇒ 死锁且不报错。
     *   `console_mutex_errors`    状态机返回非 0 —— 生产路径上不可能发生
     *                            (递归锁 + console.c 的深度计数挡住嵌套)。
     */
    selftest_report("console_mutex_off_wait", console_mutex_off_yields(), 0u, SELFTEST_EQ);
    selftest_report("console_mutex_errors",
                    console_mutex_lock_errors() + console_mutex_unlock_errors(), 0u, SELFTEST_EQ);

    /*
     * `invalid_ctx` 必须恒为 0:它不是"发生过多少件坏事"的计数,
     * 而是"调度器有没有挑到过不可切换的上下文"。挑了就是有 bug ——
     * 正常路径上 idle 的 pc==0 只在注册与第一次切走之间成立,
     * 而那个窗口里 current 就是 idle 自己(第 2 步直接返回,轮不到第 3 步)。
     */
    selftest_report("sched_preempt_invalid_ctx", g_tick_invalid_ctx, 0u, SELFTEST_EQ);

    selftest_report("switch_ctx_a", g_sw_ctx_a_ok, 1u, SELFTEST_EQ);
    selftest_report("switch_ctx_b", g_sw_ctx_b_ok, 1u, SELFTEST_EQ);
    selftest_report("switch_stack_isolated", g_sw_stack_ok, 1u, SELFTEST_EQ);

    selftest_report("smp_cpu1_online", g_percpu[1].online, 1u, SELFTEST_EQ);
    selftest_report("smp_cpu1_stage", HB[HB_SLOT_CPU1_STAGE], HB_CPU1_STAGE_ONLINE, SELFTEST_EQ);

    /*
     * 每核 1kHz tick(AM3-5)。
     *
     * 判据是"两核各自在推进",而不是"两核计数相等" ——
     * 相等的判据会在 CPU1 的 tick 恰好停下时也成立(两边都冻住)。
     * 这里发两次采样、要求 CPU1 的计数确实增长了。
     */
    {
        u32 t0 = g_percpu[1].ticks;

        timer_delay_us(20000u); /* 20ms -> 1kHz 下应当涨约 20 */

        selftest_report("smp_cpu1_ticks_advance", (g_percpu[1].ticks > t0) ? 1u : 0u, 1u, SELFTEST_EQ);
    }

    /*
     * IPI 判据是 sent == seen,不是 ">= 某个数"。
     * 每个 IPI 都是等目标核处理完才发下一个,所以漏掉任何一个都是真问题。
     */
    selftest_report("smp_ipi_received", g_smp_stress.ipi_seen, g_smp_stress.ipi_sent, SELFTEST_EQ);
    selftest_report("smp_lock_violations", g_smp_stress.violations, 0u, SELFTEST_EQ);

    /*
     * 计数必须正好是两倍轮数。
     * 少了说明丢了更新(锁没起作用),多了说明有核重复计数 ——
     * 两种都是真问题,所以用等号而不是"大于等于"。
     */
    selftest_report("smp_lock_counter", g_smp_stress.counter, 2u * SMP_STRESS_ROUNDS, SELFTEST_EQ);
    HB[HB_SLOT_CPU1_TICKS]    = g_percpu[1].ticks;
    HB[HB_SLOT_IPI_COUNT]     = g_percpu[1].ipi_count;
    HB[HB_SLOT_SMP_VIOLATION] = g_smp_stress.violations;
    /*
     * ⚠ 这里查 MPIDR,不查 cpu_id。
     *
     * 破坏性 A/B 抓出来的:cpu_id 由 CPU0 在放 CPU1 起来**之前**就填好了,
     * 所以即使 CPU1 根本没被唤醒它也是 1 —— 这一项永远不会失败,
     * 等于没判。MPIDR 只有 CPU1 自己能写进去,才是它真的跑过的证据。
     *
     * 0x80000001:Cortex-A9 的 MPIDR,bit31=0b1 表示多核系统,
     * bits[1:0] = 核号,所以 CPU1 就是 0x80000001。
     */
    selftest_report("smp_cpu1_mpidr", g_percpu[1].mpidr, 0x80000001u, SELFTEST_EQ);

    /*
     * loops >= 1 而不是 "> 0 就通过":判据写成 1/0 是为了让它和别的项
     * 一样是等值判定,避免"永远成立"的阈值(那种判据等于没判)。
     */
    selftest_report("smp_cpu1_loops", (g_percpu[1].loops >= 1u) ? 1u : 0u, 1u, SELFTEST_EQ);

    /*
     * 两个核的栈区必须不同。
     *
     * 这一项防的是"链接脚本改错了但两个核都能跑"的情况 ——
     * 共享栈区不会立刻崩,只会在负载上来之后随机踩栈,
     * 那时再回头怀疑到栈上要花很久。
     */
    selftest_report("smp_distinct_stacks",
                    (g_percpu[0].stack_top != 0u && g_percpu[1].stack_top != 0u &&
                     g_percpu[0].stack_top != g_percpu[1].stack_top)
                        ? 1u
                        : 0u,
                    1u, SELFTEST_EQ);

    HB[HB_SLOT_SELFTEST_FAILED] = selftest_summary();
    console_puts("\n");
    console_excl_end(); /* ★ 报告区间结束,状态行线程可以继续说话了 ★ */

    /* ---- 9.74 串口排他锁的破坏性对照组(必须在自检报告**之后**)---- */
    /*
     * ## 对照的是什么
     *
     * 两段**完全一样**的负载:拿住排他、等 1200ms 墙钟、放开。
     * 唯一变的是"拿住"这件事的实现:
     *
     *   A(生产路径)  源 OS 的 yield 型互斥(`console_mutex_set_legacy(0)`)
     *   B(对照组)    退回 M4-11.1 之前的"关调度"(`console_mutex_set_legacy(1)`)
     *
     * 于是同一件事有两个**可测的**侧面:
     *
     *   `sched_off`  窗口内"调度被人为关掉"的毫秒数
     *                A:≈ 0(锁不关调度)      B:≈ 窗口长度(1200)
     *   `alive`      窗口内"系统在跑"的证据 = 饥饿监视器**完成的窗口数**
     *                + 状态线程**醒过**的次数 + 它**等锁重试**过的次数
     *                A:≥ 1(实测十几)         B:== 0(整个系统停摆)
     *
     * ⚠ `alive` 为什么是**三个计数相加**,而不是只看状态线程:
     *   实测踩到过两次(第二、三次上板)——
     *     ① 报告期间状态线程就已经卡在"等锁重试"里了,它的"醒来"计数在
     *        **进入重试循环之前**就加过了 ⇒ A 窗口里那个数增量为 0;
     *     ② 就算补上"重试次数",A 窗口是 1.2s 而状态线程的周期是 1s ⇒
     *        **窗口边界正好错过一次唤醒**时它仍然是 0(第三次上板就是这样)。
     *   ⇒ 判据不能押在"另一个线程的相位恰好落进窗口"上。饥饿监视器的周期是
     *     **64ms**(前一个窗口的三十分之一),它在窗口里完成多少次是**确定**的:
     *     A 里十几次,B 里 0 次(调度关着,它一次都跑不起来)。
     *     状态线程那两个计数留着,是因为它们在 A 里额外证明"打印确实被竞争过"。
     *
     * ⚠ 判据是**两个侧面都对上**才算"检出" —— 只看 `sched_off` 的话,
     *   "窗口里其实什么都没跑"也会让 A 看起来很好。
     *
     * ⚠ 基准(三个计数)在 B 里必须**关掉调度之后**再取:那一刻起没有别人
     *   能跑,读到的就是确定值(否则那一瞬间刚好有一次唤醒/一个窗口结束时,
     *   `alive == 0` 就会假失败)。A 里没有这个讲究 —— A 要的正是"它照常在跑"。
     *
     * ⚠ `console_mutex_set_legacy()` 只在"没有窗口开着"时才接受切换
     *   (锁是空的、调度是开的),被拒会计数;这里的两处切换都在窗口之间,
     *   所以 `refused` 应当是 0 —— 一起打出来,免得"切换其实没生效"被读成
     *   "两组一样"。
     */
    {
        u64 off_a0;
        u64 off_a1;
        u64 off_b0;
        u64 off_b1;
        u32 w_a0;
        u32 w_a1;
        u32 w_b0;
        u32 w_b1;
        u32 y_a0;
        u32 y_a1;
        u32 y_b0;
        u32 y_b1;
        u32 c_a0;
        u32 c_a1;
        u32 c_b0;
        u32 c_b1;
        u32 off_a_ms;
        u32 off_b_ms;
        u32 win_a;
        u32 wake_a;
        u32 wait_a;
        u32 win_b;
        u32 wake_b;
        u32 wait_b;
        u32 refused0 = console_mutex_legacy_refused();
        bool detected;

        /* ---- A:生产路径(锁)---- */
        console_mutex_set_legacy(0u);
        off_a0 = sched_off_total_ns();
        w_a0   = g_status_wakes;
        y_a0   = console_mutex_yields();
        c_a0   = g_starve_checks;
        console_excl_begin();
        wait_ms_wall(1200u);
        console_excl_end();
        off_a1 = sched_off_total_ns();
        w_a1   = g_status_wakes;
        y_a1   = console_mutex_yields();
        c_a1   = g_starve_checks;

        /* ---- B:对照组(关调度)---- */
        console_mutex_set_legacy(1u);
        console_excl_begin(); /* ← 旧做法:这一句就是 sched_disable() */
        off_b0 = sched_off_total_ns();
        w_b0   = g_status_wakes; /* 三个基准都在关掉之后取 —— 见上面那条说明 */
        y_b0   = console_mutex_yields();
        c_b0   = g_starve_checks;
        wait_ms_wall(1200u);
        off_b1 = sched_off_total_ns();
        w_b1   = g_status_wakes;
        y_b1   = console_mutex_yields();
        c_b1   = g_starve_checks;
        console_excl_end();
        console_mutex_set_legacy(0u);

        off_a_ms = (u32)((off_a1 - off_a0) / 1000000ull);
        off_b_ms = (u32)((off_b1 - off_b0) / 1000000ull);
        win_a    = c_a1 - c_a0;
        wake_a   = w_a1 - w_a0;
        wait_a   = y_a1 - y_a0;
        win_b    = c_b1 - c_b0;
        wake_b   = w_b1 - w_b0;
        wait_b   = y_b1 - y_b0;

        detected = ((off_a_ms <= 50u) && (off_b_ms >= 1000u) &&
                    ((win_a + wake_a + wait_a) >= 1u) && ((win_b + wake_b + wait_b) == 0u) &&
                    (console_mutex_legacy_refused() == refused0));

        /*
         * 三行判定**整段排他**:它们会被日志与人逐行读,而状态行正好在
         * 这个时刻最活跃(它刚从报告那把锁上被放出来)。不排他的话,
         * 实测会把状态行劈进 `alive=+...` 中间。
         */
        console_excl_begin();
        console_printf(" Console A/B : lock(1200ms)   -> sched_off=+%u ms alive=+%u (win=+%u wake=+%u wait=+%u)\n",
                       off_a_ms, win_a + wake_a + wait_a, win_a, wake_a, wait_a);
        console_printf(" Console A/B : legacy(1200ms) -> sched_off=+%u ms alive=+%u (win=+%u wake=+%u wait=+%u)\n",
                       off_b_ms, win_b + wake_b + wait_b, win_b, wake_b, wait_b);
        console_printf(" Console A/B : yield-lock vs disable-lock -> %s\n",
                       detected ? "DETECTED" : "NOT DETECTED");
        console_excl_end();
    }

    /* ---- 9.75 之前的共同前提:★ 后面的对照组全部钉在 CPU0 ★ ---- */
    /*
     * 报告之后的每一组对照(9.75 搬帧 / 9.76 VFP / 9.77 扫描唤醒 / 9.78 无饥饿 /
     * 9.79 选核)都是**单核判据** —— 它们量的是"同一个核上的两三个线程之间
     * 发生了什么"。线程一旦被分到两个核上,这些判据**全部失去区分能力**:
     *
     *   - 搬帧  :两个线程不共享 CPU ⇒ 谁都不需要搬帧
     *   - VFP   :两个核各有自己的 d0-d31 ⇒ "被对方改掉"根本不会发生
     *   - 唤醒  :探测线程睡在另一个核上 ⇒ "不醒"与"这台机器没在跑"分不开
     *   - 无饥饿:K 个线程被拆开 ⇒ 不再是同一队列上的轮转
     *
     * ⚠ 这不是理论担忧:**上板实测过** —— M4-10 的相 6 把选核打开之后,
     *   VFP 对照组当场从 `bad=(7914,0)` 变成 `bad=(0,0)`,报告"未检出"。
     *   那一刻看起来像"VFP 保存/恢复坏了",实际是**判据的适用条件没了**。
     *   ⇒ 教训与 §0.5.6b 那条一样:**每个判据都要写清它的适用条件**,
     *     而"单核"就是这几条的适用条件。
     *
     * 所以这里显式钉回 CPU0;9.79 自己会再置 1 并(在最后)恢复成生产值 0。
     */
    sched_set_pick_cpu0(1u);

    /* ---- 9.75 抢占的破坏性对照组(必须在自检报告**之后**)---- */
    /*
     * 为什么必须在报告之后:对照组会**故意把调度状态搅歪**
     * (决策照做、执行流不动 ⇒ `current_task` 与真正在跑的上下文不一致),
     * 放进报告之前会让别的自检项跟着一起歪 ——
     * 那三条 FAIL 会看起来像"别的地方也坏了",而实际只有一个原因。
     *
     * ## 对照的是什么
     *
     * 同一段抢占代码、同一对探针线程、同一段时间预算,**只把最后一步
     * "把新帧交出去"关掉**(`g_reloc_skip = 1`)。于是:
     *
     *   搬帧开:两个线程都跑起来、互相见证、各自跑满 20ms  → 计数非 0
     *   搬帧关:决策做了一堆(current_task 都在两个线程之间轮转了好几遍),
     *           而**执行流一步没动** → 两个计数恒为 0
     *
     *   ⇒ 这两个数的差别,就是"搬帧"这件事的承重性。
     *
     * ⚠ 判据是"两者**都为 0**"(检出),不是"有一个非 0"。
     *   像 M4-7 那个"不换栈"的对照组一样,这里是**预期它坏**。
     */
    {
        tcb_t ca;
        tcb_t cb;

        spin_reset(g_spin_ctl);

        sched_disable();
        ca = sched_kthread_create(spin_ctl_a, NULL, "ca");
        cb = sched_kthread_create(spin_ctl_b, NULL, "cb");

        if (ca == NULL || cb == NULL) {
            sched_enable();
            console_puts(" Preempt A/B : kthread_create FAILED\n");
        } else {
            arm_task_ctx_t ctx_snapshot[SCHED_CTX_SNAPSHOT_MAX];
            u32            ctx_snapshot_n;
            u32            sw_before;
            u32            inv_before;
            u32            inv_after;
            arm_task_ctx_t idle_ctx_backup;

            /*
             * ★ 先把 idle 的现场**抄一份**再进对照组 ★
             *
             * 对照组期间 `current_task` 会在两个 ctl 线程之间轮转,而
             * `sched_tick` 每次都把**发指令那一刻 kmain 的现场**收进
             * "当时的 current" —— 其中就包括 idle。等对照组结束,
             * `idle->ctx` 里装的是**窗口中间某一刻**的快照:sp 指向
             * kmain 栈上某个已经不成立的调用深度。
             *
             * 现在不会用到它(队列空 + current==idle ⇒ 直接早退),
             * 但那是"碰巧安全",不是"设计安全":将来任何一相只要造一个
             * 会挂起的线程,idle 就会被重新挑中,`rfeia` 会拿着那个陈旧
             * 快照从 `wait_ms_wall` 的忙等中间把 kmain 重新跑起来 ——
             * 而它的栈已经退掉了。所以老老实实备份再还原。
             */
            idle_ctx_backup = sched_boot_idle()->ctx;
            /*
             * ★ 把队列里每个线程的 ctx 也抄一份 ★
             *
             * 对照组会把"发指令那一刻真正在跑的现场"(也就是 kmain 的)
             * 写进当时的 current —— 对 ca/cb 那是目的,但对**别的常驻线程**
             * (M4-8.4 的周期状态线程)就是污染:它的 ctx.sp 会指到 kmain 的
             * 启动栈,从此**再也切不进去**(`sched_ctx_switchable` 一直拒绝它)。
             *
             * 上板实测过一次,症状很绕:报告之后状态行再也不出声,
             * `invalid_ctx` 涨到 85139,而 A/B 自己的两个线程 done=0 ——
             * 看起来像"判据不承重",实际是对照组把别人弄坏了。
             */
            ctx_snapshot_n = sched_ctx_snapshot_all(ctx_snapshot, SCHED_CTX_SNAPSHOT_MAX);

            sw_before  = sched_tick_switched();
            /*
             * ⚠ 两个计数都要**在窗口前后各取一次**再打差值,不能只打一个
             *   末值 —— 理由见下面"慢串口"那段。
             */
            inv_before = sched_tick_invalid_ctx();

            g_reloc_skip = 1u; /* ★ 对照组:决策照做,不交出目标帧 ★ */
            sched_enable();
            wait_ms_wall(SPIN_BUDGET_US / 1000u + 200u);

            inv_after = sched_tick_invalid_ctx();
            g_reloc_skip = 0u;

            g_reloc_ctl_a = g_spin_ctl[0].count;
            g_reloc_ctl_b = g_spin_ctl[1].count;

            /*
             * 检出条件有两个,都要成立:
             *   1. 两个探针计数都是 0(线程一次都没跑起来);
             *   2. 这期间**确实做了切换决策**(否则"没跑起来"只是因为
             *      压根没人被挑中,那就什么也证明不了)。
             *
             * ⚠ 这一相里**完整的切换只有头两次**,判据里那个 `>` 比较的就是
             *   "这期间到底有没有真的搬过帧"。
             *
             * ★ `invalid_ctx` 会涨 —— 而且**它应该涨**(曾经的"时序敏感点",
             *   现在能算了)★
             *
             *   M4-8.4 的留痕写的是"invalid 一次都没涨",那是当时的事实;
             *   M4-9.5 之后同样的对照里它涨到 **53**。差别在于:
             *
             *     `g_reloc_skip = 1` 只让"交出目标帧"这一步不发生,
             *     **收现场那一步照做** —— 于是 ca/cb 的 ctx 每轮都被换成
             *     kmain 的现场(sp 指向启动栈,而它们自己的栈在栈池里)。
             *     下一发 tick 照样会挑中它们(它们从没跑过 ⇒ vruntime 极小
             *     ⇒ deadline 最小),而 `sched_ctx_switchable()` 要求
             *     "sp 必须落在自己的栈区里" ⇒ **拒绝** ⇒ `invalid_ctx++`。
             *
             *   ⇒ 它涨到多少是**可算的**:窗口里每 `TIME_SLICE` 个 tick 做一次
             *     派发决策,所以上限 = 窗口毫秒 / (TIME_SLICE × tick 毫秒)。
             *     实测 53、上限 59 ⇒ 几乎每一次派发都挑中了那两个被污染的
             *     线程,与推演一致。
             *
             *   ★ 这条上界还守住一个**历史故障**:曾经 `invalid_ctx` 涨到
             *     **85139**(对照组把常驻线程的 ctx 弄脏,它被永远拒绝),
             *     那种数量级会被这条判据当场抓住 —— 而当时它是靠"状态行
             *     不再出声"这种间接现象才被发现的。
             *
             * ⚠★ 慢串口效应:**打印本身要花掉几十毫秒,而那期间系统还在跑。**
             *   9600 波特下一行 60 多个字符要传 60~80ms,约等于几十到上百发 tick。
             *   所以"先打印一个计数、之后再从 JTAG 读同一个计数"必然对不上 ——
             *   差的不是误差,是**打印期间真的又发生了事情**。
             *   办法就是窗口前后各取一次、打差值(下面收拾挪到打印之前,
             *   也是同一个道理:别让"半复位状态"横跨慢 I/O)。
             */
            g_reloc_ctl_inv_delta = inv_after - inv_before;
            {
                u32 window_ms = SPIN_BUDGET_US / 1000u + 200u;
                u32 slice_ms  = (u32)((SCHED_TIME_SLICE * SCHED_TICK_NS) / 1000000ull);
                u32 bound     = (window_ms / ((slice_ms == 0u) ? 1u : slice_ms)) + 4u;

                g_reloc_ctl_inv_ok = (g_reloc_ctl_inv_delta <= bound) ? 1u : 0u;
            }
            g_reloc_ctl_detected =
                ((g_reloc_ctl_a == 0u) && (g_reloc_ctl_b == 0u) &&
                 (sched_tick_switched() > sw_before))
                    ? 1u
                    : 0u;

            /*
             * ---- 收拾:对照组把状态搅歪了,必须复位 ----
             *
             * ⚠ **收拾必须在任何打印之前。**
             *
             * 9600 波特下一行要传 60~80ms,而这期间 tick 一直在跑。窗口结束时
             * `current_task` 还指着 ctl 线程、就绪队列里还挂着另一个 ——
             * 于是每一发 tick 都走进"决策 → 目标上下文不可切换 → 拒绝"，
             * `invalid` 在**打印过程中**一路涨。
             *
             * 实测:窗口前后各取一次是 `invalid=0->0`,而同一轮结束后从 JTAG
             * 读同一个计数是 **78** —— 差的全部来自打印与收尾之间那几百毫秒。
             * 这既是"先打印再复位"的直接后果，也说明**慢串口会让'打印之后再读'
             * 这种取数方式必然对不上**。
             *
             * 另外两个 ctl 线程**从来没跑过**,它们的 ctx 在对照组期间被收进去的
             * 是 kmain 那一刻的现场(那发 IRQ 的帧在 kmain 的启动栈上,
             * 落在它们自己那块栈区之外) —— 那是垃圾,只能挂起,不能留着。
             */
            ca->status      = WAIT;
            ca->wakeup_time = 0u;
            cb->status      = WAIT;
            cb->wakeup_time = 0u;
            g_reloc_skip    = 0u;

            sched_set_current(sched_boot_idle());
            sched_boot_idle()->status = RUNNING;
            sched_boot_idle()->ctx    = idle_ctx_backup; /* ★ 见上面那段说明 ★ */
            sched_ctx_restore_all(ctx_snapshot, ctx_snapshot_n); /* ★ 别人也不能被污染 ★ */
            /*
             * ⚠ **不要**在这里调 `sched_kern_init()`。
             *
             * 它会 `sched_queue_init()` —— 把整个队列清空。而 M4-8.4 之后
             * **周期状态线程是常驻的、就挂在队列里**,清一次队列就把它从
             * 名册上抹掉了:它再也不会被挑中,状态行从此消失。
             *
             * 而且**根本不需要清**:M4-8.4 之后队列是"全部线程的名册",
             * 睡眠/挂起的任务本来就留在里面、靠 `sched_task_schedulable()`
             * 排除。把 ctl 线程置成 WAIT 已经够了 —— 它们既不会被选中,
             * 也不碍着别人。M4-9 那一版需要清,是因为那时"跑着的不在队列里",
             * 切换路径在维护成员关系、可能留下半拉状态。
             */

            console_excl_begin();
            console_printf(" Preempt A/B : skip_frame -> a=%u b=%u switched=%u invalid=%u->%u\n",
                           g_reloc_ctl_a, g_reloc_ctl_b, sched_tick_switched() - sw_before,
                           inv_before, inv_after);
            console_printf(" Preempt A/B : 未搬帧 -> %s(这就是「决策说切了、执行流没动」的样子)\n",
                           g_reloc_ctl_detected ? "检出" : "未检出 —— 判据不承重!");
            /*
             * ★ `invalid` 的增长量:从"没定位的敏感点"变成"可算的上界" ★
             *
             * 窗口 220ms ÷ 片长 4ms ≈ 55 次派发决策 ⇒ 上界 59。
             * 实测 53 ⇒ 几乎每一次派发都挑中了那两个被污染的线程。
             * 它同时是"污染有没有**失控**"的哨兵:曾经的 85139 会被它抓住。
             *
             * ⚠ 它的结论**只能在这里打**(不能进自检报告):
             *   报告在 9.75 **之前**就打完了(对照组会故意把调度状态搅歪,
             *   放进报告区间会让别的项跟着歪 —— 见 9.75 开头那段)。
             *   所以这条与另外五组 A/B 一样,以普通输出给出。
             *   (第一版我把它写成了报告项,于是它读到的永远是初值 0 ⇒ 一条
             *    假 FAIL —— 而"报告在对照组之前"这件事本来就是这个项目的
             *    既有约定。)
             */
            console_printf(" Preempt A/B : invalid bound: delta=%u <= %u -> %s\n",
                           g_reloc_ctl_inv_delta,
                           (SPIN_BUDGET_US / 1000u + 200u) /
                                   (u32)((SCHED_TIME_SLICE * SCHED_TICK_NS) / 1000000ull) +
                               4u,
                           g_reloc_ctl_inv_ok ? "PASS" : "FAIL(涨得比派发次数还多!)");
            console_excl_end();
            /*
             * ⚠ 一个曾经留痕、**现在已定位**的观察(留作记录):
             *
             *   早先几轮里这一行打出过 `invalid=0->0` —— 窗口内 `switched`
             *   涨了 2 而 `invalid` 一次都没涨;M4-9.5 之后同样的对照变成
             *   `0->53`。当时记的是"时序对代码改动敏感,敏感点还没定位"。
             *
             *   → 现在能算清楚了(见上面那段推导):`invalid` 的次数 =
             *     窗口内"挑中了 ctx 已被污染的线程"的派发次数,其**上界**由
             *     窗口长度 ÷ 片长决定(220ms ÷ 4ms ≈ 55,判据取 59)。
             *     53 与 55 的差就是最前面那两次:那时 ca/cb 的 ctx 还是
             *     它们**自己**的(刚造出来的合法上下文),于是 `switched` 涨 2
             *     而没有被拒绝。**两个数现在是同一条账上的。**
             *
             * ⚠ 无论取哪个值,检出判据都不受影响:它是"两个探针计数**都是 0**"
             *   且"这段时间**确实搬过帧**",两件事都由实测值直接支撑。
             */
        }
    }

    /* ---- 9.76 浮点现场的破坏性对照组(同样必须在报告**之后**)---- */
    /*
     * 同一段切换代码、同一对浮点探针,**只把"存/取浮点现场"关掉**
     * (`g_vfp_skip = 1`,那两条汇编里的 `bl` 变成空操作)。
     *
     *   存/取开:两个线程的 d0-d31 与 FPSCR 各自保持  → bad = 0
     *   存/取关:其中一个会读到对方的图案            → bad > 0
     *
     * ⚠ 判据是"**检出**"(bad > 0),不是"没有错"。与 9.7 的"不换栈"
     *   和 9.75 的"不搬帧"一样:这里是**预期它坏**。
     *
     * ⚠ 必须放在报告之后:这一相同样会把 `current_task` 与 `idle->ctx`
     *   搅歪(线程真的跑起来了,而且 `g_reloc_skip` 是关的)。
     */
    {
        tcb_t ca;
        tcb_t cb;

        fp_reset(g_fp_ctl);

        sched_disable();
        ca = sched_kthread_create(fp_ctl_a, NULL, "fca");
        cb = sched_kthread_create(fp_ctl_b, NULL, "fcb");

        if (ca == NULL || cb == NULL) {
            sched_enable();
            console_puts(" VFP A/B     : kthread_create FAILED\n");
        } else {
            arm_task_ctx_t idle_ctx_backup = sched_boot_idle()->ctx;
            arm_task_ctx_t ctx_snapshot[SCHED_CTX_SNAPSHOT_MAX];
            u32            ctx_snapshot_n;

            ctx_snapshot_n = sched_ctx_snapshot_all(ctx_snapshot, SCHED_CTX_SNAPSHOT_MAX);

            g_vfp_skip = 1u; /* ★ 对照组:不换浮点现场 ★ */
            sched_enable();
            wait_ms_wall(FP_BUDGET_US / 1000u + 300u);
            g_vfp_skip = 0u;

            console_excl_begin();
            console_printf(" VFP A/B     : bad=(%u,%u) rmode_bad=(%u,%u) done=%u\n", g_fp_ctl[0].bad,
                           g_fp_ctl[1].bad, g_fp_ctl[0].rmode_bad, g_fp_ctl[1].rmode_bad,
                           (g_fp_ctl[0].done && g_fp_ctl[1].done) ? 1u : 0u);

            /*
             * ⚠ 检出条件是"**至少一个**线程发现自己的寄存器被换了",
             *   不是两个都发现 —— 理由见 fp_probe_t 上面那段说明:
             *   只有"被换下又换回来"的那一个读得到对方的图案;
             *   对方只是"自己的图案还在",自然干净。
             *
             * ⚠ 还要确认两个线程**都真的跑过**(done):否则"没检出"
             *   可能只是因为它们压根没被调度进来。
             */
            g_vfp_ctl_bad      = g_fp_ctl[0].bad + g_fp_ctl[1].bad +
                                 g_fp_ctl[0].rmode_bad + g_fp_ctl[1].rmode_bad;
            g_vfp_ctl_detected = ((g_vfp_ctl_bad > 0u) && g_fp_ctl[0].done && g_fp_ctl[1].done)
                                     ? 1u
                                     : 0u;

            console_printf(" VFP A/B     : 未换浮点现场 -> %s(这就是「寄存器被对方改掉」的样子)\n",
                           g_vfp_ctl_detected ? "检出" : "未检出 —— 判据不承重!");
            console_excl_end();

            /* 收拾:与 9.75 同一套(挂起两个线程、复位 current / 队列 / idle 现场)*/
            ca->status      = WAIT;
            ca->wakeup_time = 0u;
            cb->status      = WAIT;
            cb->wakeup_time = 0u;
            g_vfp_skip      = 0u;

            sched_set_current(sched_boot_idle());
            sched_boot_idle()->status = RUNNING;
            sched_boot_idle()->ctx    = idle_ctx_backup;
            sched_ctx_restore_all(ctx_snapshot, ctx_snapshot_n);
            /* ⚠ 不调 sched_kern_init():见上面 9.75 那段说明 —— 它会清空队列、
             *   把常驻的周期状态线程一起抹掉。 */
        }
    }

    /* ---- 9.77 扫描唤醒的破坏性对照组(同样必须在报告**之后**)---- */
    /*
     * 同一段选取代码、同一个周期睡眠线程,**只把扫描里的那次唤醒关掉**
     * (`g_wake_skip = 1`,也就是 M4-8 "取队首"那一版的等效行为)。
     *
     *   唤醒开:线程每秒醒一次 ⇒ `wakes` 每秒钟涨
     *   唤醒关:线程睡下去就**再也醒不过来** ⇒ `wakes` 冻住不动
     *
     * ⚠ 判据是"**窗口内一次都没醒**"(检出),不是"醒得少了"。
     *   与前两个对照组一样,这里是**预期它坏**。
     *
     * ⚠ 这一相**不要**放纯占用线程:
     *   ① 对照组里睡着的线程永远不会醒,而负载线程不会主动让出 ⇒
     *      idle(也就是 kmain 自己)再也拿不到 CPU,**这一相就没法收尾**;
     *   ② 不设负载时,"没醒"唯一的原因就是那次唤醒被去掉了,
     *      归因干净。
     */
    {
        tcb_t sc;
        u32   before = g_status_ctl_wakes;

        sched_disable();
        sc = sched_kthread_create(status_thread_ctl, NULL, "wctl");
        if (sc == NULL) {
            sched_enable();
            console_puts(" Wake A/B    : kthread_create FAILED\n");
        } else {
            arm_task_ctx_t idle_ctx_backup = sched_boot_idle()->ctx;
            arm_task_ctx_t ctx_snapshot[SCHED_CTX_SNAPSHOT_MAX];
            u32            ctx_snapshot_n;

            ctx_snapshot_n = sched_ctx_snapshot_all(ctx_snapshot, SCHED_CTX_SNAPSHOT_MAX);

            g_wake_skip = 1u; /* ★ 对照组:扫描里不再唤醒 ★ */
            sched_enable();
            wait_ms_wall(1500u);
            g_wake_skip = 0u;

            g_wake_ctl_detected = (g_status_ctl_wakes == before) ? 1u : 0u;

            console_excl_begin();
            console_printf(" Wake A/B    : ctl_wakes %u -> %u over 1.5 s (line=%u)\n", before,
                           g_status_ctl_wakes, g_status_ctl_printed);
            console_printf(" Wake A/B    : 不唤醒 -> %s(这就是「睡下去就醒不过来」的样子)\n",
                           g_wake_ctl_detected ? "检出" : "未检出 —— 判据不承重!");
            console_excl_end();

            /* 收拾:与 9.75 / 9.76 同一套 */
            sc->status      = WAIT;
            sc->wakeup_time = 0u;
            g_wake_skip     = 0u;

            sched_set_current(sched_boot_idle());
            sched_boot_idle()->status = RUNNING;
            sched_boot_idle()->ctx    = idle_ctx_backup;
            sched_ctx_restore_all(ctx_snapshot, ctx_snapshot_n);
            /* ⚠ 不调 sched_kern_init():见上面 9.75 那段说明 —— 它会清空队列、
             *   把常驻的周期状态线程一起抹掉。 */
        }
    }

    /* ---- 9.78 ★ M4-8.5 的破坏性对照组:严格非抢占 ⇒ 必然饿死 ★ ---- */
    /*
     * 同一段选取代码、同一批负载、**同一个监视器**,只把"选谁"改成
     * "current 可运行就还是它"(`g_pick_sticky`,粘够 n 次之后自动恢复)。
     *
     *   粘住关(相 5 实测):4 个线程轮转,每个窗口每个都推进 ⇒ events=0 late=0
     *   粘住开           :一个线程一路跑到底,**其余 3 个一个窗口都没轮到**;
     *                      而且连监视器自己都醒不过来 ⇒ 它的窗口长度从
     *                      ~64 tick 变成上千 tick
     *
     * ⚠ 判据是"**监视器报警了**(events 或 late 有增量)**而且**受害者真的
     *   没推进(adv < K)"。两个条件都要:只有报警可能是误报,只有"没推进"
     *   可能只是因为负载压根没起来。与前四个对照组一样,这里是**预期它坏**。
     *
     * ⚠ 为什么粘住必须是**有限次数**:粘住之后没有任何人能把它清零
     *   (kmain 与监视器都轮不到 CPU),对照组会就地挂死 ——
     *   那不是"判据不承重",是**测不了**。见 src/sched.c 的说明。
     *
     * ⚠ 负载的墙钟预算(600ms)**故意短于**粘住的时长(~1.2s):
     *   于是粘住一结束,它们一被选中就立刻到期挂起,计数停在 0 ——
     *   "谁推进过、谁没推进过"在读数上是一目了然的。
     */
    {
        tcb_t sa[STARVE_K];
        u32   i;

        sched_disable();
        starve_reset(STARVE_CTL_BUDGET);
        /*
         * ★ 粘住的那一个(sa[0])必须**活过整段粘住**,其余三个必须**活不过** ★
         *
         *   活过:否则它自己到期挂起,把粘住提前结束 —— 粘住的次数就用不完,
         *         而且监视器会在"粘住还在生效"的时候拿到 CPU(上板实测过:
         *         粘住 300 次只消耗 121 次,而监视器仍然报了警 —— 结论对,
         *         但**时序不是我设计的那个**,读数要靠猜)。
         *   活不过:它们一被选中就立刻到期挂起,**计数停在 0** ——
         *         "谁被饿着"于是留在读数上,而不是"饿完接着跑满自己那份"。
         */
        g_starve[0].budget_us = STARVE_CTL_HOG;

        sa[0] = sched_kthread_create(starve_w0, NULL, "sa0");
        sa[1] = sched_kthread_create(starve_w1, NULL, "sa1");
        sa[2] = sched_kthread_create(starve_w2, NULL, "sa2");
        sa[3] = sched_kthread_create(starve_w3, NULL, "sa3");
        starve_arm(sa, STARVE_K); /* 统计量从这里清零,所以下面读到的就是增量 */

        g_pick_sticky = STARVE_STICKY_N; /* ★ 对照组:不许换人 ★ */
        sched_enable();

        if (!starve_created(sa)) {
            g_pick_sticky = 0u;
            console_puts(" No-starve A/B: kthread_create FAILED\n");
        } else {
            /*
             * ★ 打印用的数**必须在收拾之前全部抄出来** ★
             *
             * `starve_arm(NULL, 0)` 会把监视器的统计量清零 —— 那是它的职责
             * (下一次装名单要从零起算)。所以先抄进局部量,再收拾,最后打印。
             * (第一版就是先收拾再打印,打出来全是 0。)
             */
            u32 ev;
            u32 late;
            u32 full;
            u32 wmax;
            u32 observed;
            u32 skipped;
            u32 sticky_left;
            u64 t_wall = timer_read_us();

            wait_ms_wall(STARVE_CTL_MS);
            t_wall = timer_read_us() - t_wall;

            ev          = g_starve_events;
            late        = g_starve_late;
            full        = g_starve_full;
            wmax        = g_starve_win_max;
            observed    = g_starve_ticks;
            skipped     = g_starve_skipped;
            sticky_left = g_pick_sticky; /* 应当为 0:粘住的次数在窗口内用完 */
            g_starve_ctl_adv = starve_advanced();

            /*
             * ★ 判据:监视器报警了 **而且** 受害者真的没推进 ★
             *
             * 两个检出信号,任一成立即可 —— 它们量的是同一件事的两面:
             *   events>0  "某个**可运行**的被观察线程,整整一个窗口没推进"
             *   late >0   监视器自己的窗口长度超过 4N —— 连它都没被调度到
             *             (它从 64ms 的周期被拖到上千 ms,那就是饥饿本身)
             *
             * ⚠ `adv < K` 这一条是防"报警是真的、但负载压根没被饿着"的:
             *   4 个里都推进过就不能叫饿死。它是**辅助判据**,不是主判据 ——
             *   上板实测里主判据(events/late)是稳的,而"哪些受害者最终
             *   推进了"取决于粘住结束后谁先被选中,那不是我设计的东西。
             *
             * ⚠ `sticky_left` **不参与判定**(第一版把它写进判据,结果因为
             *   "粘住还没用完但监视器已经报警"而误判为未检出)。它只作为
             *   时序信息打出来:0 = 粘住次数真的用完了;非 0 = 粘住被
             *   sa[0] 自己的到期提前结束(那也是合法的一次对照,见上)。
             */
            g_starve_ctl_detect = (((ev != 0u) || (late != 0u)) && (g_starve_ctl_adv < STARVE_K)) ? 1u
                                                                                                  : 0u;

            /*
             * ---- 收拾,而且**在打印之前** ----
             *
             * 9600 波特下一行要 60~80ms,而系统一直在跑。窗口结束时
             * `g_pick_sticky` 已经归零(粘够了),但四个受害者还在就绪队列里 ——
             * 打印期间它们会被挑中、发现自己已经超期、然后挂起。
             * 那不致命,但会让"打印出来的数"与"打印之后的状态"不一致,
             * 前四个对照组已经被这件事咬过一次(见 9.75 那段留痕)。
             */
            for (i = 0u; i < STARVE_K; i++) {
                sa[i]->status      = WAIT;
                sa[i]->wakeup_time = 0u;
            }
            starve_arm(NULL, 0u); /* 撤销观察名单:现在没有东西在被监视 */
            g_pick_sticky = 0u;

            console_excl_begin();
            console_printf(" No-starve A/B: sticky n=%u left=%u phase=%u ms observed=%u tick "
                           "skipped=%u\n",
                           STARVE_STICKY_N, sticky_left, (u32)(t_wall / 1000u), observed, skipped);
            console_printf(" No-starve A/B: adv=%u/%u events=%u late=%u full=%u win_max=%u tick\n",
                           g_starve_ctl_adv, STARVE_K, ev, late, full, wmax);
            console_printf(" No-starve A/B: counts=%u,%u,%u,%u(粘住的跑满,其余停在 0)\n",
                           g_starve[0].count, g_starve[1].count, g_starve[2].count, g_starve[3].count);
            console_printf(" No-starve A/B: strict non-preempt -> %s(可运行却被无限期漏掉)\n",
                           g_starve_ctl_detect ? "DETECTED" : "MISSED - detector not load-bearing");
            console_excl_end();
        }
    }

    /* ---- 9.79 ★ M4-10 的破坏性对照组:选核永远给 CPU0 ★ ---- */
    /*
     * 同一批负载、同一个调度器,**只把"放哪个核"改成永远 CPU0**
     * (`sched_set_pick_cpu0(1)`)—— 也就是 M4-10 之前的行为。
     *
     *   选核开(相 6 实测):线程铺到两个核 ⇒ `cpu1 switched=+N preempted=+M`
     *   选核关          :CPU1 的队列里一个可运行线程都没有 ⇒
     *                    **`switched[1]` 的增量恒为 0**、而 CPU0 的队列里
     *                    堆着全部线程(CPU1 只剩自己的 idle 在空转)
     *
     * ⚠ 判据是"**CPU1 一次都没切**"(检出),不是"切得少了"。
     *   与前五组一样:这里是**预期它坏** —— 它证明"挑最短队列"这件事承重。
     *
     * ⚠ 这一相**不需要**收拾:开关置 1 就是 M4-10 之前的行为,而负载线程
     *   跑完会自己挂起(不会把 CPU 占死)。
     */
    {
        tcb_t sa2[SMP_CTL_THREADS];
        u32   i;
        u32   made = 0u;
        u32   sw1_before = g_percpu[1].switched;
        u32   sw0_before = g_percpu[0].switched;

        sched_set_pick_cpu0(1u); /* ★ 对照组:全塞 CPU0 ★ */

        g_smp_ran[0] = 0u;
        g_smp_ran[1] = 0u;
        g_smp_assign[0] = 0u;
        g_smp_assign[1] = 0u;

        for (i = 0u; i < SMP_CTL_THREADS; i++) {
            sched_disable();
            /*
             * ⚠ 探针槽接在相 6 后面用(SMP_SCHED_THREADS + SMP_APP_THREADS 起)——
             *   探针**不看自己那格里的旧值**,只往里写(图案/核号/计数),
             *   所以这里不必清;相 6 读过的那些值早就取走了。
             */
            sa2[i] = sched_kthread_create(smp_sched_probe,
                                          &g_smp_probe[SMP_SCHED_THREADS + SMP_APP_THREADS + i], "ab0");
            sched_enable();
            if (sa2[i] == NULL) {
                continue;
            }
            made++;
            if (sa2[i]->cpu_id < PERCPU_MAX_CPUS) {
                g_smp_assign[sa2[i]->cpu_id]++;
            }
        }

        /*
         * ★ 非空转:对照组必须**真的造出线程**来了 ★
         *
         * 上板踩过一次:线程池(KSTACK_SLOTS=32)在前面几相就快用满了,
         * 这里 6 个线程全部创建失败 ⇒ `assign=(0,0) ran=(0,0)`,
         * 而判据"CPU1 一次都没切"照样成立 ⇒ 打出一句**漂亮的假 DETECTED**。
         * ⇒ 判定里必须带上"这一相真的发生了什么"。
         *   (顺带:这也是 D14"线程没有退出路径"的直接后果 —— 每造一个
         *    线程就永久占一个栈槽。自检里新增 `kstack_headroom` 盯着它。)
         */
        if (made < SMP_CTL_THREADS) {
            console_excl_begin();
            console_printf(" Smp-pick A/B: INCONCLUSIVE(只造出 %u/%u 个线程 —— "
                           "线程池可能满了)\n",
                           made, SMP_CTL_THREADS);
            console_excl_end();
        } else {
            wait_ms_wall(400u); /* 截止时刻语义,不是 N 次串行 1ms —— 见坑 39 */
        }

        g_smp_ctl_cpu1_delta = g_percpu[1].switched - sw1_before;
        g_smp_ctl_cpu0_delta = g_percpu[0].switched - sw0_before;

        /*
         * ★ 判据必须**非空转** ★
         *
         * 第一版这里只判"CPU1 一次都没切" —— 结果在两种"什么都没发生"的
         * 启动里也报 **DETECTED**:① CPU1 压根没上线(一个不存在的核当然
         * 一次都没切);② 线程没造出来。这正是本项目反复踩的那类事
         * (判据看似成立,其实什么都没在测)。
         *
         * ⇒ 先要求三件事都成立,再判对照组;否则结论是 **INCONCLUSIVE**:
         *   相 6 已经证明过 CPU1 会调度 / 这一相真的造出了线程。
         */
        if ((g_smp_cpu1_switched_at == 0u) || (made < SMP_CTL_THREADS)) {
            g_smp_ctl_inconclusive = 1u;
            g_smp_ctl_detect       = 0u;
        } else {
            g_smp_ctl_detect =
                ((g_smp_ctl_cpu1_delta == 0u) && (g_smp_ctl_cpu0_delta > 0u) &&
                 (g_smp_assign[1] == 0u))
                    ? 1u
                    : 0u;
        }

        console_excl_begin();
        console_printf(" Smp-pick A/B: pick_cpu0=1 assign=(%u,%u) ran=(%u,%u)\n", g_smp_assign[0],
                       g_smp_assign[1], g_smp_ran[0], g_smp_ran[1]);
        console_printf(" Smp-pick A/B: cpu1 switched=+%u (should be 0)  cpu0 switched=+%u\n",
                       g_smp_ctl_cpu1_delta, g_smp_ctl_cpu0_delta);
        if (g_smp_ctl_inconclusive != 0u) {
            console_printf(" Smp-pick A/B: INCONCLUSIVE(相 6 没证明 CPU1 会调度 —— "
                           "这个对照组成立不了)\n");
        } else {
            console_printf(" Smp-pick A/B: pick-cpu0 -> %s(这就是「另一个核全程闲着」的样子)\n",
                           g_smp_ctl_detect ? "DETECTED" : "MISSED - not load-bearing");
        }
        console_excl_end();

        /* 收拾:开关恢复成生产值(0 = 挑最短队列)*/
        sched_set_pick_cpu0(0u);
    }

    /* ---- 9.7 切换器的破坏性对照组(必须在自检报告**之后**)---- */
    /*
     * 为什么放在最后:**这个对照组会故意毁掉调用者的栈**。
     *
     * 它跑的是同一段切换代码,只把"换栈"那一步关掉(`g_ctx_skip_sp = 1`)。
     * 于是被恢复的上下文带着被切换者的 sp 跑起来 —— 它的栈帧就压在
     * **调用者(kmain)的栈**上。
     *
     * 实测代价:第一次把它放在自检之前时,kmain 的三个局部变量
     * (`uart_present` / `uart_clock_source` / `uart_loopback_ok`)
     * 被踩成了代码地址一类的东西,报告里凭空多出 3 项 FAIL。
     *
     *   ⇒ 那三条 FAIL 不是"自检坏了",恰恰是这条判据要证明的事情本身:
     *     **不换栈不会报任何错,只会静默踩坏别人的东西。**
     *
     * 所以它只能放在最后跑,而且结论以普通输出给出(不进报告)——
     * 进了报告反而会因为它自己造成的破坏而变成误报。
     *
     * ⚠ M4-9 之后 `arch_ctx_switch` **不再被调度器使用**(它走帧路径了),
     *   所以这一段验的是"这个原语本身"—— 它仍然是 M4-11 那些
     *   可睡眠原语的基础。放在 `sched_kern_init()` 复位之后,
     *   两者不会互相干扰。
     */
    if (g_kstack.inited) {
        kstack_t stk_c;
        u32      i;

        if (kstack_alloc(&g_kstack, &stk_c) == KSTACK_OK) {
            g_sw_b_sp  = 0u;
            g_sw_b_ran = 0u;
            for (i = 0; i < 13u; i++) {
                g_ctx_b.r[i] = 0u;
            }
            g_ctx_b.sp = stk_c.top;
            g_ctx_b.pc = (u32)(uintptr_t)switch_probe_b;

            /* ★ 先把期望区间抄到全局量 —— 下面这一步会踩坏局部变量 ★ */
            g_sw_ab_base = stk_c.base;
            g_sw_ab_top  = stk_c.top;

            g_ctx_skip_sp = 1u; /* ★ 对照组:不换栈 ★ */
            if (arch_ctx_save(&g_ctx_a) == 0u) {
                g_ctx_a.r[0] = 1u;
                arch_ctx_switch(&g_ctx_throwaway, &g_ctx_b);
            }
            g_ctx_skip_sp = 0u;

            g_sw_nosp_detected = ((g_sw_b_sp < stk_c.base) || (g_sw_b_sp > stk_c.top)) ? 1u : 0u;
            g_sw_b_ran_nosp    = g_sw_b_ran;

            console_printf(" Switch A/B  : skip_sp -> b_sp=0x%08X (若换栈应落在 "
                           "[0x%08X,0x%08X))\n",
                           g_sw_b_sp, g_sw_ab_base, g_sw_ab_top);
            console_printf(" Switch A/B  : 未换栈 -> %s(这就是「静默踩栈」的样子)\n",
                           g_sw_nosp_detected ? "检出" : "未检出 —— 判据不承重!");

            /* ⚠ 不能用 stk_c 了:它已经在上面被踩坏 */
            (void)stk_c.slot;
        }
    }
    /* 自检之后才开命令通道:在此之前串口还在标定,回显会乱 */
    shell_init();
    shell_banner();

    /* ---- 11. 主循环 ---- */
    /*
     * 节奏完全由全局定时器决定,不依赖软件延时循环 ——
     * 这样即使 CPU 频率变化,闪烁频率也保持一致。
     *
     * 有串口时约每秒打印一条状态行并持续输出。
     * 在"不确定哪个 COM 口 / 波特率对不对"的阶段,一个稳定可预期的
     * 周期信号比一次性的启动横幅好找得多。
     */

    /*
     * ★★★ "整机跑到了这里"的签名 —— 由验证脚本**强制要求** ★★★
     *
     * 为什么不靠自检报告:报告是**报告那一刻**的快照。报告之后还有七组
     * 破坏性对照组(9.73…9.7)要把调度状态搅来搅去,而它们出问题的方式
     * 通常是**日志中途断掉**(异常 → 停机),那时报告早已打完、项项全绿。
     *
     * 实测教训(坑 53):M4-11.1 的第一次验证只看了"报告全绿 + 我自己那条 A/B
     * 检出",而**整机在 9.78 之前就 Data Abort 了** —— 直到事后逐行比对
     * 完整日志才发现(`No-starve A/B` / `Smp-pick A/B` 那几行压根没出现)。
     * ⇒ 把"跑完了"变成**机器可判**的:这一行必须出现,否则验证失败
     *   (见 tmp-test/verify_board.py 的 POST_SIGNATURE)。
     */
    console_excl_begin();
    console_printf(" Boot complete: all post-report A/B groups done, entering main loop\n");
    console_excl_end();

    for (;;) {
        u32 gt    = timer_read_ticks_low();
        u32 step  = gt >> 26;                  /* 333.33MHz >> 26 约 5 Hz */
        u32 ps_ph = (gt >> 28) & 1u;           /* 约 1.2 Hz */

        if (ps_ph != last_ps) {
            led_ps_set(ps_ph != 0);
            last_ps = ps_ph;
            HB[HB_SLOT_PSLED] = ps_ph;
        }

        if (step != last_step) {
            u32 sw  = sw_read();
            u32 bar = 1u << (step & 7u);

            /* 跑马灯;某位拨码开关闭合时该位取反 -> 占空比 1/8 变 7/8 */
            led_pl_set((u8)(bar ^ sw));

            last_step = step;
            loop_count++;

            HB[HB_SLOT_LOOP] = loop_count;
            HB[HB_SLOT_LED]  = bar ^ sw;
            HB[HB_SLOT_SW]   = sw;
            HB[HB_SLOT_GT]   = gt;

            /*
             * 第二个核的计数同步到心跳。
             *
             * ⚠ 这里只是**搬运** CPU1 自己维护的计数,
             *   不是代它生成。loops 由 CPU1 的主循环自增,
             *   所以这个值在变化本身就证明 CPU1 真的在独立推进 ——
             *   如果 CPU0 代写,数字照样会变,但那什么也证明不了。
             */
            HB[HB_SLOT_CPU1_LOOPS]  = g_percpu[1].loops;
            HB[HB_SLOT_CPU1_ONLINE] = g_percpu[1].online;
        }

        if (uart_present) {
            /*
             * ★ 1Hz 状态行**已经搬进 `status_thread` 里了**(M4-8.4)★
             *
             * 原来这里有一段 `if (tick != last_print) { … console_printf("alive…") }`,
             * 现在它由那个内核线程负责 —— 于是"周期任务"第一次不再是主循环里
             * 的一个 if,而是一个真的会睡会醒的线程。
             *
             * 主循环这边要注意的是:**它是 idle**,所以它能在任何时候被抢占。
             * 下面这些都不是原子的读改写,抢占只会让数字抖一下,不会坏。
             */
        }

        /*
         * 故障注入钩子:JTAG 往 PLAT_FAULT_SEL_ADDR 写码即触发对应异常,
         * 用来验证异常诊断路径。详见 arch/fault_test.h。
         */
        fault_test_poll();

        /*
         * 串口命令通道。**非阻塞** —— 没有输入就立刻返回,
         * 所以跑马灯与周期状态行完全不受影响。
         *
         * 放在主循环末尾是有意的:命令可能很慢(比如 dump 要打十几行),
         * 放前面会让这一轮的 LED 更新被推迟。放末尾则最坏情况只是
         * 下一轮稍微晚一点,节奏仍然由全局定时器决定。
         */
        shell_poll();
    }

    /* 不会到这里 */
    fail_stop();
}
