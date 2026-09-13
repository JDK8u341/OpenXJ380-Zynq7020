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
/* 可调度性判据 ← `is_task_schedulable` / `is_current_task_runnable`    */
/* ------------------------------------------------------------------ */

/*
 * ← `scheduler.cpp:177` 与 `:189` 里那个三选一的 `status` 条件。
 * 单独抽出来是因为它同时被"挑下一个"和"兜底"两条路径用到,
 * 抄两遍早晚分叉。
 */
bool sched_status_runnable(TaskStatus st);

/*
 * 这个任务能不能当**被选中的下一个**(← `is_task_schedulable`)。
 *
 * ⚠ 源 OS 这里还有两条:`t->parent_group != NULL`,以及进程组状态不能是
 *   DEATH/FUTEX/OUT/WAIT。**内核对线程现在还没有进程组**(那是 M7 的事),
 *   照抄会让**每一个**线程都不可调度 —— 于是调度器永远挑不到人。
 *   所以这两条暂时省掉,并记在退化清单里;等 M7 有了进程组再补回来。
 *   省掉的是"进程组已经死了就别调度它的线程",而那种情况现在不可能发生。
 */
bool sched_task_schedulable(tcb_t t, tcb_t current);

/* ← `is_current_task_runnable`:current 还能不能继续跑。
 * ⚠ 与上面那条**刻意不同**:它不排除 idle,也不排除自己 —— 源 OS 就是这样。 */
bool sched_current_runnable(tcb_t t);

/*
 * 从队首往后找第一个可调度的任务(源 OS 的 `select_next_task` 主干)。
 *
 * 用"遍历"而不是"取队首",是因为队列里可能有**当前正在跑的那个**
 * (它被切回来之前会一直挂在队列里)—— 取队首会挑到自己,
 * 而 `is_task_schedulable(candidate, current)` 的第一条就是把 current 排除掉。
 *
 * ⚠ idle **不在**就绪队列里(见 sched_kern.c 的说明):它的 vruntime 永远是 0,
 *   进了队列就会凭借最小的 deadline 把队首永远占住。
 *   源 OS 靠 `is_task_schedulable` 里那条 `task_level == TASK_IDLE_LEVEL`
 *   把它排除在候选之外,只在最后兜底时才用它 —— 这里等价地保留了那条排除。
 */
tcb_t sched_pick_next(const sched_queue_t *q, tcb_t current);

/* ------------------------------------------------------------------ */
/* ★ M4-9:帧搬迁 ★                                                    */
/* ------------------------------------------------------------------ */

/*
 * ## 为什么 ARM 要"搬帧",而 x86 不用
 *
 * x86 的 `change_proccess(reg, current, target)`(`scheduler.cpp:95`)**就地改写**:
 * 它把 `reg`(栈上的现场帧)的内容收进 `current->context0`,再把
 * `target->context0` 写回**同一个 `reg`**,然后 `save_registers` 收尾的
 * `mov rsp, rax` + `iretq` 从那个帧里恢复 RIP/RSP —— 帧留在原地不动。
 *
 * ARM 不能照做,原因很具体:**ARM 的异常帧里没有 SP**。
 *
 *   | | x86 `registers_t` | ARM `arm_exc_frame_t` |
 *   |---|---|---|
 *   | SP 在帧里吗 | 在(`rsp`/`ss`,硬件压的)| **不在**(SP 按模式 banked,硬件不压)|
 *   | 谁恢复 SP | `iretq` 从帧里取 | `rfeia sp!` 从**帧的地址**算出来 |
 *
 * `EXC_FRAME_LEAVE` 的收尾是 `add sp, sp, #0x38; rfeia sp!` ——
 * 于是返回后的 SP **恒等于"帧基址 + 64"**。也就是说:
 *
 *   ★ 帧在谁的栈上,谁就被恢复 ★
 *
 * 这正是 M4-7 把现场帧建在任务栈上的用处,也是 M4-9 唯一要做的事:
 * 把目标任务的现场**搭在它自己的栈上**,再把这个帧交给 `rfeia`。
 */
void sched_ctx_from_frame(tcb_t t, const arm_exc_frame_t *f);

/*
 * 目标任务的现场该搭在哪里 = `ctx.sp - ARM_EXC_FRAME_BYTES`。
 *
 * 返回 NULL 表示**这个上下文不可切换**(见 sched_ctx_switchable)。
 * 调用方必须处理 NULL —— 而不是拿到一个野指针往里写 64 字节。
 */
arm_exc_frame_t *sched_frame_for(tcb_t t);

