/*
 * 内核侧调度器 —— M4-8.3 起步,M4-9 完成
 *
 * 与 src/sched.c 的分工:
 *   sched.c      **纯逻辑**(策略 + 帧搬迁的数据搬运)。不含 MMIO/CP15,
 *                宿主可穷尽测 —— 那是策略的唯一判据,因为"等权 + 纯占用"
 *                负载下任何策略都会通过。
 *   本文件        把这些策略接到真实的 TCB、栈池、上下文切换上。
 *
 * 本文件里唯一"架构相关"的动作是切栈,而它已经由 M4-7 的
 * `arch_ctx_switch()` / M4-9 的"帧路径"封装好了 —— 所以这里连内联汇编都没有。
 *
 * ====================================================================
 * 唯一的一条切换路径(M4-9)
 * ====================================================================
 *
 * 源 OS 只有**一个**切换点:`timer_handle()`,跑在异常帧上。
 * `scheduler_yield()` 也不另开一条路 —— 它只是
 * "`scheduler_ticks = TIME_SLICE; int $32`",即软中断进同一个入口。
 *
 * 本移植照这个形状做:
 *
 *     timer IRQ ──┐
 *                 ├──> c_irq_handler / c_svc_handler ──> sched_tick(frame)
 *     svc #YIELD ─┘                                            │
 *                                       ┌──────────────────────┘
 *                          "从哪个帧离开"← 不切换:原 frame
 *                                          切换:在目标栈上新搭的帧
 *
 * M4-8 曾经用 `arch_ctx_switch` 另拼过一个协作式让出 —— 那是权宜之计
 * (那时还没有"返回值即新帧"的协议),M4-9 之后它必须退场:
 * 两套机制保存的寄存器集不同,一个被抢占过的线程若被"协作式"切回来,
 * r1-r3/r12 就是垃圾,而这种错**不报任何东西**。
 * `arch_ctx_switch` 现在只剩 M4-7 那个原语自检在用。
 */

#include <arch/console.h>
#include <arch/cpu.h>
#include <arch/heap.h>
#include <arch/kstack.h>
#include <arch/percpu.h>
#include <arch/sched.h>
#include <arch/tcb.h>
#include <arch/timer.h>
#include <krlibc.h>

/* 每核一个就绪队列。**不放进 percpu_t**:那个结构体要求"不许有指针字段"
 * (宿主与目标的布局必须一致),而 sched_queue_t 里有个 tcb_t。 */
static sched_queue_t g_runq[PERCPU_MAX_CPUS];

static u32 g_switch_count;

/* 启动上下文被注册成的那个 idle TCB(照源 OS)。静态实例:它永远存在 */
static struct arm_thread_control_block g_boot_idle_tcb;
static tcb_t                           g_boot_idle;

/*
 * 启动上下文的丢弃槽 —— **M4-9 起不再需要**。
 *
 * M4-8 的协作式让出用 `arch_ctx_switch(&g_boot_ctx, &next->ctx)` 从启动流程
 * 切走,因为 `arch_ctx_switch(from, to)` 总要先**保存**当前状态到 from,
 * 而那时没有任何"当前线程"可以当 from。
 *
 * M4-9 的帧路径不需要它:现场是**收进 `cur->ctx`** 的,而启动上下文
 * 从 `sched_register_boot_idle()` 那一刻起就有一个真实的 TCB 当容器。
 * 于是这里没有这个变量了 —— 留一段说明,免得后来者以为它丢了。
 */

/*
 * 栈池与堆由调用方在启动时绑进来,而不是 extern 全局量 ——
 * kmain 里那两个是 static 的,而且"模块需要什么就显式给它"比
 * "偷偷引一个全局量"更容易在宿主测试里替换。
 */
static kstack_pool_t *g_ks;
static heap_t        *g_heap;

/* ------------------------------------------------------------------ */
/* current 的唯一真相                                                  */
/* ------------------------------------------------------------------ */

tcb_t sched_current(void)
{
    percpu_t *pc = percpu_self();

    if (pc == NULL) {
        return NULL;
    }

    return percpu_task_ptr(pc);
}

void sched_set_current(tcb_t t)
{
    percpu_t *pc = percpu_self();

    if (pc == NULL) {
        return;
    }

    pc->current_task = (t == NULL) ? 0u : (u32)(uintptr_t)t;

    /*
     * ★ 顺带把"这个线程的浮点现场在哪"算好,交给汇编 ★
     *
     * 存/取浮点现场要卡在 C 调用链的两端(`_vec_irq` 里的两条 `bl`),
     * 所以那段汇编不能去查 TCB 的偏移 —— TCB 有指针字段,宿主与目标的
     * 布局不一样,而每核结构是"全 u32"的,它的偏移两边一致。
     * ⇒ 偏移在 C 里算,结果放进每核结构;见 arch/percpu.h 的说明。
     *
     * ⚠ `t == NULL` 时两个指针必须清 0 —— 汇编拿它当"没有 current,
     *   别碰浮点"的判据(`arch_vfp_save_current` 里的 `bxeq lr`)。
     *   留着上一次的地址会让 CPU1 或启动早期去写一个不相干的 TCB。
     */
    if (t == NULL) {
        pc->cur_vfp_d = 0u;
        pc->cur_vfp_f = 0u;
        g_vfp_save_f  = 0u;
        return;
    }

    pc->cur_vfp_d = (u32)(uintptr_t)t->vfp;
    pc->cur_vfp_f = (u32)(uintptr_t)&t->fpscr;
    g_vfp_save_f  = pc->cur_vfp_f;
}

/* ------------------------------------------------------------------ */

