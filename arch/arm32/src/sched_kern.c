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
static u32 g_idle_spins;

/* idle 线程的 TCB 与栈。静态实例:它必须永远存在,且不该依赖分配器 */
static struct arm_thread_control_block g_idle_tcb;
static kstack_t                        g_idle_stack;
static tcb_t                           g_idle_task;

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
    g_idle_spins   = 0u;
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
    if (cur != NULL && cur != g_idle_task) {
        cur->status = RUNNING;
        sched_account_run(cur, SCHED_BASE_SLICE_NS);
    }

    next = sched_pick(q);
    if (next == NULL) {
        /* 没有可运行线程 -> idle。已在跑 idle 就什么都不做 */
        next = g_idle_task;
    }
    if (next == NULL || next == cur) {
        return;
    }

    /* ---- 把当前线程放回队列,再把下一个摘下来 ---- */
    if (cur != NULL && cur != g_idle_task) {
        cur->status = START;
        (void)sched_queue_insert(q, cur);
    }

    if (next != g_idle_task) {
        sched_queue_remove(q, next);
        next->status = RUNNING;
    } else {
        g_idle_spins++;
    }

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
    if (cur == NULL) {
        arch_ctx_switch(&g_boot_ctx, &next->ctx);
    } else {
        arch_ctx_switch(&cur->ctx, &next->ctx);
    }
}

u32 sched_switch_count(void)
{
    return g_switch_count;
}

u32 sched_idle_spins(void)
{
    return g_idle_spins;
}

/* ------------------------------------------------------------------ */
/* idle                                                                */
/* ------------------------------------------------------------------ */

/*
 * idle 线程:没有别的事可做时 CPU 在这里等中断。
 *
 * 为什么必须有一个真的 idle 线程,而不是"挑不到就让调度器空转":
 *   - 空转会让 CPU 一直满速跑,板上直接表现为发热与功耗;
 *   - 更要紧的是**它没有栈** —— 协作式切换总得切到**某个**上下文上,
 *     而"没有上下文"是没法切过去的。
 */
static void idle_main(void *arg)
{
    (void)arg;

    for (;;) {
        /*
         * ⚠ 自旋计数放在 WFI **之前**:它是"idle 被调度到几次"的计数,
         *   不是"空转了几圈"。idle 每次被切进来只加一次 ——
         *   这样它与 sched_switch_count 是同一量纲,可以直接对比。
         */
        arch_wfi();
    }
}

void sched_kern_start(void)
{
    u32 i;

    memset(&g_idle_tcb, 0, sizeof(g_idle_tcb));

    /*
     * idle 的栈:从栈池取,而不是复用启动栈。
     *
     * 复用启动栈会有一个隐蔽后果:kmain 的栈帧还在上面,
     * 而 idle 永远不返回 —— 一旦将来有人让 idle 调用任何东西,
     * 它就在 kmain 的栈帧上往下长,把 kmain 还在用的局部变量踩掉。
     * (M4-7 的对照组正是踩坏了 kmain 的三个局部变量,那次是有意的。)
     */
    if (g_ks == NULL || kstack_alloc(g_ks, &g_idle_stack) != KSTACK_OK) {
        /*
         * 拿不到栈就不能切过去 —— 那会切到一个野 sp 上。
         * 如实报出来并留在当前上下文里跑(退化但不崩)。
         */
        console_puts(" Sched       : idle stack alloc FAILED, scheduling disabled\n");
        return;
    }

    g_idle_tcb.task_level   = TASK_IDLE_LEVEL;
    g_idle_tcb.status       = RUNNING;
    g_idle_tcb.kernel_stack = g_idle_stack.top;
    g_idle_tcb.kstack_base  = g_idle_stack.base;
    g_idle_tcb.kstack_guard = g_idle_stack.guard;
    g_idle_tcb.kstack_slot  = g_idle_stack.slot;
    g_idle_tcb.owns_kstack  = true;
    for (i = 0; i < sizeof(g_idle_tcb.name) - 1u && "idle"[i] != '\0'; i++) {
        g_idle_tcb.name[i] = "idle"[i];
    }
    g_idle_tcb.ctx.sp = g_idle_stack.top;
    g_idle_tcb.ctx.pc = (u32)(uintptr_t)idle_main;
    sched_entity_init(&g_idle_tcb, 0u);

    g_idle_task = &g_idle_tcb;

    console_printf(" Sched       : idle stack=0x%08X top=0x%08X guard=0x%08X\n", g_idle_stack.base,
                   g_idle_stack.top, g_idle_stack.guard);

    /*
     * ---- 交棒 ----
     *
     * 把 current 置空(此刻**没有**线程在跑,跑的是启动流程),
     * 然后走一次普通的让出:它会挑出 deadline 最小的可运行者
     * (没有就挑 idle),并从**丢弃槽**切过去。
     *
     * ⚠ 正常情况下这一句不返回:启动上下文被存进 g_boot_ctx 之后就
     *   永远躺在那里了 —— 它没有 TCB,也没人会把它放回队列。
     *   真返回了说明有人把 g_boot_ctx 当成线程切了回来,那是本模块被用错,
     *   所以下面停在 WFI 里而不是继续跑启动流程(那会是一个栈已经不一致的 kmain)。
     */
    sched_set_current(NULL);
    sched_yield();

    for (;;) {
        arch_wfi();
    }
}
