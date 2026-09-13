/*
 * 调度策略 —— 纯逻辑实现(M4-8.2)
 *
 * 本文件**不含 MMIO / CP15 / 内联汇编**,时间以纳秒参数传进来,
 * 所以宿主机能直接编译它并穷尽测策略 —— 那是策略的**唯一**判据
 * (板级证据只能证明机制对)。设计说明见 include/arch/sched.h。
 */

#include <arch/sched.h>
#include <krlibc.h>

/* ------------------------------------------------------------------ */
/* ★ 破坏性 A/B:跳过扫描里的唤醒 ★                                    */
/* ------------------------------------------------------------------ */

/*
 * 置 1 时 `sched_queue_scan()` 仍会遍历队列,但**不唤醒**到点的睡眠任务。
 *
 * 这是"唤醒发生在扫描里"这件事的对照组:关掉它,睡下去的任务就
 * **永远醒不过来** —— 而"取队首"的那一版 M4-8 本来就等于一直关着它。
 *
 * ⚠ 定义在**纯逻辑层自己这里**,不在 `sched_kern.c`:
 *   `sched_queue_scan` 是宿主可测的,宿主编译单元里必须能解析这个符号,
 *   否则宿主单测链接不过。生产路径上恒为 0。
 */
u32 g_wake_skip;

/* ------------------------------------------------------------------ */
/* 策略原语                                                            */
/* ------------------------------------------------------------------ */

u64 sched_slice(tcb_t t)
{
    /*
     * ← `task_sched_slice()` `scheduler.cpp:199`
     * slice 没设或小于下限时回落到基础片长。注意是 **< 下限**就回落,
     * 不是夹到区间里 —— 源 OS 就是这么写的,照抄。
     */
    if (t == NULL || t->eevdf_slice < SCHED_MIN_SLICE_NS) {
        return SCHED_BASE_SLICE_NS;
    }

    return t->eevdf_slice;
}

u64 sched_vruntime_delta(u64 runtime_ns)
{
    /*
     * ← `vruntime_delta()` `scheduler.cpp:232`
     *
     * ⚠ 源 OS 写的是 `(runtime_ns * SCHED_DEFAULT_WEIGHT) / SCHED_DEFAULT_WEIGHT`,
     *   **乘除同一个常数,恒等于原值**。看起来是它没做完的地方(TCB 里
     *   根本没有权重字段)。这里**照抄这个恒等式**,不擅自"补全":
     *   一旦这边引入权重,同样的输入会算出与 x86 不同的 vruntime,
     *   两边的调度行为就分叉了,而那正是"源 OS 怎么做才是"要避免的。
     *   已记入退化清单。
     */
    return (runtime_ns * SCHED_DEFAULT_WEIGHT) / SCHED_DEFAULT_WEIGHT;
}

bool sched_deadline_before(tcb_t left, tcb_t right)
{
    /* ← `deadline_before()` `scheduler.cpp:237`,NULL 语义一并照抄 */
    if (right == NULL) {
        return true;
    }
    if (left == NULL) {
        return false;
    }

    if (left->eevdf_deadline != right->eevdf_deadline) {
        return left->eevdf_deadline < right->eevdf_deadline;
    }

    return left->eevdf_vruntime < right->eevdf_vruntime;
}

void sched_account_run(tcb_t t, u64 runtime_ns)
{
    /* ← `charge_current_eevdf_runtime()` `scheduler.cpp:245` */
    if (t == NULL || t->task_level == TASK_IDLE_LEVEL) {
        return;
    }
    if (t->status != RUNNING) {
        return;
    }
    if (runtime_ns == 0u) {
        return;
    }

    t->eevdf_vruntime += sched_vruntime_delta(runtime_ns);
    t->eevdf_deadline = t->eevdf_vruntime + sched_slice(t);
}

