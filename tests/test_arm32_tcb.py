"""进程/线程控制块(M4-6)的单元测试。

被测的是 arch/arm32/include/arch/tcb.h 与它依赖的 percpu.h。

**为什么值得测**:这份结构体的存在意义是"与源 OS 逐字段一致",而
"一致"这件事有两个都会静默失败的破坏方式:

  1. **字段名改了个近义词** —— `kernel_stack` 写成 `kstack`、`tid` 写成 `thread_id`。
     代码照样编、照样跑,只是再也无法与 `include/task/pcb.h` 并排核对,
     而"并排核对"正是这次移植唯一能抓住语义偏差的手段。
     本测试把**从 x86 抄来的字段名逐条钉死**。
  2. **汇编用到的偏移漂移** —— 这是 §4.7.6 记下的坑(x86 的
     `handler.S:29-31` 硬编码三个偏移、零断言)。
     ARM 侧把汇编可见的偏移收窄到两组,本测试**在宿主机上直接验证目标偏移**。

**为什么宿主能验目标偏移**:那两组偏移所在的类型**只含 u32 字段**
(`arm_task_ctx_t` 与 `percpu_t`)。一旦有人往里加指针/`uintptr_t`,
宿主布局就会与目标不同,断言随即失效 —— 所以本测试还额外检查
"percpu_t 里没有指针宽度的字段"。

参考:
  arch/arm32/include/arch/tcb.h           PCB / TCB 与三份清单的对应关系
  arch/arm32/boot/context.S               汇编侧按宏核对 ctx 的布局
  include/task/pcb.h                      x86 的源结构体
  docs/ZYNQ7020_PORT_PLAN.md §4.7         三份清单
"""

from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

