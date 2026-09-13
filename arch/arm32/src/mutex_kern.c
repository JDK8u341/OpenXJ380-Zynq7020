/*
 * 互斥锁的**内核侧胶水** —— 四个钩子的实现 + 串口排他输出的那把锁
 *
 * 分工与 `sched.c` / `sched_kern.c` 一样:
 *   `src/mutex.c`  纯状态机(宿主穷尽测)
 *   本文件          把它接到真实的"当前任务 / 让出 / 自旋锁"上
 *
 * ====================================================================
 * ★ 它顶替的是"关调度"(退化清单 D13)★
 * ====================================================================
 *
 * M4-8.4 起,`console_excl_begin/end` 是用 `sched_disable/sched_enable`
 * 实现排他输出的。那个做法**在当时的条件下成立**,但代价很大:
 *
 *   - 自检报告要打几秒钟,这几秒里**整个系统停摆**(所有线程都醒不过来),
 *     于是饥饿监视器必须专门把"停摆"从"饥饿"里减掉(见 sched_off_total_ns);
 *   - 它根本不是锁:两个**核上**的写者各打各的,谁也拦不住谁 ——
 *     之所以没出事,只是因为 M4-10 之前所有会打印的线程都被钉在 CPU0。
 *
 * 换成源 OS 的 yield 型互斥之后:报告期间调度器照常跑,
 * 而互斥是真的(跨核也成立)。
 *
 * ⚠ 一处**钩子层**的偏离(不是状态机):源 OS 的等锁靠 `scheduler_yield()`
 *   空转,本移植改成"睡一个 tick 再重试"。理由与推演见 `hook_yield` 上面
 *   那一段 —— 根因是 D16(idle 不作候选),照抄纯 yield 会静默死锁。
 *
 * ====================================================================
 * ★ 那两条诊断计数不是装饰 ★
 * ====================================================================
 *
 * `console_off_yields`:**在"调度器被关掉"的情况下等过锁**的次数。
 *
 *   为什么它是绊线:等锁的那一下会进 `sched_tick`,而 `sched_tick` 在关调度时
 *   **第一行就返回**(照源 OS `scheduler.cpp:384`)⇒ 那时"让出/睡眠"都不发生,
 *   等锁退化成**忙等**。同核上的持锁者于是永远拿不到 CPU ——
 *   那不是"慢",是**死锁**,而且不报任何东西。
 *
 *   源 OS 有一模一样的结构性隐患(它的 `scheduler_yield()` 只是
 *   `int $32`,关调度时同样进不去)。我们的处置**不是**发明一个新的
 *   保护机制,而是把它做成**可观测的**:这个数一旦不为 0,就说明
 *   "有人在关调度的窗口里打印" —— 那正是必须去看的地方。
 *
 * `console_lock_errors` / `console_unlock_errors`:状态机返回非 0 的次数。
 *   生产路径上这两个数恒为 0(`mutex_create` 用的是递归锁,深度计数在
 *   `console.c` 里挡住嵌套)⇒ 它们红了就是用法错了,而不是"偶发"。
 */

#include <arch/console.h>
#include <arch/cpu.h>
#include <arch/mutex.h>
#include <arch/sched.h>

/*
 * 等锁时两次重试之间"让出去"多久。
 *
 * 一个 tick(= `SCHED_TICK_NS`,1ms):唤醒扫描就发生在 tick 的选取里,
 * 比它更短的睡眠不会更早被看见。为什么这里不是"纯让出"—— 见 `hook_yield`
 * 上面那段(纯 yield 在本移植里会死锁)。
 */
#define CONSOLE_LOCK_RETRY_NS SCHED_TICK_NS

static mutex_t g_console_mutex;
static spin_t  g_state_lock; /* 状态机内部临界区(irqsave 契约,见 arch/mutex.h)*/

static u32 g_console_yields;      /* 等锁时让出的总次数(= 竞争重试次数)*/
static u32 g_console_off_yields;  /* ★ 其中"当时调度器是关着的"那些(必须 0)*/
static u32 g_console_lock_errors; /* mutex_lock 返回非 0 的次数(必须 0)*/
static u32 g_console_unlock_errors;
static u32 g_console_legacy_refused; /* 在"有窗口开着"时试图切换对照组的次数 */

/*
 * 只统计**串口锁**的等待 —— `mutex_set_ops` 是模块级的,将来若有第二把锁,
 * 它的等待也会进同一个 `yield` 钩子。用这个标志把范围钉死在串口锁上,
 * 免得那个计数在不远的将来变成"几把锁之和"这种没人能解释的数。
 * (等真出现第二把锁,正确的做法是每把锁一份 ops —— 到那时再改。)
 */
