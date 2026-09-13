#pragma once

/*
 * 调度策略 —— M4-8.2(纯逻辑)
 *
 * ====================================================================
 * 照搬源 OS,不自己设计
 * ====================================================================
 *
 * 动手前先把 `kernel/task/scheduler.cpp`(594 行)读清楚。结论是
 * **它比名字听起来简单得多** —— 源 OS 的 "EEVDF" 实际是
 * **"无权重的虚拟期限公平队列 + 睡醒补偿"**:
 *
 *   | 事实                          | 出处                        |
 *   |-------------------------------|-----------------------------|
 *   | `vruntime_delta()` 是恒等式   | `scheduler.cpp:232`         |
 *   | TCB 里没有权重字段            | `include/task/pcb.h`        |
 *   | 没有 lag / eligibility        | 全文件                      |
 *   | 拾取只有 deadline_before 一句 | `scheduler.cpp:237`         |
 *   | 唯一"EEVDF 味道"是睡醒补偿    | `apply_eevdf_wakeup_credit` |
 *
 * ⚠ `vruntime_delta()` 是 `(ns * 1024) / 1024`,**乘除同一个常数**。
 *   看起来是源 OS 没做完的地方(权重字段根本没建)。按项目规程
 *   **照抄,不擅自"补全"**,并写进退化清单:
 *     「源 OS 此处是恒等式,ARM 侧照抄;将来要做权重,两侧一起改」。
 *
 * ====================================================================
 * ★ 这个文件是策略的唯一判据 ★
 * ====================================================================
 *
 * 板级证据只能证明**机制**对(切换、抢占、无饥饿、idle),证明不了策略:
 * **任何策略在"等权 + 纯占用"负载下都会通过**。
 * 所以拾取顺序、睡醒补偿、队列不变量这些**只能靠宿主穷尽测** ——
 * 见 tests/test_arm32_sched.py。
 *
 * 本文件与 src/sched.c **不含 MMIO / CP15 / 内联汇编**,也不碰时间源:
 * 时间以纳秒作参数传进来,于是宿主机上可以直接构造"过了 3ms"这种场景。
 */

#include <arch/heap.h>
#include <arch/kstack.h>
#include <arch/tcb.h>
#include <arch/types.h>

/* ------------------------------------------------------------------ */
/* 常量:逐个照抄 source(`scheduler.cpp:17-24`)                        */
/* ------------------------------------------------------------------ */

#define SCHED_TICK_NS       1000000ull /* 1 ms —— 每核 tick 周期 */
#define SCHED_TIME_SLICE    4ull       /* 基础时间片,单位 tick */
#define SCHED_MIN_SLICE     1ull
#define SCHED_BASE_SLICE_NS (SCHED_TIME_SLICE * SCHED_TICK_NS) /* 4 ms */
#define SCHED_MIN_SLICE_NS  (SCHED_MIN_SLICE * SCHED_TICK_NS)  /* 1 ms */
#define SCHED_WAKEUP_CREDIT SCHED_BASE_SLICE_NS                /* 4 ms */
#define SCHED_SLEEPER_CREDIT (SCHED_BASE_SLICE_NS * 2ull)      /* 8 ms */
#define SCHED_DEFAULT_WEIGHT 1024ull

/* ------------------------------------------------------------------ */
/* 就绪队列                                                            */
/* ------------------------------------------------------------------ */

/*
 * 侵入式单链表:**节点就在 TCB 里**(`sched_next`),不为队列单独分配内存。
 *
 * 为什么不用源 OS 的 `lock_queue`:那个结构带独立的锁与节点分配,
 * 而 ARM 侧的可睡眠同步原语排在 M4-11 —— 现在只要一个能在宿主上
 * 穷尽测的、无副作用的队列。等 M4-11 需要跨核保护时再决定要不要换。
 *
 * ⚠ 只存"当前核"的队列,所以队列本身不需要锁;跨核迁移(M4-10)会用
 *   另一套机制,到那时再处理。
 */
typedef struct
{
    tcb_t head;
    u32   count;
} sched_queue_t;

