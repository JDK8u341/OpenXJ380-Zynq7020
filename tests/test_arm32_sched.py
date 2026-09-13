"""调度策略(M4-8.2)的单元测试。

被测代码是 arch/arm32/src/sched.c —— 不含 MMIO/CP15,时间以参数传入,
所以宿主机能直接编译并穷尽测。

**为什么策略的判据必须在这里,而不是在板子上**:

板级证据只能证明**机制**对(切换、抢占、无饥饿、idle),证明不了策略 ——
因为在"等权 + 纯占用"的负载下,**任何策略都会通过**。
拾取顺序、睡醒补偿、队列不变量这些只有在这里才判得动。
(这条分工是 M4-8 开工前拍板时定下来的,见计划 §4.5。)

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

    /* ---- 8. 就绪队列:有序插入、查重、摘除、计数 ---- */
    sched_queue_init(&q);
    CHECK(q.head == NULL && q.count == 0u);
    CHECK(sched_pick(&q) == NULL);

    mk(&ta, TASK_KERNEL_LEVEL, 100u, 3000u, SCHED_BASE_SLICE_NS);
    mk(&tb, TASK_KERNEL_LEVEL, 200u, 1000u, SCHED_BASE_SLICE_NS);
    mk(&tc, TASK_KERNEL_LEVEL, 300u, 2000u, SCHED_BASE_SLICE_NS);

    CHECK(sched_queue_insert(&q, &ta));
    CHECK(sched_queue_insert(&q, &tb));
    CHECK(sched_queue_insert(&q, &tc));
    CHECK(q.count == 3u);
    /* 队首 = deadline 最小者 */
    CHECK(sched_pick(&q) == &tb);
    CHECK(q.head == &tb && tb.sched_next == &tc && tc.sched_next == &ta && ta.sched_next == NULL);

    /* ★ 重复插入必须被拒 —— 挂两次会把链表做成环,遍历就死循环了 ★ */
    CHECK(!sched_queue_insert(&q, &tb));
    CHECK(!sched_queue_insert(&q, &ta));
    CHECK(q.count == 3u);
    CHECK(q.head == &tb);

    /* 摘中间一个 */
    sched_queue_remove(&q, &tc);
    CHECK(q.count == 2u);
    CHECK(tb.sched_next == &ta);
    CHECK(tc.sched_next == NULL);
    /* 摘了再插:应当回到正确位置 */
    tc.eevdf_deadline = 500u;
    CHECK(sched_queue_insert(&q, &tc));
    CHECK(sched_pick(&q) == &tc);
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
    CHECK(!sched_queue_insert(NULL, &ta));
    CHECK(!sched_queue_insert(&q, NULL));

    /* 空队列与 NULL 的边界 */
    sched_queue_init(NULL);
    sched_queue_remove(NULL, &ta);
    sched_queue_remove(&q, NULL);
    CHECK(sched_pick(NULL) == NULL);

    /* ---- 9. 平均 vruntime(源 OS 用它给新线程定起点)---- */
    sched_queue_init(&q);
    mk(&ta, TASK_KERNEL_LEVEL, 1000u, 10u, SCHED_BASE_SLICE_NS);
    mk(&tb, TASK_KERNEL_LEVEL, 3000u, 20u, SCHED_BASE_SLICE_NS);
    sched_queue_insert(&q, &ta);
    sched_queue_insert(&q, &tb);
    CHECK(sched_queue_avg_vruntime(&q, NULL, 0u) == 2000u);
    CHECK(sched_queue_avg_vruntime(&q, &ta, 0u) == 3000u);   /* 排除自己 */
    CHECK(sched_queue_avg_vruntime(&q, &ta, 777u) == 3000u);
    sched_queue_remove(&q, &tb);
    CHECK(sched_queue_avg_vruntime(&q, &ta, 777u) == 777u);  /* 空 -> fallback */
    CHECK(sched_queue_avg_vruntime(NULL, NULL, 42u) == 42u);

    /* ---- 10. sched_entity_init:新线程的起点不是 0 ---- */
    mk(&ta, TASK_KERNEL_LEVEL, 0u, 0u, 0u);
    ta.eevdf_deadline = 12345u;
    sched_entity_init(&ta, 7000000ull);
    CHECK(ta.eevdf_vruntime == 7000000ull);
    CHECK(ta.eevdf_slice == SCHED_BASE_SLICE_NS);
    CHECK(ta.eevdf_deadline == 7000000ull + SCHED_BASE_SLICE_NS);
    CHECK(ta.eevdf_last_start == 7000000ull);
    CHECK(ta.runtime_ticks == 0u);
    CHECK(ta.sched_next == NULL);
    sched_entity_init(NULL, 1u); /* 不崩即可 */

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

    /* idle 的 deadline 最小(0),所以它在队首 —— 正是"不能让它占住队首"的场景 */
    sched_queue_insert(&q, &tb);
    sched_queue_insert(&q, &ta);
    sched_queue_insert(&q, &tc);
    CHECK(sched_pick(&q) == &tb); /* 队首确实是 idle —— 取队首的做法会错在这里 */

    /* sched_pick_next 必须跳过 idle 与挂起的,挑到 ta */
    CHECK(sched_pick_next(&q, NULL) == &ta);
    /* current 自己也要跳过 */
    CHECK(sched_pick_next(&q, &ta) == NULL);
    CHECK(sched_pick_next(NULL, NULL) == NULL);

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
