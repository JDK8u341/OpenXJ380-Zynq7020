#pragma once

/*
 * 互斥锁 —— 逐条照抄 source OS 的 `kernel/task/mutex.cpp`(189 行)
 * 与 `include/mutex.h`(29 行)
 *
 * ====================================================================
 * ★ 它是 **yield 型**,不是"可睡眠"型 ★
 * ====================================================================
 *
 * 这不是我们的选择,是源 OS 的选择。`mutex.cpp:21-22` 的作者原注释:
 *
 *     当前调度器里的 WAIT 语义并不适合通用互斥，这里采用 yield 型互斥：
 *     获取失败时主动让出时间片，但不把线程切到 WAIT，避免线程在未持锁时继续执行。
 *
 * 实现形状 = `spin_lock(内部状态)` 判一下 → 拿不到就 `scheduler_yield(); cpu_relax();`
 * 重来(不是睡进等待队列)。TCB 里那个 `wait_queue` 字段**从头到尾没人用**。
 *
 * ⚠ **绝不能用自旋锁代替它**(本项目差点这么干,见退化清单 D13):
 *   串口一行要 60~80ms,持锁者随时会被抢占;等锁者若关中断自旋,
 *   就再也没有人能放锁 —— 那不是"慢",是死锁。
 *
 * ====================================================================
 * ★ 为什么是"钩子"而不是直接调调度器 ★
 * ====================================================================
 *
 * 与 `kstack` 的 TLB 维护、`console` 的排他钩子同一个模式:本文件与
 * `src/mutex.c` **不含 MMIO / CP15 / 内联汇编**,于是宿主上能直接编译并
 * **穷尽测状态机**(递归/EDEADLK/EPERM/EBUSY/EINVAL/销毁后……)。
 *
 * 分工与 `sched.h` 顶部写的一样:策略与状态机的唯一判据是宿主测试,
 * 板级证据只能证明"机制接上了、真的被竞争过"。
 *
 * 四个钩子:
 *   `current`  取当前任务(源 OS 的 `get_current_task()`;ARM 侧 `sched_current()`)
 *   `yield`    等锁失败时"把 CPU 交出去"的那一下(源 OS 是
 *              `scheduler_yield(); cpu_relax();` —— 那两件事属于**钩子的实现**,
 *              不属于本层;`cpu_relax` 是 CPU 指令,本层不能有)。
 *              ★ 本移植的钩子不是纯 yield,而是"睡一个 tick 再重试" ——
 *              理由是 D16 的连锁后果(我们两个 idle 都不作调度候选,纯 yield
 *              会让持锁的 idle 上下文永远回不来),完整推演写在
 *              `src/mutex_kern.c` 的 `hook_yield` 上面。**状态机不受影响**:
 *              它只知道"让一下,然后再试"。)
 *   `enter_state` / `leave_state`
 *              保护锁内部状态的临界区(源 OS 里是结构体自带的
 *              `spin_t lock`,irqsave 契约)。宿主测试传空实现。
 *
 * ⚠ 钩子是**模块级**的(与 `console_set_excl_hooks` 同一个形状):
 *   本层不做分配、不存句柄,于是"谁在用哪个锁"这件事不影响状态机的可测性。
 *
 * ====================================================================
 * ★ 与源 OS 的差别(三处,每处都写清为什么)★
 * ====================================================================
 *
 * 1. **没有 `wait_queue` 字段**。源 OS 那个字段只在 create/destroy 里
 *    建/销毁,读它的人一个也没有 —— 是"本来打算做可睡眠、后来改成 yield"
 *    的化石。本项目对这类死物的规矩是**删掉而不是留着**(D10/D11),
 *    所以不搬;语义上没有任何损失(`mutex_destroy` 少一次 `queue_destroy`)。
 *
 * 2. **errno 用 `arch/errno.h` 的同名常量**,返回值就是负的 errno 数值。
 *    源 OS 四个返回值 `-EINVAL / -EDEADLK / -EPERM / -EBUSY` 一个不少,
 *    宿主测试逐条钉住(数值也必须一致 —— 那是给将来的系统调用面用的)。
 *
 * 3. **`cpu_relax()` 不在本层**(见上)。源 OS 里它紧跟 `scheduler_yield()`,
 *    本层只负责"叫一下钩子";那两件事(让出、relax)由钩子按同一个顺序做。
 *    ⚠ 而 ARM 侧的钩子连"让出"都换成了"睡一个 tick" —— 那处偏离的理由
 *    与影响面写在 `src/mutex_kern.c`,与状态机无关。
 *
 * ====================================================================
 * ★ 两个"照抄过来的边界"(钉在宿主测试里,免得被当成 bug 改掉)★
 * ====================================================================
 *
 * 1. **没有 current 任务时(钩子返回 NULL)**,`mutex_lock` 在"锁是空闲的"
 *    情况下会先命中 `owner == current`(NULL == NULL):
 *      - `rec == false` ⇒ 返回 `-EDEADLK`;
 *      - `rec == true`  ⇒ **返回 0 但并没有真的拿到锁**(`state` 仍是
 *        `MUTEX_UNLOCKED`,而 `rcc` 变成了 1)。
 *    源 OS 就是这个行为(`mutex.cpp:38-49` 在 `:51` 的"空闲就取"之前)。
 *    我们**照抄,不修正**(尊重源 OS),但要写清它在 ARM 侧**不可达**:
 *    `console_excl` 的两条钩子是在 `sched_register_boot_idle()` 之后
 *    才装上的(`src/kmain.c`),而那之前 `console_excl_begin/end` 是空操作
 *    (console.c 的深度计数 + 没装钩子)。从此 `sched_current()` 恒非空。
 *
 * 2. **`mutex->owner` 在线程退出时不注销** —— 源 OS 从不注销它
 *    (`include/mutex.h:17` 的字段,写入点只有 lock/trylock,清零点只有
 *    unlock/destroy)。M4-11.2 有了线程退出之后,"持锁线程退出"第一次
 *    **在源 OS 里也变得可达**;我们的处置写在 `src/mutex_kern.c`:
 *    退出路径上带一条绊线(计数),而不是发明一个源 OS 没有的注销机制。
 */