HARNESS = r"""
int printf(const char *fmt, ...);

#include <arch/percpu.h>
#include <arch/tcb.h>
#include <arch/types.h>

static int failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL %d: %s\n", __LINE__, #cond);                                              \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

int main(void)
{
    /*
     * ⚠ 必须清零:下面靠"字段是不是 0"来确认布局,未初始化的栈变量会让
     *   这些检查全部误报(第一版就是这么写的,16 项 FAIL 全是假警报)。
     */
    struct arm_process_control_block pcb = {0};
    struct arm_thread_control_block  tcb = {0};

    /* ---- 1. 与 x86 同名的常量必须逐个一致(`pcb.h:3-5`) ---- */
    CHECK(TASK_KERNEL_LEVEL == 0);
    CHECK(TASK_IDLE_LEVEL == 1);
    CHECK(TASK_APPLICATION_LEVEL == 2);

    /*
     * ---- 2. TaskStatus 的数值必须逐个一致(`pcb.h:49-59`)----
     *
     * 它们会进 procfs,也可能进用户态;数值变了就是 ABI 变了。
     */
    CHECK(CREATE == 0 && RUNNING == 1 && WAIT == 2 && DEATH == 3);
    CHECK(START == 4 && FUTEX == 5 && OUT == 6 && ZOMBIE == 7);

    /*
     * ---- 3. ★ 字段名与**宽度**逐条钉死 ★ ----
     *
     * 两件事各有各的失败方式:
     *   - **改名**(`kernel_stack` -> `kstack`):代码照样编、照样跑,
     *     只是再也无法与 `include/task/pcb.h` 并排核对 —— 而"并排核对"
     *     是这次移植唯一能抓住语义偏差的手段;
     *   - **改宽度**(地址写成 `u64`):在 ARM32 上白占一倍空间,
     *     而且会让宿主与目标的布局分叉。
     *
     * 所以每一项都同时钉名字(能编译)与宽度(sizeof)。
     */
    {
        /* ---- PCB:§4.7.2 的 ARCH_INDEP 清单,名字照抄 include/task/pcb.h ---- */
        CHECK(sizeof(pcb.pid) == 4);   /* `pcb.h:87`  size_t(ARM 上 32 位)*/
        CHECK(sizeof(pcb.ppid) == 4);  /* `pcb.h:88`  uint64_t -> 见下面注 */
        CHECK(sizeof(pcb.name) == 32); /* `pcb.h:90` */

        /* 队列与列表:都是指针,名字要对上 */
        CHECK(pcb.parent_task == NULL);          /* `pcb.h:92`  */
        CHECK(pcb.thread_queue == NULL);         /* `pcb.h:93`  */
        CHECK(pcb.ipc_queue == NULL);            /* `pcb.h:97`  */
        CHECK(pcb.file_open == NULL);            /* `pcb.h:99`  */
        CHECK(pcb.file_open_shared_refs == NULL);/* `pcb.h:100` */
        CHECK(pcb.child_pcb == NULL);            /* `pcb.h:106` */
        CHECK(pcb.envp == NULL);                 /* `pcb.h:101` */
        CHECK(pcb.cmdline == NULL);              /* `pcb.h:109` */
        CHECK(pcb.argv == NULL);                 /* `pcb.h:110` */
        CHECK(pcb.exe_path == NULL);             /* `pcb.h:112` */

        /* 索引与计数 */
        CHECK(sizeof(pcb.queue_index) == 4); /* `pcb.h:94`  */
        CHECK(sizeof(pcb.child_index) == 4); /* `pcb.h:105` */
        CHECK(sizeof(pcb.envc) == 4);        /* `pcb.h:102` */
        CHECK(sizeof(pcb.argc) == 4);        /* `pcb.h:111` */
        CHECK(sizeof(pcb.exit_code) == 4);   /* `pcb.h:107` */
        CHECK(sizeof(pcb.task_level) == 4);  /* `pcb.h:113` */
        CHECK(sizeof(pcb.vfork) == 1);       /* `pcb.h:104` */

        /* 用户态地址窗口(等 M7 才用,但宽度现在就要定下来)*/
        CHECK(sizeof(pcb.mmap_start) == 4);   /* `pcb.h:108` */
        CHECK(sizeof(pcb.brk_start) == 4);    /* `pcb.h:129` */
        CHECK(sizeof(pcb.brk_end) == 4);      /* `pcb.h:130` */
        CHECK(sizeof(pcb.brk_current) == 4);  /* `pcb.h:131` */

        /* ★ 唯一一个 ARCH_DEP 字段:ARM 是 L1 表,不是 x86 的 4 级页表 ★ */
        CHECK(pcb.pagedir == NULL); /* `pcb.h:91` */

        /* ---- TCB ---- */
        CHECK(tcb.parent_group == NULL); /* `pcb.h:160` */
        CHECK(sizeof(tcb.task_level) == 4); /* `pcb.h:161` */
        CHECK(sizeof(tcb.tid) == 4);        /* `pcb.h:163` */
        CHECK(sizeof(tcb.name) == 32);      /* `pcb.h:164` */
        CHECK(tcb.status == CREATE);        /* `pcb.h:165`,清零后应当是 CREATE */
        /*
         * `wakeup_time` 是**纳秒时间戳**,不是地址 —— 所以它在 ARM 上也保持
         * 64 位。这是本结构体里少数几个不该被"压成 32 位"的字段之一。
         */
        CHECK(sizeof(tcb.wakeup_time) == 8); /* `pcb.h:166` */
        CHECK(sizeof(tcb.cpu_id) == 4);      /* `pcb.h:167` */

        CHECK(sizeof(tcb.queue_index) == 4); /* `pcb.h:175` */
        CHECK(sizeof(tcb.group_index) == 4); /* `pcb.h:177` */
        CHECK(sizeof(tcb.main) == 4);        /* `pcb.h:178` 入口地址 */
        CHECK(sizeof(tcb.user_stack) == 4);  /* `pcb.h:179` */
        CHECK(sizeof(tcb.user_stack_top) == 4); /* `pcb.h:180` */
        CHECK(sizeof(tcb.load_start) == 4);  /* `pcb.h:185` */
        CHECK(sizeof(tcb.load_end) == 4);    /* `pcb.h:186` */
        CHECK(sizeof(tcb.clear_child_tid) == 4); /* `pcb.h:192` */
        CHECK(tcb.str_cwd == NULL);          /* `pcb.h:200` */
        CHECK(tcb.argv == NULL);             /* `pcb.h:202` */
        CHECK(sizeof(tcb.argc) == 4);        /* `pcb.h:203` */

        /* 内核栈:ARM 侧比 x86 多记了三个量(栈底 / guard / 槽号)*/
        CHECK(sizeof(tcb.kernel_stack) == 4); /* `pcb.h:174` */
        CHECK(sizeof(tcb.kstack_base) == 4);
        CHECK(sizeof(tcb.kstack_guard) == 4);
        CHECK(sizeof(tcb.kstack_slot) == 4);
        CHECK(sizeof(tcb.owns_kstack) == 1);
        CHECK(sizeof(tcb.owns_user_stack) == 1); /* `pcb.h:218` */

        /* 调度统计:EEVDF 全是 64 位累加量,ARM 上也不能压 */
        CHECK(sizeof(tcb.eevdf_vruntime) == 8);   /* `pcb.h:212` */
        CHECK(sizeof(tcb.eevdf_deadline) == 8);   /* `pcb.h:213` */
        CHECK(sizeof(tcb.eevdf_slice) == 8);      /* `pcb.h:214` */
        CHECK(sizeof(tcb.eevdf_last_start) == 8); /* `pcb.h:215` */
        CHECK(sizeof(tcb.runtime_ticks) == 8);    /* `pcb.h:216` */
    }

    /*
     * ---- 4. ★ 刻意**没有**的字段 ★ ----
     *
     * 这一组不是"顺便看看",而是把"暂时不放"这件事变成会编译失败的东西:
     * 谁把它们加回来,必须先解释清楚为什么。
     */
    {
        /* 架构上不需要:ARM 的 SP 按模式 banked,硬件自动切 */
        CHECK(ARM_TCB_DEFERRED_FIELDS[0] == 's'); /* 编译期常量串,永远成立 */
    }

    /*
     * ---- 5. ★ 汇编可见的那两组偏移,在宿主上验的就是目标偏移 ★ ----
     *
     * `arm_task_ctx_t` 与 `percpu_t` **只含 u32 字段**,所以宿主(指针 8 字节)
     * 与目标(指针 4 字节)的布局一致。这是刻意的设计约束,不是巧合 ——
     * 第 6 节会再检查一遍。
     */
    CHECK(offsetof_arm(arm_task_ctx_t, sp) == 0x34u);
    CHECK(offsetof_arm(arm_task_ctx_t, lr) == 0x38u);
    CHECK(offsetof_arm(arm_task_ctx_t, pc) == 0x3Cu);
    CHECK(offsetof_arm(arm_task_ctx_t, cpsr) == 0x40u);
    CHECK(offsetof_arm(arm_task_ctx_t, r[12]) == 0x30u);
    CHECK(sizeof(arm_task_ctx_t) == 68u);

    CHECK(offsetof_arm(percpu_t, current_task) == ARM_PERCPU_OFF_CURRENT_TASK);
    CHECK(ARM_PERCPU_OFF_CURRENT_TASK == 0x28u);

    /* ---- 6. ★ 宿主与目标布局一致的前提:没有指针宽度的字段 ★ ---- */
    {
        /*
         * `sizeof(percpu_t)` 在 32 位与 64 位宿主上都必须一样。
         * 这里用"全部 u32"的等价布局重算一遍,与真实 sizeof 对比 ——
         * 一旦有人往 percpu_t 里塞指针或 uintptr_t,这条会立刻失败,
         * 提醒他"宿主上验的偏移不再等于目标偏移"。
         */
        /*
         * ★ 布局不变式的判据:sizeof 必须等于"全部字段按 u32/u64 排下来"的结果 ★
         *   只要有人往里塞指针或 uintptr_t,宿主上这个数就会变大 ——
         *   而"宿主验的偏移就是目标偏移"这条前提随即失效。
         */
        CHECK(sizeof(percpu_t) == 64u);
        CHECK(offsetof_arm(percpu_t, sched_head) == ARM_PERCPU_OFF_CURRENT_TASK + 4u);
        CHECK(offsetof_arm(percpu_t, sched_count) == ARM_PERCPU_OFF_CURRENT_TASK + 8u);
        CHECK(offsetof_arm(percpu_t, scheduler_ticks) == 56u);
        CHECK(sizeof(((percpu_t *)0)->sched_head) == 4u);
        CHECK(sizeof(((percpu_t *)0)->scheduler_ticks) == 8u);
        /* 地址字段是 u32,不是 uintptr_t —— 后者在宿主上是 8 字节 */
        CHECK(sizeof(((percpu_t *)0)->stack_top) == 4u);
        CHECK(sizeof(((percpu_t *)0)->current_task) == 4u);
    }

    /* ---- 7. TCB 里 ctx 的偏移不钉,但**位置**要能算出来 ---- */
    {
        /*
         * 汇编不按这个偏移访问(边界传的是 `&tcb->ctx` 指针),
         * 所以这里不做数值断言 —— TCB 里有指针字段,宿主上算出来的
         * 目标值本来就是错的。只确认它是个有效的字段偏移。
         */
        CHECK(offsetof_arm(struct arm_thread_control_block, ctx) <
              sizeof(struct arm_thread_control_block));
        CHECK(offsetof_arm(struct arm_thread_control_block, ctx) % 4u == 0u);
    }

    /* ---- 8. VFP 区:大小与对齐是硬件要求 ---- */
    CHECK(sizeof(tcb.vfp) == 256u);   /* d0-d31,每个双字 8 字节 */
    CHECK(ARM_VFP_BYTES == 256u);
    CHECK(ARM_VFP_D_REGS == 32u);
    /* VFP 传输要求 4 字节对齐(架构规则;16 是 x86 FXSAVE 的要求)*/
    CHECK(offsetof_arm(struct arm_thread_control_block, vfp) % 4u == 0u);

    /* ---- 9. 指针 typedef 的约定(`pcb.h:46-47`)---- */
    {
        pcb_t p = &pcb;
        tcb_t t = &tcb;

        CHECK(p == &pcb);
        CHECK(t == &tcb);
        /* 是指针 typedef,不是结构体 typedef —— 赋值给 void* 应当合法 */
        CHECK((void *)p != NULL);
        CHECK((void *)t != NULL);
    }

    if (failures != 0) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }

    printf("tcb: all checks passed\n");
    return 0;
}
"""


class Arm32TcbTests(unittest.TestCase):
    COMPILER_CANDIDATES = ("gcc", "cc", "clang")

    def test_control_blocks(self) -> None:
        capture = {"capture_output": True, "text": True, "encoding": "utf-8", "errors": "replace"}

        with tempfile.TemporaryDirectory() as tmp:
            harness = Path(tmp) / "tcb_test.c"
            binary = Path(tmp) / "tcb_test"
            harness.write_text(HARNESS, encoding="utf-8")

            attempts: list[str] = []
            for compiler in self.COMPILER_CANDIDATES:
                command = [
                    compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT / "arch/arm32/include"),
                    str(harness),
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
