/*
 * 内核侧调度器 —— M4-8.3
 *
 * 与 src/sched.c 的分工:
 *   sched.c      **纯逻辑**(策略)。不含 MMIO/CP15,宿主可穷尽测 ——
 *                那是策略的唯一判据,因为"等权 + 纯占用"负载下任何策略都会通过。
 *   本文件        把这些策略接到真实的 TCB、栈池、上下文切换上。
 *
 * 本文件里唯一"架构相关"的动作是切栈,而它已经由 M4-7 的
 * `arch_ctx_switch()` 封装好了 —— 所以这里连内联汇编都没有。
 *
 * ⚠ M4-8 的切换是**协作式**的(线程自己调 sched_yield 让出)。
 *   抢占在 M4-9:那一步要在异常返回路径上做切换,也就是真正需要
 *   "连 CPSR 一起换"的地方(见 boot/context.S 顶部的分工表)。
 */

#include <arch/console.h>
#include <arch/cpu.h>
#include <arch/heap.h>
#include <arch/kstack.h>
#include <arch/percpu.h>
#include <arch/sched.h>
#include <arch/tcb.h>
#include <krlibc.h>

/* 每核一个就绪队列。**不放进 percpu_t**:那个结构体要求"不许有指针字段"
 * (宿主与目标的布局必须一致),而 sched_queue_t 里有个 tcb_t。 */
static sched_queue_t g_runq[PERCPU_MAX_CPUS];

static u32 g_switch_count;

/* 启动上下文被注册成的那个 idle TCB(照源 OS)。静态实例:它永远存在 */
static struct arm_thread_control_block g_boot_idle_tcb;
static tcb_t                           g_boot_idle;

/*
 * 启动上下文的丢弃槽。
 *
 * `arch_ctx_switch(from, to)` 总会**保存**当前状态到 from。从启动流程
 * 切进 idle/第一个线程时,没有任何"当前线程"可以当 from ——
 * 于是给它一个明确的丢弃槽,而不是 `&idle->ctx`(那会把 idle 自己的
 * 上下文覆盖掉,下次切到 idle 就跳回启动流程了)。
 */