void sched_kern_bind(kstack_pool_t *ks, heap_t *heap)
{
    g_ks   = ks;
    g_heap = heap;
}

void sched_kern_init(void)
{
    percpu_t *pc = percpu_self();

    if (pc == NULL) {
        return;
    }

    sched_queue_init(&g_runq[pc->cpu_id]);
    g_switch_count = 0u;
}

/* ------------------------------------------------------------------ */
/* 调度开关 ← `disable_scheduler()` / `enable_scheduler()`             */
/* ------------------------------------------------------------------ */

/*
 * ← `scheduler.cpp:31-34` 与 KernelMain 里的 `enable_scheduler()`。
 *
 * ## 与源 OS 的一处差别(要说清楚)
 *
 * 源 OS 里 `is_scheduler` **初值是 false**,直到 KernelMain 走到
 * "打开调度器"那一步才置真 —— 在此之前每个 tick 都在 `timer_handle` 的第一行
 * 就返回了。本移植没有那个"打开"的时刻:`sched_tick` 从第一发中断起就被调用,
 * 靠 `current == NULL` 早退(注册 idle 之前 current 就是空的)。
 * 两种写法在注册 idle 之前的行为完全一致。
 *
 * ⇒ 所以这里的**初值是"开"**,这对开关只用于"造线程/改调度状态"的窗口 ——
 *   那是一个真实存在的竞态:`sched_kthread_create` 是微秒级,而 tick 是毫秒级,
 *   于是"造第一个"和"造第二个"之间**可能**被切走,第二个线程根本没造出来。
 *   属于"偶尔错一次"的那一类,最难查,所以用开关罩起来而不是靠时序侥幸。
 */
static u32 g_sched_enabled = 1u;

/*
 * ★ M4-8.5:"关调度"的**嵌套深度**与**累计时长** ★
 *
 * ## 为什么需要深度而不只是布尔
 *
 * 关调度的用途有两个,而且会**互相嵌套**:
 *   - `console_excl_begin/end`(排他输出,见 arch/console.h)—— 它自己带计数;
 *   - kmain 里"造线程 / 改调度状态"那种多步操作的窗口。
 *
 * 只要有一次不成对(或者嵌套里的内层先 `enable`),布尔版本就会**提前打开**
 * 调度 —— 而"提前打开"不会报任何错,只表现为"偶尔被切走一次",属于最难查的
 * 那一类。改成计数之后,`enable` 只在深度归零时才真的打开。
 *
 * 不变式:深度为 0 ⟺ 调度开着。于是"多调一次 enable"是无害的空操作,
 * 而不是把某个还没结束的临界区打开。
 *
 * ## 累计时长是给饥饿监视器用的
 *
 * 排他输出期间(自检报告要好几秒)**整个系统停摆**:所有线程都醒不过来。
 * 监视器必须能把"有人故意关了调度"与"某个线程被饿着"分开,否则报告一打完
 * 它就会报一次假警。
 *
 * ⚠ 分开的办法**不是**"有这个量就作废整个窗口":饥饿也会让窗口变长,两者会
 *   叠加在同一个窗口里(上板实测就是这么漏报的)。监视器用的是
 *   `窗口长度 - 停摆时长`,见 `sched_off_total_ns()` 的说明与
 *   src/kmain.c 的 `starve_monitor`。
 */
static u32 g_sched_off_depth;
static u64 g_sched_off_since; /* 深度从 0 变 1 的那一刻(纳秒)*/
static u64 g_sched_off_total; /* 累计关掉的纳秒数 */

void sched_disable(void)
{
    if (g_sched_off_depth == 0u) {
        g_sched_off_since = timer_read_ns();
    }
    g_sched_off_depth++;
    g_sched_enabled = 0u;
}

void sched_enable(void)
{
    if (g_sched_off_depth == 0u) {
        g_sched_enabled = 1u; /* 不成对的 enable:无害空操作(见上)*/
        return;
    }

    g_sched_off_depth--;
    if (g_sched_off_depth == 0u) {
        u64 now = timer_read_ns();

        g_sched_off_total += (now - g_sched_off_since);
        g_sched_off_since = 0u;
        g_sched_enabled   = 1u;
    }
}

bool sched_is_enabled(void)
{
    return g_sched_enabled != 0u;
}

u64 sched_off_total_ns(void)
{
    /*
     * 正在关着的时候把**已经过去的那一段**也算进去 ——
     * 否则"报告打到一半时取数"会得到一个偏小的值,而那种偏小会让
     * 监视器把一个停摆窗口误判成饥饿窗口。
     */
    if (g_sched_off_depth == 0u) {
        return g_sched_off_total;
    }

    return g_sched_off_total + (timer_read_ns() - g_sched_off_since);
}

static sched_queue_t *self_runq(void)
{
    percpu_t *pc = percpu_self();

    return (pc == NULL) ? NULL : &g_runq[pc->cpu_id];
}

