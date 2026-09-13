"""调度策略(M4-8.2)的单元测试。

被测代码是 arch/arm32/src/sched.c —— 不含 MMIO/CP15,时间以参数传入,
所以宿主机能直接编译并穷尽测。

**为什么策略的判据必须在这里,而不是在板子上**:

板级证据只能证明**机制**对(切换、抢占、无饥饿、idle),证明不了策略 ——
因为在"等权 + 纯占用"的负载下,**任何策略都会通过**。
拾取顺序、睡醒补偿、队列不变量这些只有在这里才判得动。
(这条分工是 M4-8 开工前拍板时定下来的,见计划 §4.5。)

★ 第 13 节(M4-8.5 无饥饿)是这条分工的**第二个实例**,而且分工在这里
格外清楚:板上那半证的是"K=4 时时间片到点真的换人",这里证的是
"**任何** K 与任何片长下都不会漏掉" —— 那只能穷尽扫。

顺带钉住一条**源 OS 的既有事实**:`vruntime_delta()` 是
`(ns * 1024) / 1024` —— 乘除同一个常数,恒等于原值。TCB 里也没有权重字段。
也就是源 OS 的调度器**没有权重**,它的 "EEVDF" 实际是
"无权重的虚拟期限公平队列 + 睡醒补偿"。ARM 侧照抄这个恒等式,
不擅自"补全"权重(否则两边调度行为会分叉)。

参考:
  arch/arm32/include/arch/sched.h    设计说明与常量出处
  arch/arm32/src/sched.c             实现
  kernel/task/scheduler.cpp          源 OS 的同名函数
"""

from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

KR_PREFIXED = ("memcpy", "memmove", "memset", "memcmp", "strlen", "strcmp", "strncmp",
               "strcpy", "strncpy", "strcat", "strchr", "strrchr", "strtok", "isdigit", "atoi")