void sched_apply_wakeup_credit(tcb_t t, u64 base_vruntime)
{
    /* ← `apply_eevdf_wakeup_credit()` `scheduler.cpp:205` */
    u64 credit;
    u64 placed;

    if (t == NULL || t->task_level == TASK_IDLE_LEVEL) {
        return;
    }

    /*
     * 补偿量是"该任务自己的片长",但夹在 [WAKEUP_CREDIT, SLEEPER_CREDIT] 之间。
     * 也就是:睡醒的任务最多被往前拨 8ms —— 这是源 OS 里**唯一有"策略味道"
     * 的行为**,所以 M4-8 的真活(1Hz 状态行)专门选了一个会睡的负载来验它。
     */
    credit = sched_slice(t);
    if (credit < SCHED_WAKEUP_CREDIT) {
        credit = SCHED_WAKEUP_CREDIT;
    }
    if (credit > SCHED_SLEEPER_CREDIT) {
        credit = SCHED_SLEEPER_CREDIT;
    }

    placed = (base_vruntime > credit) ? (base_vruntime - credit) : 0u;

    /* ★ 只往前拨,不往后拉 ★ —— 少了这个判断,睡醒的任务反而会被惩罚 */
    if (t->eevdf_vruntime > placed) {
        t->eevdf_vruntime = placed;
    }

    t->eevdf_deadline = t->eevdf_vruntime + sched_slice(t);
}

bool sched_wake_if_due(tcb_t t, u64 now, u64 base_vruntime)
{
    /* ← `wake_sleeping_task()` `scheduler.cpp:220` */
    if (t == NULL) {
        return false;
    }

    /* wakeup_time == 0 表示"不按时间唤醒",不是"立即唤醒" */
    if (t->status == WAIT && t->wakeup_time != 0u && now >= t->wakeup_time) {
        t->wakeup_time = 0u;
        t->status      = START;
        sched_apply_wakeup_credit(t, base_vruntime);
        return true;
    }

    return false;
}

void sched_entity_init(tcb_t t, u64 now)
{
    if (t == NULL) {
        return;
    }

    /*
     * 新线程的 vruntime 从"当前时刻"起步,而不是从 0。
     *
     * 为什么:如果新线程从 0 开始,而老线程已经累积到很大的 vruntime,
     * 新线程的 deadline 会小得离谱 —— 它会独占 CPU 直到追上别人。
     * 源 OS 用 `init_task_eevdf_entity(task, queue_average_vruntime(...), now)`
     * 表达同一件事:以队列的平均 vruntime 为起点。这里由调用方传 now,
     * 保持函数是纯的。
     */
    t->eevdf_vruntime    = now;
    t->eevdf_slice       = SCHED_BASE_SLICE_NS;
    t->eevdf_deadline    = t->eevdf_vruntime + sched_slice(t);
    t->eevdf_last_start  = now;
    t->runtime_ticks     = 0u;
    t->sched_next        = NULL;
}

/* ------------------------------------------------------------------ */
/* 就绪队列                                                            */
/* ------------------------------------------------------------------ */

void sched_queue_init(sched_queue_t *q)
{
    if (q == NULL) {
        return;
    }

    q->head  = NULL;
    q->count = 0u;
}

bool sched_queue_append(sched_queue_t *q, tcb_t t)
{
    tcb_t *link;

    if (q == NULL || t == NULL) {
        return false;
    }

    /*
     * ← `queue_enqueue()` → `queue_append_node()`(`kernel/lock_queue.cpp:100`):
     *   **追加到尾部,不排序。**
     *
     * ⚠ M4-8 第一版这里是"按 deadline 有序插入,于是队首就是应选者"。
     *   那是我发明的:源 OS 的队列是普通 FIFO,**选取靠全表扫描**
     *   (`select_next_task_safe`)。排序版不只是"不一样",它还有直接后果 ——
     *   唤醒睡眠任务的那次扫描在 `select_next_task_safe` 里,而"取队首"
     *   的写法里没有那次扫描,于是睡下去的任务永远醒不过来。
     *
     * ⚠ 查重仍然保留,而且比源 OS 严格:挂两次会把链表做成环,
     *   之后任何一次遍历都无限循环(表现是"某个核突然不动了")。
     */
    for (link = &q->head; *link != NULL; link = &(*link)->sched_next) {
        if (*link == t) {
            return false;
        }
    }

    *link         = t;
    t->sched_next = NULL;
    q->count++;

    return true;
}