tcb_t sched_kthread_create(void (*entry)(void *), void *arg, const char *name)
{
    struct arm_thread_control_block *t;
    kstack_t                         stk;
    sched_queue_t                   *q;
    u32                              i;

    if (entry == NULL) {
        return NULL;
    }

    q = self_runq();
    if (q == NULL || g_ks == NULL || g_heap == NULL) {
        return NULL;
    }

    t = (tcb_t)heap_alloc(g_heap, (size_t)sizeof(struct arm_thread_control_block));
    if (t == NULL) {
        return NULL;
    }
    memset(t, 0, sizeof(*t));

    if (kstack_alloc(g_ks, &stk) != KSTACK_OK) {
        (void)heap_free(g_heap, t);
        return NULL;
    }

    t->task_level    = TASK_KERNEL_LEVEL;
    t->status        = START;
    t->parent_group  = NULL;
    t->kernel_stack  = stk.top;
    t->kstack_base   = stk.base;
    t->kstack_guard  = stk.guard;
    t->kstack_slot   = stk.slot;
    t->owns_kstack   = true;

    if (name != NULL) {
        for (i = 0; i < sizeof(t->name) - 1u && name[i] != '\0'; i++) {
            t->name[i] = name[i];
        }
        t->name[i] = '\0';
    }

    /* ---- 造初始上下文 ---- */
    for (i = 0; i < 13u; i++) {
        t->ctx.r[i] = 0u;
    }
    t->ctx.r[0] = (u32)(uintptr_t)arg; /* 线程入口的第一个参数(AAPCS:r0)*/
    t->ctx.sp   = stk.top;
    t->ctx.lr   = 0u; /* 线程不该"返回";真返回了就跳到 0,那是明确的错误 */
    t->ctx.pc   = (u32)(uintptr_t)entry;

    /*
     * ★★★ 先修:模式位 —— 以及它引出的第二次修正 ★★★
     *
     * ## 第一次修正(用户点名的那处)
     *
     * 这里原本写的是 `0u`,注释说"协作式切换不动 CPSR,这里只是记录"。
     * 那句话在 M4-8 成立,在 M4-9 **不再成立** —— 帧路径会用 `rfeia`
     * 把这个值**真的装进 CPSR**。
     *
     * 而 `0` 的 CPSR.M[4:0] = **0b00000**,不是一个已分配的模式编码
     * (AArch32 里每个合法模式的 bit4 都是 1,见 taskctx_asm.h 的 ARM_MODE_*),
     * 按架构把它装回 CPSR 是 UNPREDICTABLE。
     *
     * ## 第二次修正(上板打回来的)
     *
     * 我先改成 `ARM_MODE_SVC | ARM_CPSR_F_BIT` = **0x53**,推理是
     * "模式要 SVC、I 要为 0 否则抢占无效、F 要为 1 因为 start.S 是这么设的"。
     * 听起来完整,但**漏了一位**:上板后切到新线程的**第一条指令**就
     * Data Abort(FS = 0x16,异步外部中止),而 ctx.sp / ctx.pc 全是对的。
     * JTAG 读回真值:
     *
     *     内核真实 CPSR(idle 被抢占时收进 ctx 的)= 0x80000153
     *     我造出来的                              = 0x00000053
     *                                              ^^^ bit8 = A
     *
     * `A`(bit 8,异步中止屏蔽)是复位值 1,而 **`msr cpsr_c` 碰不到 bit8**
     * (`cpsr_c` 只含 bits[7:0])—— 所以 start.S 那句 `MODE_SVC|I|F`
     * 从头到尾没动过 A,内核跑起来之后 A 恒为 1。
     * 把它写成 0 = 在切过去的那一刻解除了异步外部中止的屏蔽。
     *
     * ⇒ 所以现在**不再手拼这个值**,用唯一的那个常量 `ARM_CPSR_KERNEL`
     *   (定义与完整经过在 arch/taskctx.h),并且由
     *   `sched_ctx_switchable()` 在每次切换前核对 A/T/模式位 ——
     *   写错了会被**拒绝切换**(可诊断),而不是让核心带着一个错的 CPSR 跑。
     *   板级还有一条 `sched_cpsr_kernel` 自检,拿这个常量与
     *   "收现场时真的读到的 CPSR"对账。
     *
     * ⚠ I 位必须**为 0**:I=1 的线程永远不会被 tick 打断,抢占对它无效 ——
     *   而且那不会报错,只表现为"这个线程独占 CPU"。
     */
    t->ctx.cpsr = ARM_CPSR_KERNEL;

    /* ---- 调度状态 ---- */
    /*
     * ← `add_task()` `scheduler.cpp:551-554`:
     *       now = nanoTime();
     *       init_task_eevdf_entity(new_task,
     *           queue_average_vruntime(min_cpu->scheduler_queue, NULL, now, NULL, NULL), now);
     *       queue_enqueue_ref(min_cpu->scheduler_queue, new_task, &new_task->sched_node);
     *
     * 三项对应关系:
     *   - 起点 vruntime 取**本核队列当前的平均值**(传 NULL 的 fallback/idle,
     *     于是那次调用只取"平均"那一路,不唤醒任何东西);
     *   - `cpu_id` / `queue_index`:ARM 侧只有本核队列(M4-10 才有挑选核),
     *     `queue_index` 在侵入式队列里没有对应物,不存;
     *   - `queue_enqueue_ref` → `sched_queue_append`(FIFO 追加)。
     *
     * ⚠ 源 OS 的 `add_task` 会挑"队列最短的核" —— 那是 M4-10 的事。
     */
    sched_entity_init(t, sched_queue_avg_vruntime(q, NULL, 0u));

    if (!sched_queue_append(q, t)) {
        /* 查重失败说明本模块自己被用错了;回滚而不是留个半成品 */
        (void)kstack_free(g_ks, &stk);
        (void)heap_free(g_heap, t);
        return NULL;
    }

    return t;
}

/* ------------------------------------------------------------------ */
/* 切换                                                                */
/* ------------------------------------------------------------------ */

/*
 * ★ 破坏性 A/B 的对照组开关 ★ —— 定义与理由见 include/arch/sched.h。
 * 生产路径上恒为 0;只有 kmain 在自检报告**之后**才会短暂置 1。
 */
u32 g_reloc_skip;