static u32 g_in_console_wait;

/* ★ 破坏性 A/B:置 1 时"退回去"用关调度实现排他(见 console_mutex_set_legacy)*/
static u32 g_legacy;

/* ------------------------------------------------------------------ */
/* 四个钩子                                                            */
/* ------------------------------------------------------------------ */

static tcb_t hook_current(void)
{
    return sched_current();
}

/*
 * 等锁失败时"把 CPU 交出去"的那一下。
 *
 * ====================================================================
 * ★ 这里**不是**源 OS 的 `scheduler_yield(); cpu_relax();` —— 理由很具体 ★
 * ====================================================================
 *
 * 结论先写:**用纯 yield 在本移植里会死锁**,所以钩子改成"让出 + 短睡"。
 * 状态机一个字没改(仍是源 OS 的"不挂等待队列、拿不到就重试"),
 * 改的只是"两次重试之间那一下怎么让"。
 *
 * 为什么会死锁 —— 三步,每一步都能在代码里指出来:
 *
 *   1. 本移植的 **idle 不作调度候选**。`sched_task_schedulable()` 排除
 *      `TASK_IDLE_LEVEL`(源 OS 也排除 idle,`scheduler.cpp:178`),
 *      而"回到 idle"只发生在兜底链的最后一步:
 *          `result = best ? best : (current 可运行 ? current : idle)`
 *          (`src/sched.c:467` ← `scheduler.cpp:358`)
 *      ⇒ **只要还有一个可运行的普通线程,idle 就永远轮不到**。
 *
 *   2. 自检报告跑在 **idle 上下文**里(kmain 就是启动流程,它注册成 BSP idle),
 *      而且它持着串口锁在打印。
 *
 *   3. 用纯 yield 的话,等锁者 `yield` 之后仍是 RUNNING/START ⇒ 它就是
 *      `best` ⇒ 调度器**又把它选回来**(`next == cur`,连切换都不做)。
 *      它拿着 CPU 转圈等锁,而持锁的 idle 永远回不来 ⇒
 *      **拿锁的人等放锁的人,放锁的人等拿锁的人让出 CPU** ——
 *      不报任何东西的静默死锁,表现为"报告打了一半就没了"。
 *
 * 源 OS 为什么没这个问题:它的 **BSP idle 是 level 0 = 可调度候选**
 *   (`main.cpp:520-541` 从不赋 `task_level`,见 PTASK.md §2.4),
 *   于是"让出"真的能把 CPU 交给持锁的 idle 上下文。
 *   而 ARM 侧刻意**没有**照抄那个 0(退化清单 D16:它会让 `context0.rip == 0`
 *   的 idle 变成候选、白做派发、还被 EEVDF 计费)。
 *   ⇒ 这是 D16 那个偏离的**连锁后果**,必须写在这里,否则下一个人只会看到
 *     "怎么没照源 OS"。
 *
 * 为什么是"睡一个 tick"而不是别的:
 *   - 唤醒扫描发生在每次 `sched_select_next`(tick 与 yield 都会进),
 *     所以**比一个 tick 更短的睡眠没有意义**;
 *   - 等锁者不再占 CPU(它变成 WAIT、不作候选)⇒ 持锁的 idle 立刻能跑;
 *   - 语义上仍是源 OS 的"不把线程挂在锁上":没有等待队列、没有所有权转移、
 *     `mutex_unlock` 也不唤醒任何人 —— 睡眠只是这一跳的实现手段,
 *     醒来之后仍然**重试同一把锁**。
 *
 * ⚠ 反方向(idle 等普通线程)不需要额外处理:那时等锁者是 idle,
 *   `sched_sleep_ns()` 对它早退成一次 yield(它自己那条
 *   `task_level == TASK_IDLE_LEVEL` 的判断),而持锁的普通线程迟早要睡
 *   ⇒ idle 一定拿得回 CPU。两个方向都不再可能静默卡住。
 */
static void hook_yield(void)
{
    if (g_in_console_wait != 0u) {
        g_console_yields++;
        if (!sched_is_enabled()) {
            /*
             * ★ 绊线:此刻"让出/睡眠"都退化(关调度时 `sched_tick` 第一行
             *   就返回),等锁变成忙等,同核上的持锁者拿不到 CPU。
             *   见文件顶部的说明。
             */
            g_console_off_yields++;
        }
    }

    sched_sleep_ns(CONSOLE_LOCK_RETRY_NS);
}

static void hook_enter_state(void)
{
    spin_lock(&g_state_lock);
}

static void hook_leave_state(void)
{
    spin_unlock(&g_state_lock);
}

static const mutex_ops_t g_ops = {
    hook_current,
    hook_yield,
    hook_enter_state,
    hook_leave_state,
};

