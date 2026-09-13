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
