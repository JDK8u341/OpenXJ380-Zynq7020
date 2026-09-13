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

void sched_disable(void)
{
    g_sched_enabled = 0u;
}

void sched_enable(void)
{
    g_sched_enabled = 1u;
}

bool sched_is_enabled(void)
{
    return g_sched_enabled != 0u;
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
 * ★ 破坏性 A/B 的对照组开关 ★ —— 定义与理由见 include/arch/sched.h。
 * 生产路径上恒为 0;只有 kmain 在自检报告**之后**才会短暂置 1。
 */
u32 g_reloc_skip;

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

/* (sched_tick / sched_tick_switched / sched_tick_preempted / sched_tick_invalid_ctx
 *  实现在文件末尾 —— 它们是完整的决策路径,而本函数只是其中的计费部分。) */

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
 * ## 与源 OS 的三处**结构**差异(每一处都写清楚为什么)
 *
 * 1. **顺序:先判可切换性,再动状态。**
 *    源 OS 先把 current/best 的状态改掉,发现 `rip == 0` 再改回来。
 *    这里把判断提前 —— 因为本移植还要挪**队列成员关系**(current 在跑时
 *    不在队列里、就绪时才在),"改完再回滚"要回滚两样东西,
 *    而回滚漏一半是静默的。可观测行为完全一致。
 *
 * 2. **队列成员关系由本函数维护。**
 *    源 OS 的 current 一直留在队列里,靠 `is_task_schedulable` 排除。
 *    这里用"跑着的不在队列里"表达同一件事(见 include/arch/sched.h 的说明),
 *    于是切换时必须一进一出。
 *
 * 3. **idle 不进队列**,用显式兜底代替源 OS 的 `idle` 出参。
 *    理由见 `sched_register_boot_idle()`。
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

        if (pc->scheduler_ticks < SCHED_TIME_SLICE) {
            return frame; /* 时间片没到 */
        }
    } else {
        pc->scheduler_ticks = 0u;
    }

    /* ---- 2. 挑下一个 ---- */
    next = sched_pick_next(q, cur);
    if (next == NULL) {
        /*
         * ← `select_next_task_safe()` 的最后一行
         *   (`scheduler.cpp:358`):
         *       result = best ? best : (is_current_task_runnable(current) ? current : idle);
         *
         * 队列里没有可调度的别人时:current 还能跑就继续跑它,
         * 否则用 idle 兜底。**这两个分支都要有** ——
         * 少了后一个,探针线程全部挂起之后就没有任何东西能让启动流程回来。
         */
        next = sched_current_runnable(cur) ? cur : g_boot_idle;
    }

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

    /* ---- 4. 状态与队列(← `timer_handle:436-441`)---- */
    if (cur->status == RUNNING) {
        cur->status = START;
        /*
         * ⚠ idle 不重新入队:它的 vruntime 永远是 0(计费对它早退),
         *   于是 deadline 恒为最小 —— 一旦进了队列,`sched_pick_next`
         *   每次都先挑到它,真实线程就再也拿不到 CPU。
         *   它的"回来"由第 2 步的兜底负责。
         */
        if (cur->task_level != TASK_IDLE_LEVEL) {
            (void)sched_queue_insert(q, cur);
        }
    }
    /* WAIT / 已挂起的线程**不入队** —— 那正是 sched_park_self 的用处 */

    if (next->status == START || next->status == CREATE) {
        next->status = RUNNING;
    }
    sched_queue_remove(q, next);

    /* ---- 5. ★★★ 搬帧 ★★★ ---- */
    /*
     * 5a. 把 current 的现场收进它的 ctx。
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
     */
    if (g_reloc_skip != 0u) {
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