void sched_queue_remove(sched_queue_t *q, tcb_t t)
{
    tcb_t *link;

    if (q == NULL || t == NULL) {
        return;
    }

    for (link = &q->head; *link != NULL; link = &(*link)->sched_next) {
        if (*link == t) {
            *link = t->sched_next;
            t->sched_next = NULL;
            if (q->count > 0u) {
                q->count--;
            }
            return;
        }
    }
}

u64 sched_queue_avg_vruntime(const sched_queue_t *q, tcb_t ignore, u64 fallback)
{
    /*
     * 只算平均、**不唤醒**的只读版本。
     *
     * 源 OS 没有这个函数:`queue_average_vruntime` 一定会顺带唤醒。
     * 这里留一个纯读的版本,是给两处"不该有副作用"的场景用的:
     *   - 宿主单测里核对平均值本身;
     *   - `sched_kthread_create()` 给新线程定 vruntime 起点。
     *     (源 OS 在 `add_task()` 里调的也是 `queue_average_vruntime`,
     *      职责是 `queue_average_vruntime(..., NULL, now, NULL, NULL)` ——
     *      传 NULL 的 fallback/idle,于是只取平均值那一路。)
     */
    const tcb_t *link;
    u64          sum   = 0u;
    u64          count = 0u;

    if (q == NULL) {
        return fallback;
    }

    for (link = &q->head; *link != NULL; link = &(*link)->sched_next) {
        if (*link == ignore) {
            continue;
        }
        sum += (*link)->eevdf_vruntime;
        count++;
    }

    return (count == 0u) ? fallback : (sum / count);
}

u64 sched_queue_scan(const sched_queue_t *q, tcb_t current, u64 now, tcb_t *out_fallback, tcb_t *out_idle)
{
    /* ← `queue_average_vruntime()` `scheduler.cpp:256-287`,逐句对应 */
    const tcb_t *link;
    u64          sum   = 0u;
    u64          count = 0u;

    if (out_fallback != NULL) {
        *out_fallback = NULL;
    }
    if (out_idle != NULL) {
        *out_idle = NULL;
    }
    if (q == NULL) {
        return 0u;
    }

    for (link = &q->head; *link != NULL; link = &(*link)->sched_next) {
        tcb_t candidate = *link;
        /*
         * 睡醒补偿的基准:有 current 就用 current 的 vruntime,
         * 否则用候选自己的(源 OS 就是这个三元表达式)。
         */
        u64 wake_base = (current != NULL) ? current->eevdf_vruntime : candidate->eevdf_vruntime;

        /* ★ 唤醒就发生在这里 ★ —— 遍历到谁就给谁一次机会 */
        if (g_wake_skip == 0u) {
            (void)sched_wake_if_due(candidate, now, wake_base);
        }

        if (candidate != current && candidate->task_level == TASK_IDLE_LEVEL && out_idle != NULL) {
            *out_idle = candidate;
        }

        if (candidate == current) {
            /*
             * current 在队列里(源 OS 就是这样),它算不算进平均取决于
             * "它现在还能不能跑" —— 注意这里用的是 `sched_current_runnable`,
             * 它对 idle **不**早退,与候选那一路刻意不对称。
             */
            if (sched_current_runnable(candidate)) {
                sum += candidate->eevdf_vruntime;
                count++;
            }
        } else if (sched_task_schedulable(candidate, current)) {
            sum += candidate->eevdf_vruntime;
            count++;
            if (out_fallback != NULL && sched_deadline_before(candidate, *out_fallback)) {
                *out_fallback = candidate;
            }
        }
    }

    return (count == 0u) ? 0u : (sum / count);
}

void sched_mark_dispatched(tcb_t t, u64 now)
{
    /* ← `mark_task_dispatched()` `scheduler.cpp:304-314` */
    if (t == NULL) {
        return;
    }

    if (t->eevdf_slice < SCHED_MIN_SLICE_NS) {
        t->eevdf_slice = SCHED_BASE_SLICE_NS;
    }
    /*
     * ⚠ 只在 deadline 为 0 时才补算。这不是"防御性编程":deadline 为 0
     *   表示"还没被算过"(新任务),而已有的 deadline 是**上次计费**的结果,
     *   无条件重算会把它抹掉 —— 那会让公平性静默失真。
     */
    if (t->eevdf_deadline == 0u) {
        t->eevdf_deadline = t->eevdf_vruntime + sched_slice(t);
    }
    t->eevdf_last_start = now;
}

