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

bool sched_queue_insert(sched_queue_t *q, tcb_t t)
{
    tcb_t *link;

    if (q == NULL || t == NULL) {
        return false;
    }

    /*
     * ⚠ 重复插入必须被拒绝,而不是静默地挂两次。
     *
     * 挂两次的后果不是"这个任务被调度两次",而是**链表被做成环** ——
     * 之后任何一次遍历都会无限循环。这类错误在板上表现为"某个核突然不动了",
     * 而现场完全看不出原因。所以这里宁可多走一遍队列也要查重。
     */
    for (link = &q->head; *link != NULL; link = &(*link)->sched_next) {
        if (*link == t) {
            return false;
        }
    }

    /* 按 deadline 有序插入:队首即"应选者",sched_pick 于是是 O(1) */
    for (link = &q->head; *link != NULL; link = &(*link)->sched_next) {
        if (sched_deadline_before(t, *link)) {
            break;
        }
    }

    t->sched_next = *link;
    *link         = t;
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

tcb_t sched_pick(const sched_queue_t *q)
{
    if (q == NULL) {
        return NULL;
    }

    return q->head;
}

u64 sched_queue_avg_vruntime(const sched_queue_t *q, tcb_t ignore, u64 fallback)
{
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

tcb_t sched_pick_next(const sched_queue_t *q, tcb_t current)
{
    const tcb_t *link;

    if (q == NULL) {
        return NULL;
    }

    for (link = &q->head; *link != NULL; link = &(*link)->sched_next) {
        if (sched_task_schedulable(*link, current)) {
            return *link;
        }
    }

    return NULL;
}

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
     * 2. ★ CPSR 的模式位必须是 SVC ★
     *
     * 这一条是绊线。造新线程时这里原本写的是 `0` —— 而 `0` 的
     * CPSR.M[4:0] = 0b00000,**不是任何一个已分配的模式编码**
     * (AArch32 每个合法模式的 bit4 都是 1),按架构把它装回 CPSR 是
     * UNPREDICTABLE。与其让核心带着一个未定义的 CPSR 跑起来,
     * 不如在这里直接拒绝切换 —— 拒绝是二值的,UB 不是。
     */
    if ((t->ctx.cpsr & ARM_CPSR_MODE_MASK) != ARM_MODE_SVC) {
        return false;
    }

    /* 3. AAPCS 要求 SP 8 字节对齐;帧是 64 字节,所以帧也跟着对齐 */
    sp = t->ctx.sp;
    if (sp == 0u || (sp & 7u) != 0u) {
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