/*
 * ★ 第二个破坏性 A/B 开关:跳过浮点现场的保存/恢复 ★
 *
 * ⚠ 它的**存储**在汇编里(boot/context.S 的 `.data`),这里只是声明 ——
 *   因为读者是 `_vec_irq` 那两条必须待在 C 调用链之外的 `bl`:
 *   "先调一个 C 函数问一句跳不跳"就等于把 C 又拉回了窗口里。
 *
 * 置 1 时两个线程的 d0-d31 不再随切换而换,于是**其中一个**会发现自己的
 * 寄存器变成了对方的图案。见 include/arch/sched.h 的说明。
 * 生产路径上恒为 0。
 */
extern u32 g_vfp_skip;

/*
 * 让出 CPU —— **走陷阱**,与"被抢占"是同一条路径。
 *
 * ← `scheduler_yield()` `scheduler.cpp:460-465`:
 *      get_current_cpu()->scheduler_ticks = TIME_SLICE;
 *      __asm__ volatile("int %0" ::"i"(32));
 *
 * ⚠ 这里**没有**任何"挑下一个 / 切上下文"的代码,那是刻意的:
 *   一旦这里再写一套,就有了两套切换机制、两份真相。
 *   本函数只做两件事:把时间片记账设成"已用尽",然后软中断。
 *
 * ⚠ `svc` 会把 CPSR.I 置 1(所有异常都这样),但 **SPSR_svc 里存的是
 *   进去之前的 CPSR**,而帧路径用 `rfeia` 从 spsr 恢复 —— 于是中断开关
 *   回到进入时的那一刻,不需要在这里手动存/恢复。
 */
void sched_yield(void)
{
    percpu_t *pc = percpu_self();

    if (pc == NULL) {
        return;
    }

    /*
     * 设成 TIME_SLICE(不是 +1):`sched_tick` 还会再 `scheduler_ticks++`,
     * 于是 `scheduler_ticks < TIME_SLICE` 这一关必然放过 ——
     * 与源 OS 的 `scheduler_ticks = TIME_SLICE; int $32` 逐字对应。
     */
    pc->scheduler_ticks = SCHED_TIME_SLICE;

    arch_svc_yield();
}

/*
 * 把自己挂起,然后切走。
 *
 * ⚠ 必须走 `sched_yield()` 而不是自己去挑下一个并 `arch_ctx_switch`:
 *   内核线程跑在自己的内核栈上,"切走"只有一条合法路径 ——
 *   经过异常帧,让 `rfeia` 把栈和 CPSR 一起换掉。直接从 C 里跳走
 *   会留在同一个栈上,那不是切换。
 *
 * ⚠ 本函数不会返回:状态是 WAIT,调度器不会再挑中自己。
 *   真返回了说明调度器出了别的问题,那就让它继续跑 ——
 *   在自检探针里,继续跑会立刻把 `done` 之后的死循环暴露出来。
 */
void sched_park_self(void)
{
    tcb_t cur = sched_current();

    if (cur == NULL) {
        return;
    }
    if (cur->task_level == TASK_IDLE_LEVEL) {
        return; /* 启动上下文不能把自己停掉 —— 停了就没人再跑启动了 */
    }

    cur->status      = WAIT;
    cur->wakeup_time = 0u; /* 0 = **不按时间唤醒**,不是"立刻唤醒" */

    sched_yield();
}

u32 sched_switch_count(void)
{
    return g_switch_count;
}

/* ------------------------------------------------------------------ */
/* ★ 破坏性对照组专用:把队列里每个线程的 ctx 抄一份 / 还原 ★          */
/* ------------------------------------------------------------------ */

/*
 * ## 为什么需要它(上板打出来的)
 *
 * `g_reloc_skip = 1` 的对照组做的是"决策照做,但执行流不动"。
 * 而 `sched_tick` 的收现场那一步**照做** —— 于是每换一次 current,
 * 它就把**发指令那一刻真正在跑的现场**(也就是 kmain 的)写进那个线程的 ctx。
 * 对 ca/cb 这两个对照组自己的线程,那正是目的;但对**别的常驻线程**
 * (M4-8.4 之后有一个永不退出的周期状态线程)就是**污染**:
 * 它的 ctx.sp 会指向 kmain 的启动栈,而它自己的栈在别处。
 *
 * 后果不是"对照组不准",是**那个线程从此再也切不进去**
 * (`sched_ctx_switchable()` 会一直拒绝它,`invalid` 一路涨)——
 * 实测:周期状态线程的 ctx.sp 变成 0x0018BF10(启动栈),
 * `invalid_ctx` 涨到 85139,**状态行从此不再输出**。
 *
 * 所以对照组在进窗口之前把这些 ctx 抄一份、出窗口之后还原。
 * 只抄**队列里**的线程:只有它们可能被 `sched_select_next` 挑中。
 */
u32 sched_ctx_snapshot_all(arm_task_ctx_t *out, u32 max)
{
    percpu_t    *pc = percpu_self();
    sched_queue_t *q;
    const tcb_t *link;
    u32          n = 0u;

    if (pc == NULL || out == NULL) {
        return 0u;
    }

    q = &g_runq[pc->cpu_id];
    for (link = &q->head; *link != NULL && n < max; link = &(*link)->sched_next) {
        out[n] = (*link)->ctx;
        n++;
    }

    return n;
}