tcb_t sched_select_next(const sched_queue_t *q, tcb_t current, u64 now)
{
    /* ← `select_next_task_safe()` `scheduler.cpp:316-362`
     *   + `select_next_task()` `scheduler.cpp:364-378`,两段合成一个函数 */
    tcb_t          fallback = NULL;
    tcb_t          idle     = NULL;
    tcb_t          best     = NULL;
    tcb_t          result;
    u64            avg;
    const tcb_t   *link;

    if (q == NULL || q->head == NULL) {
        /*
         * 源 OS 在这一支直接 return NULL(队列空),然后由 `select_next_task()`
         * 兜成 current。这里合成一处,免得调用方记两条规则。
         */
        if (current != NULL && (current->status == RUNNING || current->status == START)) {
            return current;
        }
        return NULL;
    }

    /* 1. current 自己也到点就唤醒(源 OS 在扫描**之前**单独做这一次) */
    (void)sched_wake_if_due(current, now, (current != NULL) ? current->eevdf_vruntime : 0u);

    /* 2. 扫描:唤醒 + 平均值 + fallback + idle */
    avg = sched_queue_scan(q, current, now, &fallback, &idle);

    /* 3. 闸门:fallback 达标就用它(源 OS `scheduler.cpp:340`) */
    if (fallback != NULL && fallback->eevdf_vruntime <= avg) {
        best = fallback;
    }

    /*
     * 4. fallback 不达标时,在**合格者**里取 deadline 最小的
     *    (源 OS `scheduler.cpp:344-354`)。
     *
     * ⚠ 源 OS 这里有 `if (best == NULL && fallback != NULL)` 这一层额外条件;
     *   照抄。它的含义是"只有在有 fallback 时才做第二遍扫描"——
     *   队列里没有任何可调度候选时,扫也扫不出东西。
     */
    if (best == NULL && fallback != NULL) {
        for (link = &q->head; *link != NULL; link = &(*link)->sched_next) {
            tcb_t candidate = *link;

            if (sched_task_schedulable(candidate, current) && candidate->eevdf_vruntime <= avg &&
                sched_deadline_before(candidate, best)) {
                best = candidate;
            }
        }
    }

    if (best == NULL) {
        best = fallback;
    }

    /* 5. 兜底链:best → current(可运行)→ idle(源 OS `scheduler.cpp:358`) */
    result = (best != NULL) ? best : (sched_current_runnable(current) ? current : idle);

    if (result != NULL) {
        sched_mark_dispatched(result, now);
    }

    return result;
}

/* ------------------------------------------------------------------ */
/* 可调度性判据 ← `is_task_schedulable` / `is_current_task_runnable`    */
/* ------------------------------------------------------------------ */

bool sched_status_runnable(TaskStatus st)
{
    /* ← `scheduler.cpp:177` / `:190` 的那个三选一 */
    return (st == RUNNING) || (st == START) || (st == CREATE);
}

bool sched_task_schedulable(tcb_t t, tcb_t current)
{
    /*
     * ← `is_task_schedulable()` `scheduler.cpp:173-185`,逐条对应。
     * 省掉的两条(parent_group)与理由写在头文件里 —— 不是忘了。
     */
    if (t == NULL) {
        return false;
    }
    if (t == current) {
        return false;
    }
    if (!sched_status_runnable(t->status)) {
        return false;
    }
    if (t->task_level == TASK_IDLE_LEVEL) {
        return false;
    }

    return true;
}

bool sched_current_runnable(tcb_t t)
{
    /* ← `is_current_task_runnable()` `scheduler.cpp:187-197`。
     * ⚠ 与 sched_task_schedulable 的差别不是笔误:这里**不排除 idle**、
     *   也**不排除自己** —— 源 OS 就是不对称的,照抄。 */
    if (t == NULL) {
        return false;
    }

    return sched_status_runnable(t->status);
}