/* 把 ctx 的 17 个字段铺进一个已经就位的帧(13 个寄存器 + lr + ret + spsr)。
 * `dst` 通常来自 sched_frame_for();返回 dst,便于写成一句 return。 */
arm_exc_frame_t *sched_frame_from_ctx(tcb_t t, arm_exc_frame_t *dst);

/*
 * ★ 上下文能不能切 ★
 *
 * 这一条是**绊线**,不是装饰。四条,每一条都对应一次真实踩过或差点踩到的坑:
 *
 *   1. `ctx.pc == 0` —— "上下文无效"标记(源 OS 的 `context0.rip == 0`)。
 *      启动上下文在被第一次切走之前就是这样:它的现场在 CPU 里,不在内存里。
 *      切进去的后果是跳到地址 0(那是 OCM,不是代码)。
 *
 *   2. `cpsr` 的模式位必须是 **SVC**。
 *      ⚠ 造新线程时这里原本写的是 `0`。而 `0` 的 CPSR.M[4:0] = **0b00000** ——
 *        AArch32 里**每一个已分配的模式编码 bit4 都是 1**
 *        (USR 0b10000、SVC 0b10011、SYS 0b11111 …),所以 0 不是一个模式,
 *        而是一个**未分配的编码**;按架构,把它装回 CPSR 是 UNPREDICTABLE。
 *        (本项目刻意没去实测 Cortex-A9 遇到它会怎样 —— 那是故意制造 UB。)
 *        它同时也与"被抢占的线程"记录的 CPSR 不一致:后者是 0x53。
 *        两条造上下文的路给出不同的值,本身就是分叉的开始。
 *
 *   3. `sp` 必须 **4** 字节对齐(帧里全是 u32),且非 0。
 *      ⚠ **不是 8** —— 这一条被上板打过一次:被中断的代码在 libgcc 的
 *        Thumb 函数 `__udivmoddi4` 里,它的 `push {r4,r5,lr}` 是 12 字节,
 *        于是函数体内 SP 合法地是 4 mod 8(AAPCS 只要求**调用点**8 字节对齐,
 *        而异常可以落在任意指令边界)。按 8 判会把每一个这样的上下文都拒掉。
 *
 *   4. `sp` 必须落在**这个任务自己的栈区**里(仅当栈来自栈池时)。
 *      收现场时算错一次,就会把 64 字节写到别人的栈上 —— 静默。
 */
bool sched_ctx_switchable(const tcb_t t);

/*
 * 这个 CPSR 描述的是"内核正常运行"的状态吗(A / T / 模式位与
 * `ARM_CPSR_KERNEL` 一致)。不含 I 位 —— 见 `ARM_CPSR_MUST_MATCH`。
 *
 * ★ 它是"手写常量 vs 板上实测"的对账工具 ★
 *
 * 喂给它 `sched_boot_idle()->ctx.cpsr`(被抢占时从现场帧里收进来的、
 * 板上真实读到的 CPSR),就得到一条**不依赖我推理**的判据:
 * 我写的那个常量与内核真正在跑的状态是否一致。
 *
 * 这条判据的由来:造新线程时我把 CPSR 拼成了 0x53,漏了 A 位
 * (内核实际是 0x153),于是切过去的一瞬间解除了异步外部中止的屏蔽,
 * 第一条指令就吃了 FS=0x16 的异步中止。见 arch/taskctx.h 的
 * `ARM_CPSR_KERNEL`。
 */
bool sched_cpsr_matches_kernel(u32 cpsr);

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
 * 调度开关 ← `disable_scheduler()` / `enable_scheduler()`
 * (`scheduler.cpp:31-34`)。关掉时 `sched_tick` 在第一行就返回 ——
 * 连计费都不做,与源 OS 逐字一致。
 *
 * 用途:罩住"造线程/改调度状态"这种多步操作。见 src/sched_kern.c 的说明。
 */
void sched_disable(void);
void sched_enable(void);
bool sched_is_enabled(void);

/*
 * 造一个内核线程并把本核就绪队列按 deadline 有序插入。
 *
 * TCB 从内核堆取(不是静态数组 —— 线程数不能是编译期常量),
 * 内核栈从 M4-5 的栈池取(带 guard page)。两者任一失败就整体回滚。
 */
tcb_t sched_kthread_create(void (*entry)(void *), void *arg, const char *name);