void sched_ctx_restore_all(const arm_task_ctx_t *in, u32 n)
{
    percpu_t      *pc = percpu_self();
    sched_queue_t *q;
    const tcb_t   *link;
    u32            i = 0u;

    if (pc == NULL || in == NULL) {
        return;
    }

    q = &g_runq[pc->cpu_id];
    for (link = &q->head; *link != NULL && i < n; link = &(*link)->sched_next) {
        /*
         * ⚠ 按**遍历顺序**配对,不按身份匹配 —— 因为对照组的硬件性前提是
         *   "这段窗口里没有线程被创建或销毁"。真被破坏了,错配也是静默的;
         *   所以两个函数都只走队列、不分配、不加锁,窗口内不会有别的写者。
         */
        (*link)->ctx = in[i];
        i++;
    }
}

/* ------------------------------------------------------------------ */
/* ★ 睡眠与唤醒(M4-8.4)← `scheduler_sleep_ns` / `scheduler_wake_task` ★ */
/* ------------------------------------------------------------------ */

/*
 * ← `scheduler_sleep_ns()` `scheduler.cpp:468-493`,逐句对应。
 *
 *     if (!is_scheduler || nano == 0) { yield(); return; }
 *     current = get_current_task();
 *     if (current == NULL || current->task_level == TASK_IDLE_LEVEL) { yield(); return; }
 *     now = nanoTime(); wakeup = now + nano;
 *     if (wakeup < now) wakeup = (uint64_t)-1;      // 溢出钳位
 *     current->wakeup_time = wakeup; current->status = WAIT;
 *     do { yield(); } while (current->status == WAIT);
 *     if (current->status == START) current->status = RUNNING;
 *
 * ★ 它是**轮询式**的:睡下去之后靠 `yield()` 反复进调度器,每次都被
 *   `select_next_task_safe` 的扫描看一眼醒没醒。源 OS 的三处调用
 *   (`ipc.cpp:37,46`、`sys.cpp:618`)全是
 *   `do { sleep_ns(1ms); …干活…; } while (!条件);` 这个形状。
 *
 * ⚠ 谁把它唤醒:`sched_select_next` 里的那次扫描(`sched_queue_scan`
 *   → `sched_wake_if_due`)—— 睡眠的任务**留在队列里**,等着被扫到。
 *   这正是 M4-8 的"取队首"版本做不到的事:那一版根本没有扫描。
 *
 * ⚠ `yield()` 走的是 `svc` 陷阱 → `sched_tick` → 决策 → 换帧,与抢占同一条路。
 *   于是"睡着的线程被换下、到点被唤醒、再被换回来"整条链只用一套机制。
 */
void sched_sleep_ns(u64 ns)
{
    tcb_t cur;
    u64   now;
    u64   wakeup;

    if (g_sched_enabled == 0u || ns == 0u) {
        sched_yield();
        return;
    }

    cur = sched_current();
    if (cur == NULL || cur->task_level == TASK_IDLE_LEVEL) {
        /* 启动上下文不能睡 —— 没有别人能把它叫醒 */
        sched_yield();
        return;
    }

    now    = timer_read_ns();
    wakeup = now + ns;
    if (wakeup < now) {
        wakeup = (u64)-1; /* 溢出:源 OS 的原样钳位 */
    }

    cur->wakeup_time = wakeup;
    cur->status      = WAIT;

    do {
        sched_yield();
    } while (cur->status == WAIT);

    if (cur->status == START) {
        cur->status = RUNNING;
    }
}

/*
 * ← `scheduler_wake_task()` `scheduler.cpp:495-509`。
 *
 * 显式唤醒(不是"到点自动醒"):IPC、futex、将来的可睡眠锁都靠它。
 * 补偿基准取**当前任务**的 vruntime;current 是 idle(或没有)时用 0。
 */
void sched_wake_task(tcb_t t)
{
    u64   base = 0u;
    tcb_t cur;

    if (t == NULL || t->status != WAIT) {
        return;
    }

    cur = sched_current();
    if (cur != NULL && cur->task_level != TASK_IDLE_LEVEL) {
        base = cur->eevdf_vruntime;
    }

    t->wakeup_time = 0u;
    t->status      = START;
    sched_apply_wakeup_credit(t, base);
}

/* ------------------------------------------------------------------ */
/* 启动上下文 = idle(照源 OS,不是另造一个线程)                       */
/* ------------------------------------------------------------------ */

/*
 * 源 OS 的做法(`kernel/main.cpp`,在 KernelMain 里):
 *
 *     idle_thread->kernel_stack = get_rsp();
 *     idle_thread->context0.rsp = get_rsp();
 *     idle_thread->status       = RUNNING;
 *     queue_enqueue_ref(get_current_cpu()->scheduler_queue, idle_thread, ...);
 *     get_current_cpu()->current_task = idle_thread;
 *     // context0.rip 从头到尾没设过 -> 保持 0
 *
 * 也就是说:**"idle"不是另一个线程,而是启动上下文自己**。
 *
 * ⚠ 那个 `rip == 0` 标记只说明"**在被第一次切走之前**不许切进来" ——
 *   那时它的现场在 CPU 里,不在内存里,切进去就是跳到地址 0。
 *   `change_proccess` 从 idle 切走时会把 `reg->rip` 收进 `idle->context0`,
 *   于是**从此它就是一个可恢复的普通上下文**。
 *
 * ★ 这一点我先前读错过 ★ 本文件原来的注释写"idle 只被切走、从不会被切回来,
 *   所以没有'切进 idle'这回事"。那是错的,而且后果不小:启动流程被切走
 *   之后必须能回来,靠的正是"idle 变成一个普通上下文"这条路。
 *   等两个自检探针线程干完活挂起,`select_next_task` 挑不到别人、
 *   current 又不可运行时,兜底返回的就是 idle —— 于是 kmain 从被中断的
 *   那条指令继续,自检报告照常打出来。
 *
 * ARM 侧:
 *   - "上下文无效"标记同样用 `ctx.pc == 0`;
 *   - `ctx.sp` 取**当前** sp(照源 OS 的 `get_rsp()`),它只是占位 ——
 *     真正有意义的 sp 是第一次被切走时从现场帧里收进来的那个;
 *   - **idle 不进就绪队列**。源 OS 把 idle 排进队列,但
 *     `is_task_schedulable()` 里那条 `task_level == TASK_IDLE_LEVEL`
 *     把它挡在候选之外,只在最后兜底时才用;而且它的 vruntime 永远是 0
 *     (计费对它早退),deadline 恒为最小 —— 真让它参与"取队首"的挑选,
 *     它会把队首永远占住。这里用"不排队 + 显式兜底"表达同一件事。
 *
 * ⚠ 更早的第一版我造了一个真的 `for(;;) wfi()` idle 线程 + 交棒函数。
 *   那才是真正偏离源 OS 的地方。
 */
