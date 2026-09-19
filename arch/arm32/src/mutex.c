/*
 * 互斥锁的**纯状态机** —— 与 source OS `kernel/task/mutex.cpp` 逐句对应
 *
 * 设计说明(为什么是 yield 型、为什么做成钩子、与源 OS 的三处差别、
 * 两个照抄过来的边界)全在 `arch/mutex.h` 顶部 —— 动这个文件之前先读那段。
 *
 * 本文件**不含** MMIO / CP15 / 内联汇编 / 时间源:唯一的对外依赖是四个
 * 函数指针(取当前任务 / 让出 / 进临界区 / 出临界区)。于是同一份代码
 * 在宿主机上可以穷尽测(tests/test_arm32_mutex.py),在板子上就是它本身。
 */

#include <arch/mutex.h>

static const mutex_ops_t *g_ops;

void mutex_set_ops(const mutex_ops_t *ops)
{
    g_ops = ops;
}

static tcb_t current_task(void)
{
    return ((g_ops != NULL) && (g_ops->current != NULL)) ? g_ops->current() : NULL;
}

/*
 * 临界区。源 OS 里这一段是 `spin_lock(&mutex->lock)` / `spin_unlock(...)`
 * —— **irqsave 契约**(进临界区时关中断),因为在多核上它保护的是
 * `state/owner/rcc` 这三个字段的读改写。
 *
 * ⚠ 它**只在状态机内部**用,绝不覆盖 `yield`(源 OS 也是在 `spin_unlock`
 *   之后才 `scheduler_yield()`)—— 持自旋锁去让出是死锁的写法。
 */
static void state_enter(void)
{
    if ((g_ops != NULL) && (g_ops->enter_state != NULL)) {
        g_ops->enter_state();
    }
}

static void state_leave(void)
{
    if ((g_ops != NULL) && (g_ops->leave_state != NULL)) {
        g_ops->leave_state();
    }
}

/*
 * "让出 + 重试"里的**让出**。
 *
 * ← 源 OS `mutex.cpp:61-62`:
 *      scheduler_yield();
 *      cpu_relax();
 *   两件事的顺序照抄,但**只做第一件**:`cpu_relax` 是 CPU 指令
 *   (`yield`/`wfe` 一类),属于钩子的实现 —— 见 arch/mutex.h 的"差别 3"。
 */
static void yield_cpu(void)
{
    if ((g_ops != NULL) && (g_ops->yield != NULL)) {
        g_ops->yield();
    }
}

/* ← `mutex_create()` `mutex.cpp:10-19`(少一行 `queue_init`,见 arch/mutex.h)*/
void mutex_create(mutex_t *m, bool recursive)
{
    if (m == NULL) {
        return;
    }

    m->state = MUTEX_UNLOCKED;
    m->owner = NULL;
    m->rcc   = 0;
    m->rec   = recursive;
}

/*
 * ← `mutex_lock()` `mutex.cpp:23-64`,逐句对应:
 *
 *      if (mutex == NULL) return -EINVAL;
 *      current = get_current_task();
 *      for (;;) {
 *          spin_lock(&mutex->lock);
 *          if (state == DESTROYED) { unlock; return -EINVAL; }
 *          if (owner == current) {
 *              if (rec) { rcc++; unlock; return 0; }
 *              unlock; return -EDEADLK;
 *          }
 *          if (state == UNLOCKED) { state = LOCKED; owner = current; rcc = 1;
 *                                  unlock; return 0; }
 *          unlock;
 *          scheduler_yield();
 *          cpu_relax();
 *      }
 *
 * ★ 循环里**没有任何计数上限**:等多久取决于持锁者什么时候放锁,
 *   与源 OS 一致(它也没有超时)。掉进这个循环出不来只有两种可能 ——
 *   持锁者被饿死(调度器坏了),或者持锁者自己也在等这把锁(用法错了)。
 */
int mutex_lock(mutex_t *m)
{
    tcb_t current;

    if (m == NULL) {
        return -EINVAL;
    }

    current = current_task();

    for (;;) {
        state_enter();

        if (m->state == MUTEX_DESTROYED) {
            state_leave();
            return -EINVAL;
        }

        if (m->owner == current) {
            if (m->rec) {
                m->rcc++;
                state_leave();
                return 0;
            }

            state_leave();
            return -EDEADLK;
        }

        if (m->state == MUTEX_UNLOCKED) {
            m->state = MUTEX_LOCKED;
            m->owner = current;
            m->rcc   = 1;
            state_leave();
            return 0;
        }

        state_leave();

        /* 拿不到 ⇒ 让出时间片再来看一眼(源 OS 的 yield 型语义)*/
        yield_cpu();
    }
}

/*
 * ← `mutex_trylock()` `mutex.cpp:67-104`。
 *
 * ★ 与 `mutex_lock` 的唯一结构性差别:**不循环、不让出** ——
 *   拿不到就返回 `-EBUSY`。源 OS 就是这样,宿主测试专门钉住
 *   "trylock 一次 yield 都不能发生"(否则它就不再是"非阻塞"了)。
 */