/* (`sched_pick_next` 在这里 —— 它是 M4-9 中间的产物:"从队首往后找第一个
 *  可调度的"。M4-8.4 对齐源 OS 之后它没有调用者了:`sched_select_next`
 *  做的是**全表扫描 + avg_vruntime 闸门**,不是"取第一个合格的"。
 *  删掉而不是留着 —— "声明与定义都在、就是没人调"会让人以为功能还在。) */

/* ------------------------------------------------------------------ */
/* ★ M4-9:帧搬迁 ★                                                    */
/* ------------------------------------------------------------------ */

bool sched_ctx_switchable(const tcb_t t)
{
    u32 sp;

    if (t == NULL) {
        return false;
    }

    /* 1. "上下文无效"标记 —— 启动上下文在被第一次切走之前就是这样 */
    if (t->ctx.pc == 0u) {
        return false;
    }

    /*
     * 2. ★ CPSR 的 A 位必须是 1,模式位必须是 SVC ★
     *
     * 这一条是绊线,而且**真的抓到过两次**(两次都是我自己写错的,
     * 两次都是同一类:把"应该是这样"当成"就是这样"):
     *
     *   (a) 造新线程时这里原本写的是 `0`(一个未分配的模式编码,UB);
     *   (b) 我先改成 `ARM_MODE_SVC | ARM_CPSR_F_BIT`(0x53)—— 模式对了,
     *       但 **A 位(bit8,异步中止屏蔽)是 0**,而内核实际跑在 0x153。
     *       `msr cpsr_c` 碰不到 bit8,所以 A 从复位起一直是 1;
     *       我把它写成 0,等于切过去的那一刻解除了异步外部中止的屏蔽,
     *       于是第一条指令就吃了 FS=0x16 的异步中止,而 DFAR 是 meaningless
     *       的,排查方向被带偏了一整轮。
     *
     * ⚠ 这里**不查 I 位**(可以合法地是 1:`svc` 陷阱能在关中断的临界区里
     *   发生,而那个值必须原样还回去),**也不查 T 位** —— 内核会合法地跑在
     *   Thumb 态里(libgcc 的 `__udivmoddi4` 就是 Thumb,而 `timer_read_us()`
     *   的 64 位除法会进到它里面)。第一版把 T 也纳入判定,结果拒绝了每一个
     *   这样的上下文,`invalid_ctx` 涨到 45965。
     *
     * 完整经过写在 arch/taskctx.h 的 `ARM_CPSR_KERNEL` 与
     * `ARM_CPSR_MUST_MATCH` 上面。
     */
    if (arm_cpsr_must(t->ctx.cpsr) != arm_cpsr_must(ARM_CPSR_KERNEL)) {
        return false;
    }

    /*
     * 3. ★ SP 只需 **4 字节**对齐,不是 8 ★
     *
     * 这一条也是上板打回来的 —— 同一类错误的**第四次**:我按"栈指针当然
     * 是 8 字节对齐的"写了检查,而板上的真值是 `0x0299FFA4`(4 mod 8)。
     *
     * 为什么 4 mod 8 是**正常**的:被中断的那条指令在 `__udivmoddi4` 里,
     * 那是 libgcc 的 **Thumb** 代码,它的序言是 `push {r4,r5,lr}` ——
     * **12 字节**,于是函数体里 SP 就是 4 mod 8。
     * AAPCS 只要求"**在调用公开接口的那一刻** SP 8 字节对齐",
     * 函数体内部可以 4 字节对齐;而异常可以在**任意指令边界**发生。
     *
     * 帧本身需要的只是 4 字节对齐:它的 16 个槽全是 u32。
     * ARMv7-A 上 `ldr/str` 4 字节、`ldrd/strd` 4 字节、VFP 传输 4 字节,
     * 所以按 4 对齐搬这个帧没有任何问题。
     *
     * ⚠ 仍然存疑、并已记入待办的一点:`bl c_xxx_handler` 之后 C 处理函数
     *   就在一个 4 mod 8 的 SP 上跑,严格来说违反 AAPCS 的"公开接口处
     *   8 字节对齐"。本板实测它扛得住(被拒绝的那一轮里 `sched_tick`
     *   在这个状态下执行了 3 万多次而没有出错),但这属于"没观察到问题",
     *   不是"证明没问题"。真要根治得让异常入口把帧对齐并把**原始 SP**
     *   存进帧里 —— 那是加宽帧(16 → 18 字)的改动,不该顺手做。
     */
    sp = t->ctx.sp;
    if (sp == 0u || (sp & 3u) != 0u) {
        return false;
    }

    /*
     * 4. 栈必须落在**这个任务自己的**栈区里。
     *
     * ⚠ `owns_kstack == false`(启动上下文)时**不做范围检查**:
     *   那两个边界(`kstack_base` / `kernel_stack`)是注册 idle 那一刻的
     *   快照,而启动流程在任何深度都可能被中断 —— 拿快照当区间会把
     *   "kmain 后来跑到更浅的调用深度"误判成"栈指针是野的"。
     *   代价是启动上下文的 sp 只做了对齐检查;真正的越界会在
     *   guard page 上以 Data Abort 的形式出现,而不是静默。
     */
    if (!t->owns_kstack) {
        return true;
    }

    if (sp <= t->kstack_base || sp > t->kernel_stack) {
        return false;
    }

    /* 帧整体(64 字节)也得在栈区里,不能有一半落进 guard 页 */
    if (sp - ARM_EXC_FRAME_BYTES < t->kstack_base) {
        return false;
    }

    return true;
}