void sched_register_boot_idle(void)
{
    u32 i;

    memset(&g_boot_idle_tcb, 0, sizeof(g_boot_idle_tcb));

    g_boot_idle_tcb.task_level   = TASK_IDLE_LEVEL;
    g_boot_idle_tcb.status       = RUNNING;
    g_boot_idle_tcb.kernel_stack = arch_read_sp(); /* 当前 sp,不另取栈 */
    g_boot_idle_tcb.ctx.sp       = arch_read_sp();
    g_boot_idle_tcb.ctx.pc       = 0u; /* ★ 0 = 上下文无效(第一次切走前)★ */
    /*
     * ⚠ cpsr 必须写成内核的规范 CPSR(见 arch/taskctx.h 的 ARM_CPSR_KERNEL)。
     *
     *   留成 0 会让 `sched_ctx_switchable()` 判定"不可切换",
     *   于是 idle 被切走一次之后就永远回不来了 —— 自检报告再也打不出来。
     *
     *   注意:**这个值不久之后就会被真实值覆盖** —— 第一次从 idle 切走时
     *   `sched_ctx_from_frame()` 会把现场帧里的 spsr 收进来(板上实测是
     *   0x80000153)。所以它只在"被第一次切走之前"这一段有效,
     *   而恰好就是那一段需要它(A/T/模式位要能过 `sched_ctx_switchable`)。
     */
    g_boot_idle_tcb.ctx.cpsr    = ARM_CPSR_KERNEL;
    g_boot_idle_tcb.owns_kstack = false; /* 它就是启动栈,不归栈池管 */
    for (i = 0; i < sizeof(g_boot_idle_tcb.name) - 1u && "idle"[i] != '\0'; i++) {
        g_boot_idle_tcb.name[i] = "idle"[i];
    }
    sched_entity_init(&g_boot_idle_tcb, 0u);

    g_boot_idle = &g_boot_idle_tcb;
    sched_set_current(g_boot_idle);

    /*
     * ★ idle **要进就绪队列**(照源 OS)★
     *
     * 源 OS 的 KernelMain 就是这么做的:
     *     queue_enqueue_ref(get_current_cpu()->scheduler_queue, idle_thread, ...);
     *
     * M4-9 时我把它**排除在队列外**,理由是"它的 vruntime 恒为 0、deadline
     * 恒最小,进了队列会把队首永远占住"。那个理由只在"取队首"的选取方式下
     * 成立 —— 而源 OS 的选取是**全表扫描 + avg_vruntime 闸门**,而且
     * `sched_task_schedulable()` 里有 `task_level == TASK_IDLE_LEVEL` 一条
     * 把它排除在候选之外,只在最后兜底时才用它。
     * ⇒ 队列化之后它是"名册上的一员、但不是候选",正是源 OS 的语义,
     *   而且 `queue_average_vruntime` 也按"候选才计入"处理它(不计入)。
     */
    if (!sched_queue_append(&g_runq[0], g_boot_idle)) {
        /* 队列为空且 idle 是新对象,插不进去只可能是本模块被用错了 */
        g_boot_idle = NULL;
        sched_set_current(NULL);
    }
}

tcb_t sched_boot_idle(void)
{
    return g_boot_idle;
}

/* ------------------------------------------------------------------ */
/* tick 里给 current 计费(照源 OS)                                    */
/* ------------------------------------------------------------------ */

/*
 * ⚠ 这里原本有一个 `sched_tick_account()`(M4-8):它只做"给 current 计费",
 *   因为那时 `sched_tick` 还只做决策、不做切换,M4-8 想把"计费"单独测。
 *
 *   M4-9 把计费并进了 `sched_tick` 的第一步之后,它**没有任何调用者**了 ——
 *   而"声明 + 定义都在、就是没人调"这种东西会让人以为功能还在,
 *   所以按退化清单 D10 删掉,而不是留着。
 *   (板级自检里那一项叫 `sched_tick_account`,那是**检查项的名字**,
 *    由 `acc_probe` 线程给出结论,与本函数无关。)
 */

/* (sched_tick / sched_tick_switched / sched_tick_preempted / sched_tick_invalid_ctx
 *  实现在文件末尾 —— 它们是完整的决策路径。) */

/* ------------------------------------------------------------------ */
/* M4-9:tick 里的调度决策 —— 真的搬帧                                  */
/* ------------------------------------------------------------------ */

static u32 g_tick_switched;   /* 真的完成了切换(搬了帧)的次数 */
static u32 g_tick_preempted;  /* 其中"被换下的是真实线程"的次数 */
static u32 g_tick_invalid_ctx;