#include <arch/errno.h>
#include <arch/tcb.h>
#include <arch/types.h>

/* ← `include/mutex.h:7-11` */
typedef enum
{
    MUTEX_UNLOCKED  = 0,
    MUTEX_LOCKED    = 1,
    MUTEX_DESTROYED = 2
} mutex_state_t;

/*
 * ← `include/mutex.h:14-21`,少了 `spin_t lock`(那是 `enter_state/leave_state`
 * 钩子的事)与 `lock_queue *wait_queue`(化石,见上)。
 *
 * 字段顺序与源 OS 一致(state → owner → rcc → rec),纯粹为了两边对照着读;
 * 本结构**不跨 ABI**(不像 TCB 那样被汇编引用)。
 */
typedef struct
{
    mutex_state_t state; /* 锁状态 */
    tcb_t         owner; /* 当前持有者(被锁定时非空)*/
    u32           rcc;   /* 递归计数(可重入锁)*/
    bool          rec;   /* 是否为递归锁 */
} mutex_t;

typedef struct
{
    tcb_t (*current)(void);
    void (*yield)(void);
    void (*enter_state)(void);
    void (*leave_state)(void);
} mutex_ops_t;

/*
 * 装上四个钩子(启动时一次)。传 NULL 表示"清空钩子":
 * 那时 `current` 恒为 NULL、`yield` 与临界区都是空操作 ——
 * 宿主测试里有一节专门用这个形态跑(它同时就是上面"边界 1"的场景)。
 */
void mutex_set_ops(const mutex_ops_t *ops);

/* ← `mutex_create(mutex_t*, bool recursive)`。⚠ NULL 时**静默返回**(照源 OS)*/
void mutex_create(mutex_t *m, bool recursive);

/* ← `mutex_lock`:拿不到就"让出 + 重试",**不睡**。返回 0 或 -EINVAL/-EDEADLK */
int mutex_lock(mutex_t *m);

/* ← `mutex_trylock`:一次判定,不阻塞、**从不让出**。返回 0 或 -EINVAL/-EDEADLK/-EBUSY */
int mutex_trylock(mutex_t *m);

/* ← `mutex_unlock`:非持有者 ⇒ -EPERM;已销毁 ⇒ -EINVAL */
int mutex_unlock(mutex_t *m);

/* ← `mutex_destroy`:锁着的时候销毁 ⇒ -EBUSY(照源 OS,不许"销毁一个还在用的锁")*/
int mutex_destroy(mutex_t *m);

bool  mutex_is_locked(mutex_t *m);
tcb_t mutex_get_owner(mutex_t *m);

/* ------------------------------------------------------------------ */
/* ★ 落地层入口(M4A-1.4):只换名字、不换语义 ★                        */
/* ------------------------------------------------------------------ */