/*
 * 让出 CPU。**走的是陷阱,不是自己实现一套切换**。
 *
 * ← `scheduler_yield()` `scheduler.cpp:460-465`:把本核的时间片记账设成
 * 已用尽,然后软中断进调度器入口 —— 于是"让出"与"被抢占"是同一条路径。
 * ARM 侧对应 `svc #ARM_SVC_YIELD`,理由(含为什么不用 SGI)见 taskctx_asm.h。
 *
 * ⚠ 这个函数**可能不会立刻返回**:它把控制权交给调度器,回来的时候
 *   世界已经变了(当前线程可能已经不是调用它的那个)。
 *   从调用者的角度看,它与"被打断又恢复"没有区别 —— 这正是要点。
 */
void sched_yield(void);

/*
 * 把自己挂起(status = WAIT,wakeup_time = 0 = 不按时间唤醒),然后切走。
 *
 * 内核线程没有"退出"这个概念(源 OS 里任务是被 `process_exit` 回收的,
 * 而那是用户进程的事)。自检里的探针线程干完活就调它,
 * 于是它**不再可调度**,idle 才有机会被挑中、启动流程才回得来。
 *
 * ⚠ 本函数**不会返回**(除非调用者是 idle)。
 */
void sched_park_self(void);

/*
 * ★ 破坏性 A/B 的对照组开关 ★
 *
 * 置 1 时 `sched_tick` **照做完一切**(计费、挑下一个、改状态、挪队列、
 * 收现场、搭新帧),只在最后一步**不把新帧交出去** —— 仍旧从原来那个帧返回。
 * 也就是说:决策说"切走了 N 次",而执行流一步都没动。
 *
 * 这就是 M4-9 之前的行为,也正是"搬帧"要证明承重的那一件事:
 *   同一段代码、同一个负载,只差这一步,两个不让出的线程
 *   从"一次都跑不起来"变成"真的交错执行"。
 *
 * 生产路径上恒为 0。
 */
extern u32 g_reloc_skip;

/*
 * 把**启动上下文**注册成 idle(照源 OS)。
 *
 * 关键:`ctx.pc = 0` 是"上下文无效"的标记 —— 源 OS 用 `context0.rip == 0`,
 * 而且**只在最初**成立:idle 第一次被切走时,`change_proccess` 会把
 * 真实的现场收进它的 `context0`,于是从此它就是一个**可恢复的普通上下文**。
 *
 * ⚠ 本文件早先在这里写过"没有'切进 idle'这回事",那是**读错了一半**:
 *   那个 0 标记的作用只是"在被第一次切走**之前**不许切进来"
 *   (那时它的现场在 CPU 里,不在内存里,切进去就是跳到地址 0)。
 *   切走一次之后 idle 照样会被挑中、被恢复 —— 而且**必须**能,
 *   否则启动流程被切走之后就再也回不来了。
 *
 * 也就是说 **idle 不是另一个线程,而是启动流程自己**;它不是"进去"的,
 * 是被切走之后又被切回来的。
 */
void  sched_register_boot_idle(void);
tcb_t sched_boot_idle(void);

/*
 * tick 里给 current 计费 ← `timer_handle()` `scheduler.cpp:437`。
 * 独立出来是为了能单独测"计费"这件事(它不涉及切换)。
 */
void sched_tick_account(void);

/*
 * ★ M4-9:tick 里的完整调度决策 —— 而且**真的搬帧** ★
 * ← `timer_handle()` `scheduler.cpp:430-470`
 *
 * 返回"应当从哪个帧离开":
 *   - current 为空 / 时间片没到 / 挑不到别人 / 目标上下文不可切换:
 *     原样返回 `frame`(执行流不变)
 *   - 否则:收 current 的现场进它的 ctx,在**目标任务的栈上**搭一个新帧
 *     并返回它 —— `rfeia` 于是落到目标的 PC/CPSR 上,SP 也变成它的
 *
 * ⚠ current 为 NULL 时**必须**早退(照源 OS `scheduler.cpp:390`):
 *   那时没有"从哪来"可收,放过去等于把启动上下文永久丢掉 ——
 *   它的 ctx 从头到尾没人填过,一旦切走就再也回不来。
 */
arm_exc_frame_t *sched_tick(arm_exc_frame_t *frame);

/* 诊断:本核在 tick 里**真的完成**的切换次数(= 搬帧次数)*/
u32 sched_tick_switched(void);
/* 诊断:其中"被换下的是真实线程"(即真正意义上的抢占)的次数 */
u32 sched_tick_preempted(void);
/* 诊断:挑到了别人、但目标上下文不可切换而放弃的次数 */
u32 sched_tick_invalid_ctx(void);

/* 诊断:本核累计切换次数 */
u32 sched_switch_count(void);