HARNESS = r"""
int printf(const char *fmt, ...);

#include <arch/sched.h>
#include <arch/tcb.h>
#include <krlibc.h>

static int failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL %d: %s\n", __LINE__, #cond);                                              \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

/*
 * 三个 TCB。用静态实例而不是分配器:本测试只关心策略,
 * 构造一个分配器只会把"策略对不对"和"分配器对不对"混在一起。
 */
static struct arm_thread_control_block ta;
static struct arm_thread_control_block tb;
static struct arm_thread_control_block tc;

static tcb_t mk(struct arm_thread_control_block *t, u32 level, u64 vruntime, u64 deadline, u64 slice)
{
    memset(t, 0, sizeof(*t));
    t->task_level     = (i32)level;
    t->status         = RUNNING;
    t->eevdf_vruntime = vruntime;
    t->eevdf_deadline = deadline;
    t->eevdf_slice    = slice;
    return t;
}

/* ------------------------------------------------------------------ */
/* ★ M4-8.5:无饥饿判据的模拟器 ★                                     */
/* ------------------------------------------------------------------ */

/*
 * ## 判据是一句话
 *
 *     一个**可运行**的线程,永远不会被无限期地漏掉。
 *
 * 这里把它落成一个**可测量的二值量**:
 *
 *     把时间切成 N = 64 tick 的窗口;每个窗口里,每个可运行线程的
 *     **自己的计数器**都必须至少涨过一次。整段观测里一次都不许违反。
 *
 * ⚠ 它**不是**公平性/比率判据 —— 计划里明写"任何策略在等权 + 纯占用
 *   负载下都会通过",所以"公平份额 ≈ 理论值"与"EEVDF 与 round-robin 有别"
 *   两条**列为不验**。窗口判据有区分能力,而份额判据没有。
 *
 * ⚠ N 取 64 的算法:`SCHED_TIME_SLICE = 4`,K 个等权纯占用线程每个约每
 *   `4K` tick 轮到一次;K=4 时是 16,取 **4 倍余量**。
 *   写成"刚好够"不会更严,只会**更脆**(负载抖一下就误报)。
 *
 * ## 模拟的是什么(以及故意不模拟什么)
 *
 * `sched_tick` 的骨架:每个 tick 给 current 计数并计费;时间片到点就调
 * `sched_select_next`;换人就把片计数清零。时间以参数传给策略层。
 *
 * ⚠ **故意不模拟队列的进出**:源 OS 的队列是"全部线程的名册",
 *   current 与睡眠者都留在里面(见 sched.h)。在宿主上多模拟一份队列维护,
 *   等于验一个板上不存在的模型。
 */
#define SIM_K_MAX        8u
#define SIM_WINDOW_TICKS 64u  /* ★ 与板上同一个 N ★ */
#define SIM_WINDOWS      16u  /* 结算多少个完整窗口 */

typedef struct
{
    u32 ticks_run; /* ★ 线程**自己的**计数器(与板上那个 count 同义)*/
    u32 miss;      /* 有多少个窗口里它一次都没推进(>0 就是被饿着了)*/
} sim_stat_t;

static struct arm_thread_control_block sim_tcb[SIM_K_MAX];

/*
 * 跑一段模拟,返回**结算过的完整窗口数**。
 * `k` 个线程全部是"永不睡眠的纯占用"(status 恒为 RUNNING)。
 */
static u32 sim_run(u32 k, u64 slice_ns, sim_stat_t *st)
{
    sched_queue_t q;
    tcb_t         cur = NULL;
    u32           ck[SIM_K_MAX];
    u32           slice_used = 0u;
    u32           windows    = 0u;
    u32           total      = (SIM_WINDOWS + 1u) * SIM_WINDOW_TICKS;
    u64           now        = 0u;
    u32           tick;
    u32           i;

    for (i = 0u; i < k; i++) {
        memset(&sim_tcb[i], 0, sizeof(sim_tcb[i]));
        sim_tcb[i].task_level = TASK_KERNEL_LEVEL;
        sim_tcb[i].status     = RUNNING;
        sched_entity_init(&sim_tcb[i], 0u, 0u);
        sim_tcb[i].eevdf_slice = slice_ns;
        st[i].ticks_run        = 0u;
        st[i].miss             = 0u;
        ck[i]                  = 0u;
    }

    sched_queue_init(&q);
    for (i = 0u; i < k; i++) {
        CHECK(sched_queue_append(&q, &sim_tcb[i]));
    }

    for (tick = 0u; tick < total; tick++) {
        now += SCHED_TICK_NS;

        /* ---- 窗口边界:先结算**上一个**窗口,再往下走 ---- */
        if ((tick != 0u) && ((tick % SIM_WINDOW_TICKS) == 0u)) {
            windows++;
            for (i = 0u; i < k; i++) {
                if (st[i].ticks_run == ck[i]) {
                    st[i].miss++; /* ★ 整整一个窗口一次都没推进 ★ */
                }
                ck[i] = st[i].ticks_run;
            }
        }

        if (cur != NULL) {
            st[(u32)(cur - &sim_tcb[0])].ticks_run++; /* 它真的跑了一个 tick */
            slice_used++;
            sched_account_run(cur, SCHED_TICK_NS);
            if (slice_used < SCHED_TIME_SLICE) {
                continue; /* 时间片没到:tick 什么都不做 */
            }
        }

        {
            tcb_t nxt = sched_select_next(&q, cur, now);

            if (nxt == NULL) {
                break;
            }
            cur        = nxt;
            slice_used = 0u;
        }
    }

    return windows;
}

int main(void)
{
    sched_queue_t q;

    /* ---- 1. 常量必须与源 OS 逐个一致(`scheduler.cpp:17-24`)---- */
    CHECK(SCHED_TICK_NS == 1000000ull);
    CHECK(SCHED_TIME_SLICE == 4ull && SCHED_MIN_SLICE == 1ull);
    CHECK(SCHED_BASE_SLICE_NS == 4000000ull);   /* 4 ms */
    CHECK(SCHED_MIN_SLICE_NS == 1000000ull);    /* 1 ms */
    CHECK(SCHED_WAKEUP_CREDIT == 4000000ull);
    CHECK(SCHED_SLEEPER_CREDIT == 8000000ull);  /* 2 x base */
    CHECK(SCHED_DEFAULT_WEIGHT == 1024ull);

    /* ---- 2. sched_slice:小于下限就回落,不是夹到区间里 ---- */
    mk(&ta, TASK_KERNEL_LEVEL, 0, 0, SCHED_BASE_SLICE_NS);
    CHECK(sched_slice(&ta) == SCHED_BASE_SLICE_NS);
    ta.eevdf_slice = SCHED_MIN_SLICE_NS;            /* == 下限,保留 */
    CHECK(sched_slice(&ta) == SCHED_MIN_SLICE_NS);
    ta.eevdf_slice = SCHED_MIN_SLICE_NS - 1u;       /* < 下限,回落 */
    CHECK(sched_slice(&ta) == SCHED_BASE_SLICE_NS);
    ta.eevdf_slice = 0u;
    CHECK(sched_slice(&ta) == SCHED_BASE_SLICE_NS);
    CHECK(sched_slice(NULL) == SCHED_BASE_SLICE_NS);

    /* ---- 3. ★ vruntime_delta 是恒等式 —— 源 OS 的事实,照抄 ★ ---- */
    CHECK(sched_vruntime_delta(0u) == 0u);
    CHECK(sched_vruntime_delta(1u) == 1u);
    CHECK(sched_vruntime_delta(1234567u) == 1234567u);
    CHECK(sched_vruntime_delta(SCHED_BASE_SLICE_NS) == SCHED_BASE_SLICE_NS);

    /* ---- 4. deadline_before:deadline 优先,同 deadline 比 vruntime ---- */
    mk(&ta, TASK_KERNEL_LEVEL, 100u, 500u, SCHED_BASE_SLICE_NS);
    mk(&tb, TASK_KERNEL_LEVEL, 100u, 400u, SCHED_BASE_SLICE_NS);
    CHECK(sched_deadline_before(&tb, &ta));       /* deadline 小的在前 */
    CHECK(!sched_deadline_before(&ta, &tb));

    tb.eevdf_deadline = 500u;                     /* 同 deadline -> 比 vruntime */
    tb.eevdf_vruntime = 50u;
    CHECK(sched_deadline_before(&tb, &ta));
    CHECK(!sched_deadline_before(&ta, &tb));

    tb.eevdf_vruntime = 100u;                     /* 完全相同 -> 不是 before */
    CHECK(!sched_deadline_before(&tb, &ta));
    CHECK(!sched_deadline_before(&ta, &tb));

    /* NULL 语义照抄源 OS */
    CHECK(sched_deadline_before(&ta, NULL));
    CHECK(!sched_deadline_before(NULL, &ta));
    CHECK(sched_deadline_before(NULL, NULL));     /* 源 OS:right==NULL 先判,返回 true */

    /* ---- 5. sched_account_run:三条早退 + 记账 ---- */
    mk(&ta, TASK_KERNEL_LEVEL, 1000u, 0u, SCHED_BASE_SLICE_NS);
    ta.status = RUNNING;
    sched_account_run(&ta, 2000000ull);           /* 2 ms */
    CHECK(ta.eevdf_vruntime == 1000u + 2000000ull);
    CHECK(ta.eevdf_deadline == ta.eevdf_vruntime + SCHED_BASE_SLICE_NS);

    /* runtime 为 0:不记账 */
    {
        u64 before = ta.eevdf_vruntime;
        sched_account_run(&ta, 0u);
        CHECK(ta.eevdf_vruntime == before);
    }
    /* 非 RUNNING:不记账 */
    mk(&tb, TASK_KERNEL_LEVEL, 500u, 0u, SCHED_BASE_SLICE_NS);
    tb.status = START;
    sched_account_run(&tb, 2000000ull);
    CHECK(tb.eevdf_vruntime == 500u);
    /* WAIT 也一样 */
    tb.status = WAIT;
    sched_account_run(&tb, 2000000ull);
    CHECK(tb.eevdf_vruntime == 500u);
    /* ★ idle 任务完全不记账 —— 否则 idle 会把平均 vruntime 拖飞 ★ */
    mk(&tc, TASK_IDLE_LEVEL, 700u, 0u, SCHED_BASE_SLICE_NS);
    tc.status = RUNNING;
    sched_account_run(&tc, 9000000ull);
    CHECK(tc.eevdf_vruntime == 700u);
    sched_account_run(NULL, 1000u); /* 不崩即可 */

    /* ---- 6. 睡醒补偿:夹在 [4ms, 8ms],且只往前拨不往后拉 ---- */
    mk(&ta, TASK_KERNEL_LEVEL, 100000000ull, 0u, SCHED_BASE_SLICE_NS);
    sched_apply_wakeup_credit(&ta, 50000000ull);  /* base 50ms,credit 4ms */
    CHECK(ta.eevdf_vruntime == 50000000ull - SCHED_WAKEUP_CREDIT);
    CHECK(ta.eevdf_deadline == ta.eevdf_vruntime + SCHED_BASE_SLICE_NS);

    /* ★ 只往前拨:vruntime 已经比 placed 小的时候不许被拉大 ★ */
    mk(&ta, TASK_KERNEL_LEVEL, 1000u, 0u, SCHED_BASE_SLICE_NS);
    sched_apply_wakeup_credit(&ta, 90000000ull);
    CHECK(ta.eevdf_vruntime == 1000u);            /* 原值保留,没被拉大 */

    /* base 小于 credit 时 placed 夹到 0,不能下溢 */
    mk(&ta, TASK_KERNEL_LEVEL, 90000000ull, 0u, SCHED_BASE_SLICE_NS);
    sched_apply_wakeup_credit(&ta, 1000000ull);   /* base 1ms < credit 4ms */
    CHECK(ta.eevdf_vruntime == 0u);

    /* 片长大时 credit 被夹到 8ms 上限 */
    mk(&ta, TASK_KERNEL_LEVEL, 100000000ull, 0u, 1000000000ull); /* slice 1s */
    sched_apply_wakeup_credit(&ta, 50000000ull);
    CHECK(ta.eevdf_vruntime == 50000000ull - SCHED_SLEEPER_CREDIT);

    /* idle 不享受补偿 */
    mk(&tc, TASK_IDLE_LEVEL, 12345u, 0u, SCHED_BASE_SLICE_NS);
    sched_apply_wakeup_credit(&tc, 90000000ull);
    CHECK(tc.eevdf_vruntime == 12345u);

    /* ---- 7. sched_wake_if_due ---- */
    mk(&ta, TASK_KERNEL_LEVEL, 50000000ull, 0u, SCHED_BASE_SLICE_NS);
    ta.status      = WAIT;
    ta.wakeup_time = 10000000ull;                 /* 10 ms */
    CHECK(!sched_wake_if_due(&ta, 9999999ull, 50000000ull)); /* 还差 1ns */
    CHECK(ta.status == WAIT);
    CHECK(sched_wake_if_due(&ta, 10000000ull, 50000000ull)); /* 正好到点 */
    CHECK(ta.status == START);
    CHECK(ta.wakeup_time == 0u);
    CHECK(ta.eevdf_vruntime == 50000000ull - SCHED_WAKEUP_CREDIT);
    /* 再调一次:已经不是 WAIT 了 */
    CHECK(!sched_wake_if_due(&ta, 20000000ull, 50000000ull));

    /* wakeup_time == 0 表示"不按时间唤醒",不是"立即唤醒" */
    mk(&tb, TASK_KERNEL_LEVEL, 0u, 0u, SCHED_BASE_SLICE_NS);
    tb.status      = WAIT;
    tb.wakeup_time = 0u;
    CHECK(!sched_wake_if_due(&tb, 999999999ull, 0u));
    CHECK(tb.status == WAIT);
    CHECK(!sched_wake_if_due(NULL, 1u, 1u));

    /* ---- 8. 就绪队列:★ 插入序 FIFO（照源 OS），查重、摘除、计数 ---- */
    /*
     * ⚠ 这一节在 M4-8.4 被**改写过**，因为 M4-8 的"按 deadline 有序插入、
     *   队首就是应选者"是**我自己发明的**：
     *
     *     源 OS 的队列（`lock_queue`）是普通 FIFO 追加
     *     （`queue_enqueue` → `queue_append_node`，`kernel/lock_queue.cpp:100`），
     *     **选取靠全表扫描**（`select_next_task_safe`）：先算 avg_vruntime，
     *     再要求 `candidate->vruntime <= avg`，在合格者里取 deadline 最小。
     *
     *   排序版不只是"不一样"，它还有个直接后果：**唤醒睡眠任务的那次扫描
     *   在 `select_next_task_safe` 里**，取队首的写法里根本没有它 ⇒
     *   睡下去的任务永远醒不过来。
     */
    sched_queue_init(&q);
    CHECK(q.head == NULL && q.count == 0u);

    /* deadline 故意给成**逆序**，这样"排序"与"追加"一眼可分 */
    mk(&ta, TASK_KERNEL_LEVEL, 100u, 3000u, SCHED_BASE_SLICE_NS);
    mk(&tb, TASK_KERNEL_LEVEL, 200u, 1000u, SCHED_BASE_SLICE_NS);
    mk(&tc, TASK_KERNEL_LEVEL, 300u, 2000u, SCHED_BASE_SLICE_NS);

    CHECK(sched_queue_append(&q, &ta));
    CHECK(sched_queue_append(&q, &tb));
    CHECK(sched_queue_append(&q, &tc));
    CHECK(q.count == 3u);
    /* ★ 保持**插入序**，不按 deadline 排 ★ */
    CHECK(q.head == &ta && ta.sched_next == &tb && tb.sched_next == &tc && tc.sched_next == NULL);

    /* ★ 重复插入必须被拒 —— 挂两次会把链表做成环,遍历就死循环了 ★ */
    CHECK(!sched_queue_append(&q, &tb));
    CHECK(!sched_queue_append(&q, &ta));
    CHECK(q.count == 3u);
    CHECK(q.head == &ta);

    /* 摘中间一个 */
    sched_queue_remove(&q, &tb);
    CHECK(q.count == 2u);
    CHECK(ta.sched_next == &tc);
    CHECK(tb.sched_next == NULL);
    /* 摘了再插:追加到**尾部**(不是插回原位) */
    CHECK(sched_queue_append(&q, &tb));
    CHECK(q.head == &ta && ta.sched_next == &tc && tc.sched_next == &tb);
    CHECK(q.count == 3u);

    /* 摘不存在的,不动 */
    {
        struct arm_thread_control_block tx;
        mk(&tx, TASK_KERNEL_LEVEL, 0u, 0u, SCHED_BASE_SLICE_NS);
        sched_queue_remove(&q, &tx);
        CHECK(q.count == 3u);
    }
    /* 摘光 */
    sched_queue_remove(&q, &tc);
    sched_queue_remove(&q, &tb);
    sched_queue_remove(&q, &ta);
    CHECK(q.count == 0u && q.head == NULL);
    /* 重复摘除不会把计数减成负数 */
    sched_queue_remove(&q, &ta);
    CHECK(q.count == 0u);
    CHECK(!sched_queue_append(NULL, &ta));
    CHECK(!sched_queue_append(&q, NULL));

    /* 空队列与 NULL 的边界 */
    sched_queue_init(NULL);
    sched_queue_remove(NULL, &ta);
    sched_queue_remove(&q, NULL);

    /* ---- 9. 平均 vruntime(源 OS 用它给新线程定起点)---- */
    sched_queue_init(&q);
    mk(&ta, TASK_KERNEL_LEVEL, 1000u, 10u, SCHED_BASE_SLICE_NS);
    mk(&tb, TASK_KERNEL_LEVEL, 3000u, 20u, SCHED_BASE_SLICE_NS);
    sched_queue_append(&q, &ta);
    sched_queue_append(&q, &tb);
    CHECK(sched_queue_avg_vruntime(&q, NULL, 0u) == 2000u);
    CHECK(sched_queue_avg_vruntime(&q, &ta, 0u) == 3000u);   /* 排除自己 */
    CHECK(sched_queue_avg_vruntime(&q, &ta, 777u) == 3000u);
    sched_queue_remove(&q, &tb);
    CHECK(sched_queue_avg_vruntime(&q, &ta, 777u) == 777u);  /* 空 -> fallback */
    CHECK(sched_queue_avg_vruntime(NULL, NULL, 42u) == 42u);

    /* ---- 10. sched_entity_init ← `init_task_eevdf_entity` 逐值对齐(D15)---- */
    /*
     * ★ 这一节在 M4-10.1 之前是**钉错了**:它断言 `vruntime == base`,
     *   而源 OS 是 `vruntime = base - EEVDF_WAKEUP_CREDIT`(`scheduler.cpp:294`)。
     *   宿主单测当时"通过"只是因为判据跟着实现一起写错了 ——
     *   这正是本项目的规程要防的那类事:判据必须回到**源 OS**去核对,
     *   而不是回到"我以前是怎么写的"。
     */
    mk(&ta, TASK_KERNEL_LEVEL, 0u, 0u, 0u);
    ta.eevdf_deadline = 12345u;
    sched_entity_init(&ta, 7000000ull, 2222ull);
    CHECK(ta.eevdf_vruntime == 7000000ull - SCHED_WAKEUP_CREDIT); /* ★ 减掉一个补偿 ★ */
    CHECK(ta.eevdf_slice == SCHED_BASE_SLICE_NS);                 /* 没设过 ⇒ 回落基础片长 */
    CHECK(ta.eevdf_deadline == ta.eevdf_vruntime + SCHED_BASE_SLICE_NS);
    CHECK(ta.eevdf_last_start == 2222ull); /* now 只记诊断量,不参与起点 */
    CHECK(ta.runtime_ticks == 0u);
    CHECK(ta.sched_next == NULL);

    /* base <= credit 时钳到 0,不是下溢(源 OS 的三元表达式) */
    sched_entity_init(&ta, 0u, 1ull);
    CHECK(ta.eevdf_vruntime == 0u);
    sched_entity_init(&ta, SCHED_WAKEUP_CREDIT, 1ull);
    CHECK(ta.eevdf_vruntime == 0u); /* 相等也钳到 0(是 >,不是 >=)*/
    sched_entity_init(&ta, SCHED_WAKEUP_CREDIT + 1ull, 1ull);
    CHECK(ta.eevdf_vruntime == 1u);

    /* 片长已经设成下限之上时按它算 deadline;低于下限则回落 */
    mk(&ta, TASK_KERNEL_LEVEL, 0u, 0u, 0u);
    ta.eevdf_slice = 9000000ull; /* 9ms */
    sched_entity_init(&ta, 100000000ull, 0ull);
    CHECK(ta.eevdf_slice == 9000000ull);
    CHECK(ta.eevdf_deadline == ta.eevdf_vruntime + 9000000ull);

    sched_entity_init(NULL, 1u, 1u); /* 不崩即可 */

    /* ================================================================ */
    /* 11. 可调度性判据(M4-9)                                           */
    /* ================================================================ */

    /*
     * ← is_task_schedulable / is_current_task_runnable。
     *
     * 这一段的重点是那条**不对称**:两者对 idle 的处理不同 ——
     * 挑别人时 idle 不算候选,而 idle 当 current 时算"能跑"。
     * 抄成一个函数会让调度器要么永远挑不到 idle(启动流程回不来),
     * 要么永远只挑 idle(真实线程拿不到 CPU)。两种都不会报错。
     */
    CHECK(sched_status_runnable(RUNNING));
    CHECK(sched_status_runnable(START));
    CHECK(sched_status_runnable(CREATE));
    CHECK(!sched_status_runnable(WAIT));
    CHECK(!sched_status_runnable(DEATH));
    CHECK(!sched_status_runnable(FUTEX));
    CHECK(!sched_status_runnable(OUT));
    CHECK(!sched_status_runnable(ZOMBIE));

    sched_queue_init(&q);

    /* 排三个:一个普通、一个 idle、一个挂起 */
    mk(&ta, TASK_KERNEL_LEVEL, 100u, 10u, SCHED_BASE_SLICE_NS); /* 普通,可调度 */
    mk(&tb, TASK_IDLE_LEVEL, 0u, 0u, SCHED_BASE_SLICE_NS);      /* idle:不是候选 */
    mk(&tc, TASK_KERNEL_LEVEL, 200u, 20u, SCHED_BASE_SLICE_NS);
    tc.status = WAIT;                                           /* 挂起:不是候选 */

    /* idle 的 deadline 最小(0)，排在队首 —— 正是"取队首"会错的地方 */
    sched_queue_append(&q, &tb);
    sched_queue_append(&q, &ta);
    sched_queue_append(&q, &tc);
    CHECK(q.head == &tb); /* 队首是 idle */

    /*
     * ★ `sched_select_next` 必须跳过 idle 与挂起的，挑到 ta ★
     *
     * 注意判据里带了 `now`：这个函数会**唤醒到点的睡眠任务**，
     * 所以时间是个输入（纯逻辑层的纪律：宿主上要能构造"过了 3ms"）。
     */
    CHECK(sched_select_next(&q, NULL, 0u) == &ta);
    /* current 自己不算候选;队列里没有别人可跑时兜底回 current */
    CHECK(sched_select_next(&q, &ta, 0u) == &ta);
    CHECK(sched_select_next(NULL, NULL, 0u) == NULL);
    /*
     * current = tc(WAIT,不可运行)：队列里还有可跑的 ta ⇒ 挑 ta。
     * ⚠ idle **不是**第一顺位 —— 它只在"没有任何候选、current 也不能跑"
     *   时才兜底（见 11c）。第一版这里写成了 `== &tb`(idle)，是判据写错了。
     */
    CHECK(sched_select_next(&q, &tc, 0u) == &ta);

    CHECK(!sched_task_schedulable(NULL, NULL));
    CHECK(!sched_task_schedulable(&tb, NULL)); /* idle 永不作为候选 */
    CHECK(!sched_task_schedulable(&ta, &ta));  /* 自己不算 */
    CHECK(sched_task_schedulable(&ta, NULL));
    CHECK(sched_task_schedulable(&ta, &tc));
    CHECK(!sched_task_schedulable(&tc, NULL)); /* WAIT */

    /* ★ 不对称在这里:同一个 idle 对象,作为 current 时是"能跑"的 ★ */
    CHECK(sched_current_runnable(&tb));
    CHECK(sched_current_runnable(&ta));
    CHECK(!sched_current_runnable(&tc));
    CHECK(!sched_current_runnable(NULL));

    /* ================================================================ */
    /* 11b. ★ M4-8.4:扫描里的**唤醒** ★                                */
    /* ================================================================ */

    /*
     * 这是 M4-8.4 最要紧的一条判据，也是"取队首"那一版**做不到**的事：
     * 睡下去的任务留在队列里，靠 `sched_queue_scan()` 在**选取的过程中**
     * 被扫到并唤醒。
     *
     * 判据分两半，缺一不可：
     *   - 没到点：**不许**醒（早醒了就变成忙等，睡醒补偿也会失真）；
     *   - 到点  ：**必须**醒，而且醒来后成为候选。
     */
    {
        tcb_t fb = NULL;
        tcb_t id = NULL;

        sched_queue_init(&q);
        /* 一个睡眠任务：wakeup_time = 5000 */
        mk(&ta, TASK_KERNEL_LEVEL, 1000u, 1000u, SCHED_BASE_SLICE_NS);
        ta.status      = WAIT;
        ta.wakeup_time = 5000ull;

        /* 一个就绪任务：vruntime 大一点，用来当 current 的参照 */
        mk(&tb, TASK_KERNEL_LEVEL, 9000u, 9000u, SCHED_BASE_SLICE_NS);

        sched_queue_append(&q, &ta);
        sched_queue_append(&q, &tb);

        /* --- 未到点：扫描不许把它弄醒 --- */
        (void)sched_queue_scan(&q, &tb, 4999ull, &fb, &id);
        CHECK(ta.status == WAIT);
        CHECK(ta.wakeup_time == 5000ull);
        CHECK(ta.eevdf_vruntime == 1000u); /* 补偿没被施加 */
        /* 睡眠中 ⇒ 不是候选 */
        CHECK(fb == NULL);

        /* --- 到点：扫描必须唤醒，并给睡醒补偿 --- */
        (void)sched_queue_scan(&q, &tb, 5000ull, &fb, &id);
        CHECK(ta.status == START);
        CHECK(ta.wakeup_time == 0ull);
        /*
         * ← `apply_eevdf_wakeup_credit(task, base=tb.vruntime=9000)`：
         *     credit = 自己的片长（4ms）夹到 [4ms, 8ms] ⇒ 4000000
         *     placed = 9000 - 4000000 ⇒ 下溢 ⇒ 0
         *     ta.vruntime = min(1000, 0) = 0
         */
        CHECK(ta.eevdf_vruntime == 0u);
        CHECK(ta.eevdf_deadline == 0u + SCHED_BASE_SLICE_NS);
        /* 醒了就是候选 */
        CHECK(fb == &ta);

        /* --- 醒来的任务必须真的能被选中 --- */
        CHECK(sched_select_next(&q, &tb, 5000ull) == &ta);
    }
    CHECK(sched_queue_scan(NULL, NULL, 0u, NULL, NULL) == 0u);
    CHECK(sched_queue_scan(&q, NULL, 1u, NULL, NULL) == 0u || 1u); /* 不崩即可 */

    /* ---- 11c. `sched_select_next` 的兜底链 ---- */
    {
        sched_queue_init(&q);
        mk(&ta, TASK_KERNEL_LEVEL, 0u, 0u, SCHED_BASE_SLICE_NS);
        ta.status = WAIT; /* 唯一的任务在睡，队列里没有候选 */
        sched_queue_append(&q, &ta);

        /* current 可运行 ⇒ 继续跑它 */
        mk(&tb, TASK_KERNEL_LEVEL, 100u, 100u, SCHED_BASE_SLICE_NS);
        CHECK(sched_select_next(&q, &tb, 0u) == &tb);
        /* current 不可运行、也没有 idle ⇒ NULL */
        tb.status = WAIT;
        CHECK(sched_select_next(&q, &tb, 0u) == NULL);

        /* 队列里放个 idle：兜底就该挑它 */
        mk(&tc, TASK_IDLE_LEVEL, 0u, 0u, SCHED_BASE_SLICE_NS);
        sched_queue_append(&q, &tc);
        CHECK(sched_select_next(&q, &tb, 0u) == &tc);
    }

    /* ================================================================ */
    /* 12. ★ M4-9:帧搬迁 ★                                             */
    /* ================================================================ */

    /*
     * 这四条要钉住的是那条**架构事实**:
     *
     *     ARM 的异常帧里没有 SP,返回后的 SP 恒等于"帧基址 + 64"。
     *     ⇒ 帧在谁的栈上,谁就被恢复。
     *
     * 所以"搬迁"这件事的全部内容就是两个方向的数据搬运,
     * 而它们必须是**互逆**的 —— 收现场再铺回去,ctx 必须一字不差。
     * 这一条只能在宿主上穷尽测:板上只能证明"跑起来了",
     * 证明不了"每个字段都对"。
     */

    /* 12a. 帧布局:结构体字段偏移必须与汇编共用的一致 */
    CHECK(sizeof(arm_exc_frame_t) == ARM_EXC_FRAME_BYTES);
    CHECK(offsetof_arm(arm_exc_frame_t, r) == ARM_EXC_OFF_R0);
    CHECK(offsetof_arm(arm_exc_frame_t, svc_lr) == ARM_EXC_OFF_SVC_LR);
    CHECK(offsetof_arm(arm_exc_frame_t, ret) == ARM_EXC_OFF_RET);
    CHECK(offsetof_arm(arm_exc_frame_t, spsr) == ARM_EXC_OFF_SPSR);
    CHECK(sizeof(arm_task_ctx_t) == ARM_CTX_BYTES);

    {
        /*
         * ⚠ 这里有一个**宿主特有的**陷阱,必须说清楚,否则这一段的写法
         *   看起来像是"判据放宽了"。
         *
         * `arm_task_ctx_t.sp` 是 **u32** —— 这是目标板的 ABI(ARM32 的
         * 地址就是 32 位,而且汇编按这个宽度访问它)。宿主的指针却是 8 字节,
         * 于是"把一个真指针存进 ctx.sp"在宿主上会**截断**。
         *
         * 本项目在 vmap 的 l2_pool 上已经因为同样的事踩过一次
         * (`u32` 装指针 → 野地址)。所以:
         *
         *   - 帧本身用**真指针**(要真的读写那 64 字节);
         *   - 凡是与 `ctx.sp` 比较的地方,期望值也按 `(u32)` 截断一次 ——
         *     **目标板上那个截断是空操作**,于是同一条判据在两边都成立、
         *     而且都是精确相等,不是"近似"或"跳过"。
         *
         * 换句话说:判据没有放宽,只是把"目标板上恒等的那个转换"写了出来。
         */
        static u64        arena[32];
        arm_exc_frame_t  *fr = (arm_exc_frame_t *)(void *)&arena[4]; /* 8 字节对齐 */
        u32               sp32;
        struct arm_thread_control_block t;
        u32               i;

        CHECK(sizeof(arena) >= 2u * ARM_EXC_FRAME_BYTES);
        CHECK(((uintptr_t)fr & 7u) == 0u);

        sp32 = (u32)(uintptr_t)fr + ARM_EXC_FRAME_BYTES; /* 目标板上就是入口那一刻的 sp */
        memset(&t, 0, sizeof(t));

        /* ---- 12b. sched_ctx_switchable:四条拒绝理由,逐条钉 ---- */

        /* (1) pc == 0 = 上下文无效(启动上下文在被第一次切走之前)
         *     ⚠ cpsr 给一个**合法**值,否则这一条会同时踩中理由 2,
         *       判据就不专一了(失败时看不出是哪一条拦下的)。*/
        t.ctx.pc   = 0u;
        t.ctx.sp   = sp32;
        t.ctx.cpsr = ARM_CPSR_KERNEL;
        CHECK(!sched_ctx_switchable(&t));
        CHECK(sched_frame_for(&t) == NULL);

        /* (2) ★ cpsr 的 A / T / 模式位必须与内核一致 ★
         *
         *     这一条是被一次真实的上板故障逼出来的,判据也照着那次事故写:
         *
         *       内核真实 CPSR(被抢占时从现场帧收进来的)= 0x80000153
         *       我手写的新线程 CPSR                      = 0x00000053
         *                                                 ^^^ bit8 = A
         *
         *     `msr cpsr_c` 只含 bits[7:0],所以 start.S 那句
         *     `MODE_SVC|I_BIT|F_BIT` 从来没动过 A;A 是复位值 1。
         *     把它写成 0 = 在切过去的一瞬间解除了异步外部中止的屏蔽,
         *     于是第一条指令就吃了 FS=0x16 的异步中止(而 DFAR 是垃圾值)。
         */
        CHECK(ARM_CPSR_KERNEL == 0x153u);
        CHECK((ARM_CPSR_KERNEL & ARM_CPSR_MODE_MASK) == ARM_MODE_SVC);
        CHECK((ARM_CPSR_KERNEL & ARM_CPSR_A_BIT) != 0u); /* ★ A=1 ★ */
        CHECK((ARM_CPSR_KERNEL & ARM_CPSR_F_BIT) != 0u); /* FIQ 屏蔽 */
        CHECK((ARM_CPSR_KERNEL & ARM_CPSR_I_BIT) == 0u); /* ★ I=0,否则抢占无效 ★ */
        CHECK((ARM_CPSR_KERNEL & ARM_CPSR_T_BIT) == 0u); /* ARM 态 */

        /* 板上实测到的那个真值必须满足同一条谓词 */
        CHECK(sched_cpsr_matches_kernel(0x80000153u)); /* idle 被抢占时收的现场 */
        CHECK(sched_cpsr_matches_kernel(0x00000153u));
        /* ★ 我第一版拼出来的那个值必须被拒绝 ★ */
        CHECK(!sched_cpsr_matches_kernel(0x00000053u));
        /* I 位不参与:关中断的临界区里发生 svc 陷阱是合法的 */
        CHECK(sched_cpsr_matches_kernel(0x000001D3u)); /* A|I|F|SVC */
        /*
         * ★ T 位也不参与 ★ —— 这一条是**板上打回来的第二次**。
         *
         * 第一版把 T 纳入了判定,理由是"内核是 .arm 编的,T 必然是 0"。
         * 上板立刻否掉:一个被抢占的探针线程 ctx.cpsr 读回来是 0x173
         * (**T=1**),pc 落在 `__udivmoddi4` 里 —— 那是 libgcc 的 **Thumb**
         * 代码,而 `timer_read_us()` 的 64 位除法就会进到它里面。
         * 于是每一个这样的上下文都被拒绝,invalid_ctx 涨到 45965,
         * 线程卡在就绪队列里,启动流程再也回不来。
         */
        CHECK(sched_cpsr_matches_kernel(0x00000173u)); /* A|T|F|SVC:Thumb 态 */
        CHECK(sched_cpsr_matches_kernel(0x20000173u)); /* 板上实测的那个值 */
        /* 模式位参与 */
        CHECK(!sched_cpsr_matches_kernel(0x00000153u & ~ARM_CPSR_MODE_MASK));
        CHECK(!sched_cpsr_matches_kernel(0x00000153u & ~ARM_CPSR_A_BIT));

        t.ctx.pc   = 0x1234u;
        t.ctx.cpsr = 0u;
        CHECK((0u & ARM_CPSR_MODE_MASK) != ARM_MODE_SVC);
        /* 0 也不是 User:每个合法模式的 bit4 都是 1 */
        CHECK((0u & ARM_CPSR_MODE_MASK) != ARM_MODE_USR);
        CHECK(!sched_ctx_switchable(&t));
        t.ctx.cpsr = ARM_MODE_USR;
        CHECK(!sched_ctx_switchable(&t));
        t.ctx.cpsr = ARM_MODE_IRQ;
        CHECK(!sched_ctx_switchable(&t));
        t.ctx.cpsr = ARM_MODE_SYS; /* 特权,但**不是** SVC —— 同样拒绝 */
        CHECK(!sched_ctx_switchable(&t));
        /* ★ A=0 必须被拒绝 —— 这正是那次上板故障的形态 ★ */
        t.ctx.cpsr = 0x00000053u;
        CHECK(!sched_ctx_switchable(&t));
        /* 内核的规范值接受 */
        t.ctx.cpsr = ARM_CPSR_KERNEL; /* 0x153 */
        CHECK(sched_ctx_switchable(&t));
        /* I=1 也接受 —— 只看 A/模式域(见 ARM_CPSR_MUST_MATCH 的说明)*/
        t.ctx.cpsr = ARM_CPSR_KERNEL | ARM_CPSR_I_BIT;
        CHECK(sched_ctx_switchable(&t));
        /* ★ T=1 也接受 —— libgcc 的 Thumb 代码就是这种形态 ★ */
        t.ctx.cpsr = 0x20000173u;
        CHECK(sched_ctx_switchable(&t));

        /* (3) sp 必须 **4** 字节对齐(帧里全是 u32)与非 0
         *
         *     ⚠ 不是 8 —— 这一条也是上板打回来的。被中断的代码在 libgcc 的
         *       Thumb 函数 `__udivmoddi4` 里(`push {r4,r5,lr}` = 12 字节),
         *       函数体内 SP 合法地是 4 mod 8;AAPCS 只要求**调用点** 8 字节
         *       对齐,而异常可以落在任意指令边界。
         *       按 8 判会把每一个这样的上下文都拒掉 —— 实测 invalid_ctx
         *       涨到 30716,线程永远卡在就绪队列里。 */
        t.ctx.cpsr = ARM_CPSR_KERNEL;
        t.ctx.sp   = sp32 + 2u; /* 连 4 都不对齐:拒绝 */
        CHECK(!sched_ctx_switchable(&t));
        t.ctx.sp = sp32 + 4u; /* 4 对齐但 4 mod 8:**接受**(板上就是这种) */
        CHECK(sched_ctx_switchable(&t));
        t.ctx.sp = sp32 + 4u + 0x1000u;
        CHECK(((t.ctx.sp & 7u) != 0u));
        CHECK(sched_ctx_switchable(&t));
        t.ctx.sp = 0u;
        CHECK(!sched_ctx_switchable(&t));

        /* (4) 栈必须落在自己那块栈区里(仅当栈来自栈池) */
        t.ctx.sp       = sp32;
        t.owns_kstack  = true;
        t.kstack_base  = sp32 - ARM_EXC_FRAME_BYTES; /* 帧整体刚好贴着栈底:合法 */
        t.kernel_stack = sp32;
        CHECK(sched_ctx_switchable(&t));
        t.kstack_base = sp32 - ARM_EXC_FRAME_BYTES + 4u; /* 帧会有一半落进 guard 页:拒绝 */
        CHECK(!sched_ctx_switchable(&t));
        t.kstack_base = sp32 + 8u; /* sp 在栈区之外:拒绝 */
        CHECK(!sched_ctx_switchable(&t));
        t.kernel_stack = sp32 - 8u; /* sp 超出栈顶:拒绝 */
        t.kstack_base  = 0u;
        CHECK(!sched_ctx_switchable(&t));

        /*
         * ⚠ 启动上下文(owns_kstack == false)**不做范围检查**:
         *   它的两个边界是注册那一刻的快照,而启动流程在任何深度都可能
         *   被中断 —— 拿快照当区间会把"kmain 跑到更浅的调用深度"
         *   误判成"栈指针是野的",于是启动流程再也回不来。
         */
        t.owns_kstack  = false;
        t.kstack_base  = 0u;
        t.kernel_stack = 0u;
        CHECK(sched_ctx_switchable(&t));

        CHECK(!sched_ctx_switchable(NULL));

        /* ---- 12c. sched_frame_for:帧就在 sp - 64 ---- */
        t.ctx.sp = sp32;
        CHECK((uintptr_t)sched_frame_for(&t) == (uintptr_t)(u32)(uintptr_t)fr);
        /* 帧地址与 sp 同余,所以"对齐"只需要 sp 一条就够 */
        CHECK((((uintptr_t)sched_frame_for(&t)) & 7u) == 0u);

        /* ---- 12d. ★ 收现场:帧 -> ctx ★ ---- */
        memset(fr, 0, sizeof(*fr));
        for (i = 0u; i < 13u; i++) {
            fr->r[i] = 0xA0000000u + i;
        }
        fr->svc_lr = 0xDEADBEEFu;
        fr->ret    = 0x00101904u;
        fr->spsr   = ARM_CPSR_KERNEL; /* 被中断时内核的真实 CPSR(含 A=1)*/

        memset(&t, 0, sizeof(t));
        sched_ctx_from_frame(&t, fr);
        for (i = 0u; i < 13u; i++) {
            CHECK(t.ctx.r[i] == 0xA0000000u + i);
        }
        CHECK(t.ctx.lr == 0xDEADBEEFu);
        CHECK(t.ctx.pc == 0x00101904u);
        CHECK(t.ctx.cpsr == ARM_CPSR_KERNEL);
        /*
         * ★★ 这一行就是"搬帧"的全部内容 ★★
         *
         *   帧基址 + 64 == 异常入口那一刻的 sp
         *   (srsdb -8、push lr -4、push {r0-r12} -52,共 64 字节)
         *
         * 反过来说:因为 `EXC_FRAME_LEAVE` 的收尾是
         * `add sp,sp,#0x38; rfeia sp!`,所以返回后的 sp **恒等于帧基址 + 64**
         * —— "帧搭在谁的栈上,谁就被恢复"就是这么来的。
         */
        CHECK(t.ctx.sp == (u32)(uintptr_t)fr + ARM_EXC_FRAME_BYTES);

        /* 收了现场之后这个上下文就可切换;而它的帧正好落回原处(原地往返)*/
        CHECK(sched_ctx_switchable(&t));
        CHECK((uintptr_t)sched_frame_for(&t) == (uintptr_t)(u32)(uintptr_t)fr);

        /* ---- 12e. ★ 铺回去:ctx -> 帧,必须与 12d 互逆 ★ ---- */
        {
            arm_exc_frame_t fr2;

            memset(&fr2, 0, sizeof(fr2));
            CHECK(sched_frame_from_ctx(&t, &fr2) == &fr2);

            for (i = 0u; i < 13u; i++) {
                CHECK(fr2.r[i] == 0xA0000000u + i);
            }
            CHECK(fr2.svc_lr == 0xDEADBEEFu);
            CHECK(fr2.ret == 0x00101904u);
            CHECK(fr2.spsr == ARM_CPSR_KERNEL);

            /* 逐字节相同:16 个字全部被写到,没有哪个槽漏了 */
            CHECK(memcmp(&fr2, fr, sizeof(fr2)) == 0);
        }

        /* NULL 参数不崩,也不写坏东西 */
        sched_ctx_from_frame(NULL, fr);
        sched_ctx_from_frame(&t, NULL);
        CHECK(sched_frame_from_ctx(NULL, fr) == NULL);
        CHECK(sched_frame_from_ctx(&t, NULL) == NULL);
        (void)sched_frame_from_ctx(&t, fr); /* 复原 */

        /* ---- 12f. 往返:ctx -> 帧 -> ctx' ---- */
        {
            struct arm_thread_control_block t2;
            arm_exc_frame_t                fr3;
            u32                            k;

            memset(&t2, 0, sizeof(t2));
            t2.ctx = t.ctx;

            (void)sched_frame_from_ctx(&t2, &fr3);
            memset(&t, 0, sizeof(t));
            sched_ctx_from_frame(&t, &fr3);

            CHECK(t.ctx.pc == t2.ctx.pc);
            CHECK(t.ctx.lr == t2.ctx.lr);
            CHECK(t.ctx.cpsr == t2.ctx.cpsr);
            for (k = 0u; k < 13u; k++) {
                CHECK(t.ctx.r[k] == t2.ctx.r[k]);
            }

            /*
             * ★ sp **不**随帧搬运 ★ —— 它是从帧的**位置**算出来的。
             *
             * 这不是缺陷,正是设计:ARM 的异常帧里根本没有 sp 这个槽
             * (SP 按模式 banked,硬件不压),所以"帧在哪"就等价于"sp 是多少"。
             * 于是往返之后 sp 指向 fr3,而不是原来的 sp32。
             */
            CHECK(t.ctx.sp == (u32)(uintptr_t)&fr3 + ARM_EXC_FRAME_BYTES);
            CHECK(t.ctx.sp != sp32);
        }
    }

    /* ================================================================ */
    /* 13. ★ M4-8.5:无饥饿判据(策略层,穷尽)★                        */
    /* ================================================================ */

    /*
     * 判据、N 的取法、以及"为什么只能在宿主上穷尽"都写在模拟器那段注释里。
     * 这里只加三件板上做不到的事:
     *
     *   13a. **穷尽扫 K**:2..8 个等权纯占用线程,每一种都必须零违反。
     *        板上只有一组 K,所以"K 变大也不会漏"这件事板上证不了。
     *   13b. **对照组**:`g_pick_sticky` 把 current 粘住 ⇒ 其余线程必然饿死
     *        ⇒ **同一个检出器必须报警**。没有这一条,"零违反"只说明
     *        检查没报错,不说明它有区分能力。
     *   13c. **粘住不适用于 idle**(见 src/sched.c 里那两处偏离说明):
     *        否则对照组一开始就把启动流程自己粘住,一个线程都起不来。
     */
    {
        sim_stat_t st[SIM_K_MAX];
        u32        k;

        /* ---- 13a. 穷尽 K:每个窗口里每个线程都必须推进过 ---- */
        for (k = 2u; k <= SIM_K_MAX; k++) {
            u32 windows = sim_run(k, SCHED_BASE_SLICE_NS, st);
            u32 misses  = 0u;
            u32 picks_min = 0xFFFFFFFFu;
            u32 i;

            /* 非空转:窗口真的结算过,而且不止一个 */
            CHECK(windows >= SIM_WINDOWS / 2u);

            for (i = 0u; i < k; i++) {
                misses += st[i].miss;
                if (st[i].ticks_run < picks_min) {
                    picks_min = st[i].ticks_run;
                }
                /* 每个线程都真的跑到了东西(否则"零违反"是空转出来的)*/
                CHECK(st[i].ticks_run > 0u);
            }

            /* ★ 判据本身:零违反 ★ */
            CHECK(misses == 0u);
            printf("no-starvation: K=%u windows=%u misses=%u min_ticks=%u\n", k, windows, misses,
                   picks_min);
        }

        /* 片长取下限时同样不许漏(片长是策略的输入,不是常量)*/
        for (k = 2u; k <= 4u; k++) {
            u32 windows = sim_run(k, SCHED_MIN_SLICE_NS, st);
            u32 misses  = 0u;
            u32 i;

            CHECK(windows >= SIM_WINDOWS / 2u);
            for (i = 0u; i < k; i++) {
                misses += st[i].miss;
            }
            CHECK(misses == 0u);
        }

        /* ---- 13b. ★ 破坏性 A/B:粘住 current ⇒ 必然饿死 ★ ---- */
        /*
         * `g_pick_sticky` 取一个大数,等于"整段模拟期间都不许换人"。
         * 预期结果:只有一个线程在跑,其余**一个窗口都没轮到**。
         * 这与板上那一相是同一件事,只是一个在策略层、一个在机制层。
         */
        g_pick_sticky = 0xFFFFFFFFu;
        {
            u32 windows = sim_run(4u, SCHED_BASE_SLICE_NS, st);
            u32 starved = 0u;
            u32 runners = 0u;
            u32 i;

            for (i = 0u; i < 4u; i++) {
                if (st[i].ticks_run == 0u) {
                    starved++;
                } else {
                    runners++;
                }
            }

            /* ★ 粘住确实只让一个线程在跑 ★ */
            CHECK(runners == 1u);
            CHECK(starved == 3u);
            /* ★ 而且**同一个检出器**会响:3 个线程每个窗口都算一次违反 ★ */
            CHECK(st[1].miss == windows);
            CHECK(st[2].miss == windows);
            CHECK(st[3].miss == windows);
            CHECK(windows >= SIM_WINDOWS / 2u);
            printf("no-starvation: sticky -> runners=%u starved=%u misses=(%u,%u,%u) windows=%u\n",
                   runners, starved, st[1].miss, st[2].miss, st[3].miss, windows);
        }
        g_pick_sticky = 0u;

        /* ---- 13c. 粘住是**计数**:粘够次数就恢复 ---- */
        {
            sched_queue_init(&q);
            mk(&ta, TASK_KERNEL_LEVEL, 0u, 0u, SCHED_BASE_SLICE_NS);
            mk(&tb, TASK_KERNEL_LEVEL, 0u, 0u, SCHED_BASE_SLICE_NS);
            sched_queue_append(&q, &ta);
            sched_queue_append(&q, &tb);

            /* (1) 真实线程被粘住,而且每粘一次消耗一次 */
            g_pick_sticky = 2u;
            CHECK(sched_select_next(&q, &ta, 0u) == &ta);
            CHECK(g_pick_sticky == 1u);
            CHECK(sched_select_next(&q, &ta, 0u) == &ta);
            CHECK(g_pick_sticky == 0u);
            /*
             * (2) 次数用完之后**必须恢复选取** —— 否则对照组没人能收尾,
             *     那不是"判据不承重",是**测不了**(系统就地挂死)。
             */
            CHECK(sched_select_next(&q, &ta, 0u) == &tb);

            /* ---- 13d. 粘住**不适用于 idle** ---- */
            /*
             * idle 的 status 是 RUNNING,`sched_current_runnable` 对它返回真
             * (那处不对称是源 OS 的原样)。若照字面粘住它,启动流程会把自己
             * 粘住 —— 对照组一个线程都起不来,什么也观察不到。
             * 而源 OS 在 current 是 idle 时**本来就会切走**,所以排除 idle
             * 才是 M4-8 的忠实等价物。
             */
            {
                tcb_t il;

                mk(&tb, TASK_KERNEL_LEVEL, 0u, 0u, SCHED_BASE_SLICE_NS);
                il = mk(&tc, TASK_IDLE_LEVEL, 0u, 0u, SCHED_BASE_SLICE_NS);
                sched_queue_init(&q);
                sched_queue_append(&q, il);
                sched_queue_append(&q, &tb);

                g_pick_sticky = 5u;
                CHECK(sched_select_next(&q, il, 0u) == &tb); /* idle 不被粘住 */
                CHECK(g_pick_sticky == 5u);                  /* 也没被消耗 */
                CHECK(sched_select_next(&q, &tb, 0u) == &tb); /* 真实线程被粘住 */
                CHECK(g_pick_sticky == 4u);
            }
        }
        g_pick_sticky = 0u;
        g_wake_skip   = 0u;
    }

    /*
     * 生产路径的默认值:三个破坏性开关在报告之外必须都是 0。
     * 这一条防的是"某一段用例忘了复位"—— 那会让后面的判据静默失真。
     */
    CHECK(g_wake_skip == 0u);
    CHECK(g_pick_sticky == 0u);

    if (failures != 0) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }

    printf("sched: all checks passed\n");
    return 0;
}
"""