/*
 * 为什么需要这三个"换名字"的入口 —— 不是冗余,是 C++ 语言层面的硬约束:
 *
 * 上游 `include/mutex.h` 声明的是 **C++ 链接** 的
 * `void mutex_create(mutex_t*, bool)`(实测符号 `_Z12mutex_createP5mutexb`),
 * 而 `driver/fs/fatfs/fatfs.cpp` 会调它 ⇒ 落地层**必须**给出那些修饰名。
 * 落地层同时还要调移植侧这份**已经板上验证过**的实现 ——
 * 但移植侧的函数**同名**(`mutex_create`)。在 C++ 里"同一个名字、两种链接"
 * 是**非法**的(`conflicts with a previous declaration`),所以落地层
 * 无法用同一个名字声明两边。
 * ⇒ 由移植侧提供这三个入口:函数体就是一行转发,语义**一个字都没变**。
 *
 * ⚠ 参数是 `void *`,因为上游 `mutex_t` 与移植侧 `mutex_t` **布局不同**
 *   (上游多了 `spin_t lock` 与 `lock_queue *wait_queue`),两边不能互相
 *   reinterpret。落地层只是把**上游那个对象的前 16 字节**当存储交过来 ——
 *   这在本项目里是安全的,因为 ARM 图里**没有**上游的 mutex 实现
 *   (`kernel/task/mutex.cpp` 不在构建里),那些字节在 ARM 上只有这一条
 *   读写路径。两侧各有一条静态断言钉住这个前提(见 `src/mutex.c` 与
 *   `src/upstream_api.cpp`)。
 */
void arm_mutex_create(void *m, bool recursive);
int  arm_mutex_lock(void *m);
int  arm_mutex_unlock(void *m);

/* ------------------------------------------------------------------ */
/* 内核侧(实现在 src/mutex_kern.c)                                    */
/* ------------------------------------------------------------------ */

/*
 * 装上四个钩子(取当前任务 / 让出 / 进临界区 / 出临界区)、创建串口锁,
 * 并把 `console_excl_begin/end` 接到这把锁上。
 *
 * 调用的时机有要求:**必须在 `sched_register_boot_idle()` 之后** ——
 * 那之前 `sched_current()` 是 NULL,而"没有 current 任务时空闲的锁会命中
 * `owner == current`"是源 OS 的一个已知边界(见本文件顶部"边界 1")。
 * 在那之前 `console_excl_*` 本来就是空操作(console.c 没装钩子),所以
 * 这个顺序不需要额外保护,只需要说清。
 */
void console_mutex_init(void);

/*
 * ★ 破坏性 A/B:把排他输出"退回去"用关调度实现(置 1)★
 *
 * 这是 M4-11.1 那件事的**对照组**:同一段打印负载,一种做法按一下
 * 调度器开关(几秒钟内整个系统停摆),另一种拿一把 yield 型互斥。
 * 判据是差值的对比,不是"某一项看起来对":
 *
 *     关调度时长(sched_off_total_ns 的窗口差值):新模式 ≈ 0,老模式 ≈ 窗口长度
 *     状态线程在窗口里醒过几次                    :新模式 ≥ 1,老模式 == 0
 *
 * ⚠ 只允许在**没有窗口开着**的时候切换(锁是空的、调度器是开着的);
 *   否则拒绝并计数(`console_mutex_legacy_refused`)。
 *   生产路径上它恒为 0(→ 新模式)。
 */
void console_mutex_set_legacy(u32 on);
bool console_mutex_legacy(void);

/* 等锁时让出的总次数(= 竞争重试次数)。恒为 0 说明这把锁**从来没被竞争过***
 * —— 那会让"互斥成立"这句话变成空话,所以报告里它是一条判据。 */
u32 console_mutex_yields(void);

/* ★ 其中"当时调度器是关着的"那些。恒为 0 是死锁隐患的绊线(见 .c 的说明)★ */
u32 console_mutex_off_yields(void);

/* 状态机返回非 0 的次数(锁/解锁各一个)。生产路径上都是 0 */
u32 console_mutex_lock_errors(void);
u32 console_mutex_unlock_errors(void);

/* 在"有窗口开着"时试图切换对照组的次数 —— 它是上面那条约束的**执行证据** */
u32 console_mutex_legacy_refused(void);

/*
 * ★ M4-11.2 的绊线:当前线程是不是正持着这把串口锁 ★
 *
 * `mutex->owner` 在源 OS 里**从不注销**,于是"持锁线程退出"会把锁永久带走
 * (下一次打印会一直等下去,不报任何东西)。M4-11.2 让"线程会退出"第一次
 * 变得可达 ⇒ 退出路径上问一句、计数、进报告(恒为 0);
 * 报告里另有一条**正向对照**(故意在持锁时问一次,必须为真)。
 * ⚠ 处置是报警,**不是**发明源 OS 没有的"退出时注销 owner"。
 */
bool console_mutex_owner_is_current(void);