void  sched_queue_init(sched_queue_t *q);
void  sched_queue_remove(sched_queue_t *q, tcb_t t);
/* 按 deadline 有序插入(同 deadline 按 vruntime)。这样队首就是应选者 */
bool  sched_queue_insert(sched_queue_t *q, tcb_t t);
/* 队首(应选者);空队列返回 NULL */
tcb_t sched_pick(const sched_queue_t *q);

/* ------------------------------------------------------------------ */
/* 策略原语(逐个对应源 OS 的同名函数)                                 */
/* ------------------------------------------------------------------ */

/* ← `task_sched_slice()` `scheduler.cpp:199` */
u64 sched_slice(tcb_t t);

/* ← `vruntime_delta()` `scheduler.cpp:232`。⚠ 源 OS 是恒等式,这里照抄 */
u64 sched_vruntime_delta(u64 runtime_ns);

/* ← `deadline_before()` `scheduler.cpp:237`。NULL 语义也要照抄 */
bool sched_deadline_before(tcb_t left, tcb_t right);

/*
 * ← `charge_current_eevdf_runtime()` `scheduler.cpp:245`
 * 计费:累加 vruntime,重算 deadline。**不重复计费**是它的三条早退
 * (idle / 非 RUNNING / runtime 为 0)在做的事 —— 宿主测逐一钉住。
 */
void sched_account_run(tcb_t t, u64 runtime_ns);

/* ← `apply_eevdf_wakeup_credit()` `scheduler.cpp:205` */
void sched_apply_wakeup_credit(tcb_t t, u64 base_vruntime);

/*
 * ← `wake_sleeping_task()` `scheduler.cpp:220`
 * 到点就唤醒:清 wakeup_time、置 START、给睡醒补偿。返回是否唤醒了。
 */
bool sched_wake_if_due(tcb_t t, u64 now, u64 base_vruntime);

/* 新线程的初始调度状态。源 OS 在 `init_task_eevdf_entity()` 里做同名的事 */
void sched_entity_init(tcb_t t, u64 now);

/* 纯函数:队列平均 vruntime(源 OS 的 `queue_average_vruntime` 用来算基准)。
 * 队列为空时返回 fallback */
u64 sched_queue_avg_vruntime(const sched_queue_t *q, tcb_t ignore, u64 fallback);

/* ------------------------------------------------------------------ */
/* 内核侧(M4-8.3,实现在 src/sched_kern.c)                            */
/* ------------------------------------------------------------------ */

#define SCHED_MAX_SWITCHES_TRACKED 4096u /* 切换次数计数上限(诊断用,不封顶)*/

/*
 * 本核当前线程。**唯一真相在 percpu_t.current_task**;这里只做转换,
 * 不另存一份(存两份就会有"派生量忘了同步"的静默失真)。
 */
tcb_t sched_current(void);
void  sched_set_current(tcb_t t);

/*
 * 把栈池与内核堆绑进来(启动时一次)。不 extern 全局量的理由见 .c。
 */
void sched_kern_bind(kstack_pool_t *ks, heap_t *heap);

/* 清空本核就绪队列并把 current 置空。启动时每个核各调一次 */
void sched_kern_init(void);

/*
 * 造一个内核线程并把本核就绪队列按 deadline 有序插入。
 *
 * TCB 从内核堆取(不是静态数组 —— 线程数不能是编译期常量),
 * 内核栈从 M4-5 的栈池取(带 guard page)。两者任一失败就整体回滚。
 */
tcb_t sched_kthread_create(void (*entry)(void *), void *arg, const char *name);

/*
 * 让出 CPU:给当前线程计费 -> 重新入队 -> 选 deadline 最小者 -> 切换。
 * 当前线程是 idle(或队列里只剩自己)时直接返回,不做无谓的切换。
 */
void sched_yield(void);

/*
 * 造 idle 线程并切过去 —— **不返回**。
 * idle 就是 `for (;;) arch_wfi();`,它的存在让"没有可运行线程"这件事
 * 有一个合法的落点,而不是让调度器去挑一个空队列。
 */
void sched_kern_start(void);

/* 诊断:本核累计切换次数、idle 被调度到的次数 */
u32 sched_switch_count(void);
u32 sched_idle_spins(void);