/*
 * ← `timer_handle()` `scheduler.cpp:381-458`。逐段对应:
 *
 *   if (current == NULL) { send_eoi(); return reg; }
 *   if (current 在跑 且 不是 idle) { scheduler_ticks++;
 *                                    charge_current_eevdf_runtime(current, TICK_NS);
 *                                    if (scheduler_ticks < TIME_SLICE) return reg; }
 *   else scheduler_ticks = 0;
 *
 *   best = select_next_task();
 *   if (best == NULL || best == current) { scheduler_ticks = 0; return reg; }
 *   if (current->status == RUNNING) current->status = START;
 *   if (best->status == START || best->status == CREATE) best->status = RUNNING;
 *   if (best->context0.rip != 0) { change_proccess(...); cpu->current_task = best; }
 *   else { current->status = RUNNING; scheduler_ticks = 0; }
 *
 * ⚠ 时间片计数放在**每核**结构里(源 OS 用 `cpu->scheduler_ticks`):
 *   两个核各跑各的 tick,共用一个静态变量会让两核互相把对方的时间片清零 ——
 *   表现为"抢占几乎不发生",而且只在双核下出现。
 *
 * ## 与源 OS 的两处结构差异(每一处都写清楚为什么)
 *
 * 1. **顺序:先判可切换性,再动状态。**
 *    源 OS 先把 current/best 的状态改掉,发现 `rip == 0` 再改回来。
 *    这里把判断提前,失败时无需回滚 —— 回滚漏一半是静默的。
 *    可观测行为完全一致。
 *
 * 2. **idle 的"上下文无效"判定用的是 `sched_ctx_switchable()`**,
 *    它比源 OS 那句 `context0.rip != 0` 多查几位(A/模式位/sp 对齐/sp 在自己栈区里)。
 *    多出来的每一条都对应一次真踩过的坑,见 arch/taskctx.h。
 *
 * ★ M4-8.4 之后这里**不再碰队列** ★
 *
 *   源 OS 的 `timer_handle` 只改状态,队列成员关系一动不动(current 与
 *   睡眠中的任务都留在队列里,由 `is_task_schedulable` 排除)。M4-9 的版本
 *   维护着"跑着的不在队列里",于是每次切换要一进一出 —— 那是为了配合
 *   "取队首"的选取方式。选取改回源 OS 的全表扫描之后,那套维护整段消失。
 */