int mutex_trylock(mutex_t *m)
{
    tcb_t current;

    if (m == NULL) {
        return -EINVAL;
    }

    current = current_task();

    state_enter();

    if (m->state == MUTEX_DESTROYED) {
        state_leave();
        return -EINVAL;
    }

    if (m->owner == current) {
        if (m->rec) {
            m->rcc++;
            state_leave();
            return 0;
        }

        state_leave();
        return -EDEADLK;
    }

    if (m->state == MUTEX_UNLOCKED) {
        m->state = MUTEX_LOCKED;
        m->owner = current;
        m->rcc   = 1;
        state_leave();
        return 0;
    }

    state_leave();
    return -EBUSY;
}

/*
 * ← `mutex_unlock()` `mutex.cpp:107-139`。
 *
 * 判据是**三合一**的:`owner != current || state != LOCKED || rcc == 0`
 * ⇒ `-EPERM`。三者缺一都会让"没拿锁的人把锁放掉"变成静默的坏事
 * (放掉之后别人就能进临界区,而原持有者还以为自己独占)。
 *
 * ⚠ `rcc--` 到 0 才真的放锁;`state` 在"已被销毁"时**不动**
 *   (源 OS 那句 `if (state != DESTROYED)` —— 走到这里时 state 必然是
 *   LOCKED,所以那一句永远成立;我们照抄这个形状,不"优化"掉它)。
 */
int mutex_unlock(mutex_t *m)
{
    tcb_t current;

    if (m == NULL) {
        return -EINVAL;
    }

    current = current_task();

    state_enter();

    if (m->state == MUTEX_DESTROYED) {
        state_leave();
        return -EINVAL;
    }

    if (m->owner != current || m->state != MUTEX_LOCKED || m->rcc == 0u) {
        state_leave();
        return -EPERM;
    }

    m->rcc--;

    if (m->rcc == 0u) {
        m->owner = NULL;
        if (m->state != MUTEX_DESTROYED) {
            m->state = MUTEX_UNLOCKED;
        }
    }

    state_leave();
    return 0;
}

/*
 * ← `mutex_destroy()` `mutex.cpp:142-163`(少一次 `queue_destroy` ——
 * 那个 wait_queue 是化石,见 arch/mutex.h 的"差别 1")。
 *
 * ★ "锁着的时候销毁"返回 `-EBUSY` 而不是硬销毁:硬销毁会让持有者的
 *   临界区凭空消失(它还以为自己独占)。源 OS 的选择,照抄。
 */
int mutex_destroy(mutex_t *m)
{
    if (m == NULL) {
        return -EINVAL;
    }

    state_enter();

    if (m->state == MUTEX_LOCKED) {
        state_leave();
        return -EBUSY;
    }

    m->state = MUTEX_DESTROYED;
    m->owner = NULL;
    m->rcc   = 0u;

    state_leave();
    return 0;
}

/* ← `mutex_is_locked()` `mutex.cpp:166-176` */
bool mutex_is_locked(mutex_t *m)
{
    bool locked;

    if (m == NULL) {
        return false;
    }

    state_enter();
    locked = (m->state == MUTEX_LOCKED);
    state_leave();

    return locked;
}

/* ← `mutex_get_owner()` `mutex.cpp:179-189` */
tcb_t mutex_get_owner(mutex_t *m)
{
    tcb_t owner;

    if (m == NULL) {
        return NULL;
    }

    state_enter();
    owner = m->owner;
    state_leave();

    return owner;
}

/* ------------------------------------------------------------------ */
/* ★ 落地层入口(M4A-1.4):只换名字、不换语义 ★                        */
/* ------------------------------------------------------------------ */

/*
 * 为什么需要它们、为什么参数是 `void *`,写在 <arch/mutex.h> 的同一节。
 * 这里只说实现上的两条:
 *
 *   1. **语义一个字没变** —— 每个函数体就是一行转发。上游 FATFS 拿到的
 *      就是 M4-11.1 在板上验过的那把 mutex(含 -EPERM/-EDEADLK/-EBUSY
 *      那几条边界),不是另一套实现。
 *   2. **"只占 16 字节"这个前提由静态断言钉住**:落地层交过来的是上游
 *      `mutex_t` 对象,移植侧把它当自己的 16 字节结构写。改大了这个大小,
 *      上游那个对象就可能被写越界 —— 而那种错误在源码上看不出来。
 */
_Static_assert(sizeof(mutex_t) == 16u,
               "落地层把上游 mutex_t 对象当 16 字节存储用(见 arch/mutex.h);"
               "改了这个大小必须同时核对 include/mutex.h 的布局");

void arm_mutex_create(void *m, bool recursive)
{
    mutex_create((mutex_t *)m, recursive);
}

int arm_mutex_lock(void *m)
{
    return mutex_lock((mutex_t *)m);
}

int arm_mutex_unlock(void *m)
{
    return mutex_unlock((mutex_t *)m);
}