void sched_ctx_from_frame(tcb_t t, const arm_exc_frame_t *f)
{
    u32 i;

    if (t == NULL || f == NULL) {
        return;
    }

    for (i = 0; i < 13u; i++) {
        t->ctx.r[i] = f->r[i];
    }

    /*
     * ★ 这一行是"帧在谁的栈上,谁就被恢复"的全部内容 ★
     *
     * 帧基址 + 64 == 异常入口那一刻的 sp(`srsdb` -8、`push lr` -4、
     * `push {r0-r12}` -52),而 `EXC_FRAME_LEAVE` 的 `add sp,#0x38; rfeia sp!`
     * 正好把 sp 还原成"帧基址 + 64"。于是存下这个值,下次就回到同一段栈。
     */
    t->ctx.sp   = (u32)(uintptr_t)f + ARM_EXC_FRAME_BYTES;
    t->ctx.lr   = f->svc_lr;
    t->ctx.pc   = f->ret;
    t->ctx.cpsr = f->spsr;
}

arm_exc_frame_t *sched_frame_for(tcb_t t)
{
    if (!sched_ctx_switchable(t)) {
        return NULL;
    }

    return (arm_exc_frame_t *)(uintptr_t)(t->ctx.sp - ARM_EXC_FRAME_BYTES);
}

arm_exc_frame_t *sched_frame_from_ctx(tcb_t t, arm_exc_frame_t *dst)
{
    u32 i;

    if (t == NULL || dst == NULL) {
        return NULL;
    }

    for (i = 0; i < 13u; i++) {
        dst->r[i] = t->ctx.r[i];
    }
    dst->svc_lr = t->ctx.lr;
    dst->ret    = t->ctx.pc;
    dst->spsr   = t->ctx.cpsr;

    return dst;
}

bool sched_cpsr_matches_kernel(u32 cpsr)
{
    /*
     * 与 `ARM_CPSR_KERNEL` 在"A / T / 模式位"上一致吗 ——
     * 也就是"这个 CPSR 描述的是内核正常运行的状态吗"。
     *
     * ★ 存在的理由:那个常量是**手写的**,而手写的东西必须与真实对一次账。
     *   判据不是"我认为内核跑在 0x153",而是"**收现场时真的读到的那个值**
     *   满足同一条谓词" —— 后者是在板上量出来的,不是推出来的。
     *
     *   M4-9 就是因为在这一点上只做了前者(而且只推理了 I/F 两位),
     *   把 A 位写成了 0,白跑了一整轮上板。完整经过见 arch/taskctx.h。
     *
     * 不算 I:见 ARM_CPSR_MUST_MATCH 的说明。
     */
    return arm_cpsr_must(cpsr) == arm_cpsr_must(ARM_CPSR_KERNEL);
}