arm_exc_frame_t *sched_tick(arm_exc_frame_t *frame)
{
    sched_queue_t  *q;
    tcb_t           cur;
    tcb_t           next;
    percpu_t       *pc;
    arm_exc_frame_t *nf;

    if (frame == NULL) {
        return frame;
    }

    /*
     * ← `timer_handle()` 的第一行(`scheduler.cpp:384-387`):
     *      if (!is_scheduler) { send_eoi(); return reg; }
     *   关掉时连计费都不做 —— 照抄。
     */
    if (g_sched_enabled == 0u) {
        return frame;
    }

    pc = percpu_self();
    if (pc == NULL) {
        return frame;
    }

    /*
     * ⚠ M4-10 之前**只有 CPU0 参与调度**。
     *
     * CPU1 的 1kHz 私有定时器同样会进这里,而 `g_runq[1]` 从来没被
     * `sched_kern_init()` 碰过(那是 CPU0 在启动时调的),`g_boot_idle`
     * 更是 **CPU0 的对象** —— 放 CPU1 走过去,它会去恢复 CPU0 的 idle 现场,
     * 也就是**两个核同时往同一个上下文里塞现场**。
     * 那是双核阶段典型的一类崩溃,而且现象与"内存坏了"几乎一样。
     *
     * 每个核一份队列 + 每核自己的 idle 是 M4-10 的事。
     */
    if (pc->cpu_id != 0u) {
        return frame;
    }

    q   = &g_runq[pc->cpu_id];
    cur = sched_current();

    /*
     * ★ current 为空必须早退 ★(源 OS `scheduler.cpp:390-393`)
     *
     * 没有"从哪来"可收:放过去就会挑一个线程切走,而启动上下文的 ctx
     * 从头到尾没人填过 —— 它被永久丢掉,kmain 再也回不来。
     */
    if (cur == NULL) {
        return frame;
    }

    /* ---- 1. 计费 + 时间片 ---- */
    if (cur->status == RUNNING && cur->task_level != TASK_IDLE_LEVEL) {
        pc->scheduler_ticks++;
        sched_account_run(cur, SCHED_TICK_NS);
        /*
         * ← `timer_handle()` `scheduler.cpp:398`:
         *       __atomic_fetch_add(&current->runtime_ticks, 1ULL, __ATOMIC_RELAXED);
         *
         * 「这个线程实际占了多少 tick」—— 与 `eevdf_vruntime` 不同:
         * 后者是**策略量**(将来有权重时会与真实时间脱钩),
         * 前者是**记账量**。现在还没有读者,但空着会让人以为它已经在工作,
         * 所以照源 OS 补上(退化清单 D9)。
         */
        cur->runtime_ticks++;

        if (pc->scheduler_ticks < SCHED_TIME_SLICE) {
            return frame; /* 时间片没到 */
        }
    } else {
        pc->scheduler_ticks = 0u;
    }

    /* ---- 2. 挑下一个 ← `select_next_task()` ---- */
    /*
     * ★ 一次调用里包含唤醒 ★
     *
     * `sched_select_next` 会:唤醒 current、扫描队列(顺带唤醒所有到点的
     * 睡眠任务)、按 avg_vruntime 闸门挑人、兜底到 current/idle。
     * 睡眠任务留在队列里等着被扫到 —— 这就是 `sched_sleep_ns` 能醒的原因。
     *
     * 时钟由这里传进去(本层是纯逻辑,宿主上要能构造"过了 3ms")。
     */
    next = sched_select_next(q, cur, timer_read_ns());

    if (next == NULL || next == cur) {
        pc->scheduler_ticks = 0u;
        return frame;
    }

    /* ---- 3. ★ 先判可切换性,再动任何状态 ★(源 OS 用 `context0.rip != 0`)*/
    if (!sched_ctx_switchable(next)) {
        /*
         * 源 OS 在这里把 current 的状态改回 RUNNING —— 因为它在上面
         * 已经改过了。本函数把判断放在状态迁移**之前**,所以这里
         * 什么都不用回滚(见上面"结构差异 1")。
         */
        pc->scheduler_ticks = 0u;
        g_tick_invalid_ctx++;
        return frame;
    }

    /*
     * ---- 3b. 新帧先在**只读**的前提下算出来 ----
     *
     * ⚠ 顺序上这一句必须在状态迁移之前。第一版把它放在迁移之后,于是
     *   "算不出帧"那一支就成了一个**半途而废的切换**:cur 已经重新入队、
     *   next 已经被摘下来并置成 RUNNING —— 放弃之后 current 反而是个
     *   队列成员,next 则永远不再被挑中。那种状态歪得很安静,比"不切换"
     *   难查得多。
     *
     *   现在两步都只依赖 `sched_ctx_switchable()`,所以第二步在正常路径上
     *   不可能失败;真失败了也没有任何状态被改过,直接返回即可。
     */
    nf = sched_frame_for(next);
    if (nf == NULL) {
        g_tick_invalid_ctx++;
        pc->scheduler_ticks = 0u;
        return frame;
    }

    /* ---- 4. 状态(← `timer_handle:436-441`)★ 不碰队列 ★ ---- */
    /*
     * ★ M4-8.4:这里**只有两行状态迁移**,与源 OS 一模一样 ★
     *
     * 源 OS 的 `timer_handle` 从头到尾不动队列:`current` 与睡眠中的任务
     * 都留在队列里,靠 `is_task_schedulable()` 把它们排除在候选之外。
     * 队列是"全部线程的名册",不是"就绪链表"。
     *
     * M4-9 那一版在这里做了"cur 入队 / next 出队"两件事 —— 那是为了配合
     * "跑着的不在队列里"的模型,而那个模型又是为了配合"取队首"的选取方式。
     * 选取改回全表扫描之后,两者一起消失。
     */
    if (cur->status == RUNNING) {
        cur->status = START;
    }
    if (next->status == START || next->status == CREATE) {
        next->status = RUNNING;
    }

    /* ---- 5. ★★★ 搬帧 ★★★ ---- */
    /*
     * ★ 浮点现场**不在这里换** ★
     *
     * 这里只改"谁是这个线程"的账;真正的 d0-d31 存/取由 `_vec_irq`
     * 在**这段 C 的前后两端**做(见 boot/context.S 的
     * `arch_vfp_save_current` 那段长说明)。
     *
     * 为什么不在这儿顺手做:恢复完 next 的 d0-d31 之后还要经过本函数与
     * `c_irq_handler` 的收尾,而"那些收尾会不会碰浮点寄存器"是**编译器
     * 说了算**的 —— 本内核里 GCC 已经在用 `vldr d16/vstr d16` 做 64 位
     * 清零,而下面那句 `pc->scheduler_ticks = 0u` 正是"64 位存零"。
     * 卡在 C 的两端,判据里就不含这个前提。
     *
     * 唯一需要在这里配合的是**破坏性 A/B**:`g_reloc_skip` 置 1 时执行流
     * 留在 cur,于是要让 `_vec_irq` 的恢复也跳过 —— 否则它会把 next 的
     * 现场装到正在跑的 cur 身上,对照组就同时改了两件事。
     */

    /* 5a. 收整数现场。
     *
     *     ⚠ cur 是 idle 时**照样要收** —— 那个 0 标记只是"还没被切走过",
     *       收完它就是一个可恢复的普通上下文了。这正是启动流程能回来的原因。
     */
    sched_ctx_from_frame(cur, frame);

    /* 5b. 在**目标任务自己的栈上**搭新帧(地址已在 3b 算好)*/
    (void)sched_frame_from_ctx(next, nf);

    sched_set_current(next);
    pc->scheduler_ticks = 0u;
    g_switch_count++;
    g_tick_switched++;
    if (cur->task_level != TASK_IDLE_LEVEL) {
        g_tick_preempted++;
    }

    /*
     * ★ 破坏性 A/B:只差这一步 ★
     *
     * 上面全部照做,只有"把新帧交出去"被跳过。返回原来的帧 ⇒ `rfeia`
     * 回到**原线程被打断的地方**,执行流一步没动。
     * 于是同一段代码、同一个负载,行为从"两个线程真的交错"
     * 变成"两个线程一次都没跑起来" —— 那是"搬帧承重"的唯一证明方式。
     *
     * ⚠ 浮点现场也必须**跟着不换**:执行流留在 cur,而 `_vec_irq` 的
     *   恢复是照"新的 current"来的 —— 所以这里把每核结构里那两个指针
     *   一起清掉,让那一句变成空操作。对照组于是**只改了搬帧这一件事**。
     */
    if (g_reloc_skip != 0u) {
        pc->cur_vfp_d = 0u;
        pc->cur_vfp_f = 0u;
        g_vfp_save_f  = 0u;
        return frame;
    }

    return nf;
}

u32 sched_tick_switched(void)
{
    return g_tick_switched;
}

u32 sched_tick_preempted(void)
{
    return g_tick_preempted;
}

u32 sched_tick_invalid_ctx(void)
{
    return g_tick_invalid_ctx;
}