static arm_task_ctx_t g_boot_ctx;

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
    t->ctx.cpsr = 0u; /* 协作式切换不动 CPSR,这里只是记录 */

    /* ---- 调度状态 ---- */
    sched_entity_init(t, sched_queue_avg_vruntime(q, NULL, 0u));

    if (!sched_queue_insert(q, t)) {
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
 * 让出 CPU。这是 M4-8 唯一改变执行流的函数,所以每一句为什么都要写清楚。
 */
void sched_yield(void)
{
    sched_queue_t *q = self_runq();
    tcb_t          cur;
    tcb_t          next;
    u64            ran;

    if (q == NULL) {
        return;
    }

    cur = sched_current();

    /*
     * 给"刚刚跑过的那一段"计费。
     *
     * ⚠ 计费必须在**入队之前**:入队要按 deadline 排序,而 deadline
     *   正是由这次计费算出来的。顺序反了的话,线程会带着上一次的
     *   deadline 去排队,公平性立刻失真 —— 而且那种失真不会报错,
     *   只表现为"某些线程拿到的 CPU 偏多",极难察觉。
     *
     * 用固定片长而不是读时钟:协作式让出本来就不按时间片,
     * 而"跑一次算一片"与源 OS 的 tick 计费在长期份额上等价,
     * 又不必在这条热路径上碰全局定时器。
     */
    if (cur != NULL && cur != g_boot_idle) {
        cur->status = RUNNING;
        sched_account_run(cur, SCHED_BASE_SLICE_NS);
    }

    next = sched_pick(q);
    if (next == NULL || next == cur) {
        return;
    }

    /*
     * ⚠ `next->ctx.pc == 0` 表示"这个线程的上下文无效"(源 OS 用
     *   `context0.rip != 0` 做同一个判断)。挑到这种线程时**不许切过去** ——
     *   它的现场根本不在内存里(启动上下文就是这样)。
     *   源 OS 在这种情况下是"什么都不做,继续跑 current"。
     */
    if (next->ctx.pc == 0u) {
        return;
    }

    /* ---- 把当前线程放回队列,再把下一个摘下来 ---- */
    if (cur != NULL && cur != g_boot_idle) {
        cur->status = START;
        (void)sched_queue_insert(q, cur);
    }

    sched_queue_remove(q, next);
    next->status = RUNNING;

    sched_set_current(next);
    g_switch_count++;
    if (g_switch_count > SCHED_MAX_SWITCHES_TRACKED) {
        /*
         * 计数器不封顶会让"切换太频繁"这件事在日志里变成一个大数字;
         * 反过来,封顶又会让它失去"到底切了多少次"的信息。
         * 这里保留真值,只是不再往队列里加 —— 真正的开销在切换本身。
         */
    }
    (void)ran;

    /*
     * ★ 切换点 ★
     *
     * `arch_ctx_switch(&cur->ctx, &next->ctx)` —— 它会把当前寄存器与栈
     * 存进 `cur->ctx`,再从 `next->ctx` 恢复。
     *
     * ⚠ 从 cur 的角度看,**这一句之后不会立刻继续**:要等别的线程切回来。
     *   所以它下面不能假设 `next` 还是被选中的那个 —— 回来时世界已经变了。
     *
     * ⚠ cur 为 NULL(第一次启动)时不能走这条路:`&cur->ctx` 是空指针解引用。
     *   那时的正确做法是"只恢复 next,不保存任何人" ——
     *   用一个丢弃槽当 from。
     */
    if (cur == NULL || cur == g_boot_idle) {
        /*
         * 从"上下文不在内存里"的线程切走:没有现场可保存,用丢弃槽。
         * (启动上下文就属于这种 —— 它的现场在 CPU 里,而且按源 OS 的模型
         *  它只被切走、会被 M4-9 的帧改写路径切回来。)
         */
        arch_ctx_switch(&g_boot_ctx, &next->ctx);
    } else {
        arch_ctx_switch(&cur->ctx, &next->ctx);
    }
}

u32 sched_switch_count(void)
{
    return g_switch_count;
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
 * 它的 `context0.rip` 是 0,而 `timer_handle` 拿 `rip != 0` 当"上下文有效"的守卫
 * (`scheduler.cpp:444`)—— 挑到它时**什么都不做**,启动流程继续跑。
 * 这就是 idle 的执行方式:没有"切进 idle"这回事。
 *
 * ⚠ 第一版我造了一个真的 `for(;;) wfi()` idle 线程 + `sched_kern_start()` 交棒。
 *   那是**偏离源 OS** 的,而且立刻推出一堆不存在的问题:
 *   "协作式让出会把当前线程重新入队 ⇒ 队列永不为空 ⇒ 挑不到 idle ⇒ 出不来"。
 *   源 OS 里 idle 永远是 current,压根不需要"进去"。
 *
 * ARM 侧的"上下文无效"标记同样用 `ctx.pc == 0`(源 OS 用 `context0.rip`)。
 * `ctx.sp` 取**当前** sp —— 现场就在 CPU 里,不需要保存,
 * 因为这台"线程"只被切**走**,从不会被切**回来**…
 * 需要被切回来时,x86 的 `change_proccess` 会把改写后的帧交回 iretq,
 * ARM 侧对应的是改写现场帧再 rfeia(M4-9)。
 */
void sched_register_boot_idle(void)
{
    u32 i;

    memset(&g_boot_idle_tcb, 0, sizeof(g_boot_idle_tcb));

    g_boot_idle_tcb.task_level   = TASK_IDLE_LEVEL;
    g_boot_idle_tcb.status       = RUNNING;
    g_boot_idle_tcb.kernel_stack = arch_read_sp(); /* 当前 sp,不另取栈 */
    g_boot_idle_tcb.ctx.sp       = arch_read_sp();
    g_boot_idle_tcb.ctx.pc       = 0u; /* ★ 0 = 上下文无效,调度器不许切进来 ★ */
    g_boot_idle_tcb.owns_kstack  = false; /* 它就是启动栈,不归栈池管 */
    for (i = 0; i < sizeof(g_boot_idle_tcb.name) - 1u && "idle"[i] != '\0'; i++) {
        g_boot_idle_tcb.name[i] = "idle"[i];
    }
    sched_entity_init(&g_boot_idle_tcb, 0u);

    g_boot_idle = &g_boot_idle_tcb;
    sched_set_current(g_boot_idle);
}

tcb_t sched_boot_idle(void)
{
    return g_boot_idle;
}

/* ------------------------------------------------------------------ */
/* tick 里给 current 计费(照源 OS)                                    */
/* ------------------------------------------------------------------ */

/*
 * ← `timer_handle()` `scheduler.cpp:437`:
 *      charge_current_eevdf_runtime(current, EEVDF_TICK_NS);
 *
 * **这是 M4-8 里唯一会让策略在板上"活起来"的地方**,而且它不做任何切换 ——
 * 切换是 M4-9 的事。所以本步的板级判据可以是:
 * 跑 N 个 tick 之后,current(启动上下文)的 vruntime 正好涨了 N 毫秒。
 *
 * ⚠ 只在 current 确实在跑、且不是 idle 时才计费 —— 与源 OS 的三条早退一致
 *   (idle / 非 RUNNING / runtime 为 0),那些早退在 sched_account_run 里。
 */
void sched_tick_account(void)
{
    tcb_t cur = sched_current();

    if (cur == NULL) {
        return;
    }

    sched_account_run(cur, SCHED_TICK_NS);
}

/* (sched_tick / sched_tick_would_switch / sched_tick_invalid_ctx
 *  实现在文件末尾 —— 它们是完整的决策路径,而本函数只是其中的计费部分。) */

/* ------------------------------------------------------------------ */
/* M4-9:tick 里的调度决策                                              */
/* ------------------------------------------------------------------ */

static u32 g_tick_would_switch;
static u32 g_tick_invalid_ctx;

/*
 * ← `timer_handle()` `scheduler.cpp:430-470`。逐段对应:
 *
 *   if (current 在跑 且 不是 idle) { scheduler_ticks++;
 *                                    charge_current_eevdf_runtime(current, TICK_NS);
 *                                    if (scheduler_ticks < TIME_SLICE) return; }
 *   else scheduler_ticks = 0;
 *
 *   best = select_next_task();
 *   if (best == NULL || best == current) { scheduler_ticks = 0; return; }
 *   if (best->context0.rip != 0) 换;  else 什么都不做;
 *
 * ⚠ 时间片计数放在**每核**结构里(源 OS 用 `cpu->scheduler_ticks`):
 *   两个核各跑各的 tick,共用一个静态变量会让两核互相把对方的时间片清零 ——
 *   表现为"抢占几乎不发生",而且只在双核下出现。
 */
arm_exc_frame_t *sched_tick(arm_exc_frame_t *frame)
{
    sched_queue_t *q;
    tcb_t          cur;
    tcb_t          next;
    percpu_t      *pc;

    if (frame == NULL) {
        return frame;
    }

    pc = percpu_self();
    if (pc == NULL) {
        return frame;
    }

    q   = &g_runq[pc->cpu_id];
    cur = sched_current();

    /* ---- 1. 计费 + 时间片 ---- */
    if (cur != NULL && cur->status == RUNNING && cur->task_level != TASK_IDLE_LEVEL) {
        pc->scheduler_ticks++;
        sched_account_run(cur, SCHED_TICK_NS);

        if (pc->scheduler_ticks < SCHED_TIME_SLICE) {
            return frame; /* 时间片没到 */
        }
    } else {
        pc->scheduler_ticks = 0u;
    }

    /* ---- 2. 挑下一个 ---- */
    next = sched_pick(q);
    if (next == NULL || next == cur) {
        pc->scheduler_ticks = 0u;
        return frame;
    }

    /* ---- 3. ★ 上下文有效才切换 ★(源 OS:`context0.rip != 0`)*/
    if (next->ctx.pc == 0u) {
        /*
         * 上下文无效 —— 源 OS 在这里**什么都不做**,并把 current 的状态
         * 恢复成 RUNNING。这正是 idle 的执行方式:启动上下文没有可恢复的
         * 现场,所以"挑到它"等于"不切换"。
         */
        pc->scheduler_ticks = 0u;
        g_tick_invalid_ctx++;
        if (cur != NULL && cur->status != RUNNING) {
            cur->status = RUNNING;
        }
        return frame;
    }

    /*
     * ---- 4. 到这里"本来应该切换" ----
     *
     * ⚠ 本步**只记数、不搬帧**:先让决策路径每 tick 跑起来并被观测,
     *   确认"该不该切"是对的,再让它动帧。
     *   "该不该切"错 与 "搬帧搬错" 是两类完全不同的错误,混在一起没法归因 ——
     *   这个项目已经因为"把两位信息压成一位"多花过一整轮上板时间。
     */
    g_tick_would_switch++;
    pc->scheduler_ticks = 0u;

    return frame;
}

u32 sched_tick_would_switch(void)
{
    return g_tick_would_switch;
}

u32 sched_tick_invalid_ctx(void)
{
    return g_tick_invalid_ctx;
}