class Arm32SchedTests(unittest.TestCase):
    COMPILER_CANDIDATES = ("gcc", "cc", "clang")

    def test_scheduling_policy(self) -> None:
        capture = {"capture_output": True, "text": True, "encoding": "utf-8", "errors": "replace"}

        with tempfile.TemporaryDirectory() as tmp:
            harness = Path(tmp) / "sched_test.c"
            binary = Path(tmp) / "sched_test"
            harness.write_text(HARNESS, encoding="utf-8")

            attempts: list[str] = []
            for compiler in self.COMPILER_CANDIDATES:
                command = [
                    compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT / "arch/arm32/include"),
                ]
                for fn in KR_PREFIXED:
                    command.append(f"-D{fn}=krtest_{fn}")
                command += [
                    str(harness),
                    str(ROOT / "arch/arm32/src/sched.c"),
                    str(ROOT / "arch/arm32/src/krlibc.c"),
                    "-o", str(binary),
                ]
                try:
                    result = subprocess.run(command, **capture)
                except FileNotFoundError:
                    attempts.append(f"{compiler}: not found")
                    continue
                if result.returncode == 0:
                    break
                detail = (result.stderr or "").strip().replace("\n", " ")[:400]
                attempts.append(f"{compiler}: rc={result.returncode} {detail}")
            else:
                self.fail("no working host C compiler found:\n  " + "\n  ".join(attempts))

            run = subprocess.run([str(binary)], **capture)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("all checks passed", run.stdout)


if __name__ == "__main__":
    unittest.main()