/* ------------------------------------------------------------------ */
/* 装入 console 的两条排他钩子                                          */
/* ------------------------------------------------------------------ */

/*
 * `console_excl_begin/end` 的钩子。**注意 console.c 有自己的嵌套深度计数**:
 * 这两条只在"0 → 1"和"1 → 0"的时刻被叫到,所以锁的获取/释放是严格配对的,
 * 而且同一时刻只有一个线程可能持有它(嵌套由深度计数挡住)。
 *
 * ⚠ 递归标志仍然给 `true`:源 OS 的 `mutex_create(mtx, recursive)` 就是
 *   这个语义,而"排他输出里再排他输出"是真实存在的形状(报告内部还会打印)。
 *   深度计数让生产路径**走不到**递归那一支 —— 但那是"这一层的用法使它不必
 *   递归",不是"mutex 不该支持递归"。宿主测试把递归那一支逐值钉住。
 */
static void console_excl_acquire(void)
{
    int rc;

    if (g_legacy != 0u) {
        /*
         * ★ 对照组:退回到 M4-11.1 之前的"关调度" ★
         * 与 `console_excl_release` 成对;由 `console_mutex_set_legacy`
         * 保证只在"没有窗口开着"的时候才可能切进来。
         */
        sched_disable();
        return;
    }

    g_in_console_wait = 1u;
    rc                = mutex_lock(&g_console_mutex);
    g_in_console_wait = 0u;

    if (rc != 0) {
        g_console_lock_errors++;
    }
}

static void console_excl_release(void)
{
    if (g_legacy != 0u) {
        sched_enable();
        return;
    }

    if (mutex_unlock(&g_console_mutex) != 0) {
        g_console_unlock_errors++;
    }
}

/* ------------------------------------------------------------------ */
/* 对外                                                                */
/* ------------------------------------------------------------------ */

void console_mutex_init(void)
{
    spin_init(&g_state_lock);
    mutex_set_ops(&g_ops);
    mutex_create(&g_console_mutex, true);

    console_set_excl_hooks(console_excl_acquire, console_excl_release);
}

/*
 * ★ 破坏性 A/B 的开关:把排他输出"退回去"用关调度 ★
 *
 * 只在**没有窗口开着**的时候允许切换,判据是两条:
 *   - 锁是空的(`mutex_is_locked()` 为假)—— 锁被拿着还切走,释放那一头
 *     就会去 `sched_enable()`,于是锁永远留在那里;
 *   - 调度器是开着的 —— 关着说明正处在某个 `sched_disable` 窗口里。
 *
 * 被拒绝时**不改任何东西**,只把次数记下来(静默失败的拒绝等于没拒绝)。
 */
void console_mutex_set_legacy(u32 on)
{
    if (mutex_is_locked(&g_console_mutex) || !sched_is_enabled()) {
        g_console_legacy_refused++;
        return;
    }

    g_legacy = (on != 0u) ? 1u : 0u;
}

bool console_mutex_legacy(void)
{
    return g_legacy != 0u;
}

u32 console_mutex_yields(void)
{
    return g_console_yields;
}

u32 console_mutex_off_yields(void)
{
    return g_console_off_yields;
}

u32 console_mutex_lock_errors(void)
{
    return g_console_lock_errors;
}

u32 console_mutex_unlock_errors(void)
{
    return g_console_unlock_errors;
}

u32 console_mutex_legacy_refused(void)
{
    return g_console_legacy_refused;
}

/*
 * ★ M4-11.2 的绊线:当前线程是不是**正持着串口锁**。
 *
 * 为什么需要它:源 OS 的 `mutex->owner` **从不注销**(`include/mutex.h:17`,
 * 清零点只有 unlock/destroy)。在"线程不会退出"的世界里那不构成问题;
 * 而 M4-11.2 让"线程会退出"第一次变得可达 ⇒ **持锁线程退出**就成了一个
 * 真实的死锁源:锁永远回不来,下一次 `console_excl_begin()` 会一直等下去
 * (等锁者睡一个 tick 再重试,于是表现为"串口再也不出声",不报任何东西)。
 *
 * ⚠ 处置是**报警而不是修**:源 OS 没有"退出时注销 owner"这回事,
 *   按 2026-09-13 的决定不发明它。所以退出路径上问一句、计数、进报告
 *   (判据恒为 0),同时报告里有一条**正向对照**(故意在持锁时问一次,
 *   必须为真)证明这条绊线接在了正确的信号上,而不是永远为假。
 */
bool console_mutex_owner_is_current(void)
{
    tcb_t owner = mutex_get_owner(&g_console_mutex);

    return (owner != NULL) && (owner == sched_current());
}
