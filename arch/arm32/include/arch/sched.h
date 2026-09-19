#pragma once

/*
 * ★ 整份声明放进 `extern "C"`(2026-09-18)。
 *
 * 理由与 `arch/device.h` 那次完全相同,而且这次是**实测撞出来的**:
 * 落地层 `arch/arm32/src/upstream_api.cpp` 是 C++(它必须跨两个世界,
 * 理由见那个文件头),它调用移植侧的 `sched_yield()` /
 * `sched_sleep_ns()` / `sched_wake_task()` —— 而这三个的**实现是 C**
 * (`src/sched_kern.c`)。声明若按 C++ 链接,链接期就报
 *     undefined reference to `sched_yield()'
 * (实测正是如此。同一批里 `arch_irq_disable()` 没出问题 —— 它是
 *  `static inline`,根本不产生符号。)
 *
 * ⇒ 由此得到一条**通用规则**:
 *   **移植侧的头文件只要会被 C++ 翻译单元看到,就必须声明 C 链接。**
 *   已照此办过的:`arch/device.h`、`krlibc.h`、本文件。
 */
#ifdef __cplusplus
extern "C" {
#endif

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
 * 就绪队列。**照源 OS：插入序 FIFO,不排序**。
 *
 * ⚠ 这条在 M4-8 被我写错过一次:M4-S 第一版是"按 deadline 有序插入、
 *   取队首就是应选者"。而源 OS 的队列(`lock_queue`)是 **FIFO 追加**
 *   (`queue_enqueue` → `queue_append_node`),**选取靠全表扫描**
 *   (`select_next_task_safe`):先算 `avg_vruntime`,再要求
 *   `candidate->eevdf_vruntime <= avg`,在合格者里取 deadline 最小。
 *
 *   差别不只是"像不像":**唤醒睡眠任务的那次扫描在 `select_next_task_safe`
 *   里**,取队首的写法里根本没有它 ⇒ 睡下去的任务永远醒不过来。
 *   M4-8.4 要做的第一件事就是把这个账补上。
 *
 * 仍然沿用 M4-8 的两条 ARM 侧选择(与源 OS 的差别,各有理由):
 *   - **内嵌 `sched_next` 指针**,不用源 OS 那种独立分配的 `lock_node`
 *     (队列不需要额外分配,宿主测试也就不需要构造分配器);
 *   - **无锁**:单核,只有 CPU0 碰它(M4-10 再定每核一把还是无锁)。
 *
 * ⚠ `current`(正在跑的那个)与睡眠中的任务**都留在队列里** —— 源 OS 就是这样,
 *   靠 `sched_task_schedulable()` 把它们排除在候选之外。
 *   队列是"全部线程的名册",不是"就绪链表"。
 */
typedef struct
{
    tcb_t head;
    u32   count;
} sched_queue_t;

void  sched_queue_init(sched_queue_t *q);
void  sched_queue_remove(sched_queue_t *q, tcb_t t);
/* FIFO 追加(← `queue_enqueue` 的 `queue_append_node`)。重复插入被拒绝 */
bool  sched_queue_append(sched_queue_t *q, tcb_t t);

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

/*
 * 新线程的初始调度状态 ← `init_task_eevdf_entity()` `scheduler.cpp:289-297`
 *
 * `base_vruntime` 是**目标队列的平均 vruntime**(源 OS 在 `add_task` 里传
 * `queue_average_vruntime(...)`),`now` 只用来记 `eevdf_last_start`。
 *
 * ★ 源 OS 会把 `base_vruntime` **减去一个 `EEVDF_WAKEUP_CREDIT`** 再作为起点:
 *     vruntime = base > CREDIT ? base - CREDIT : 0
 *   ARM 侧 M4-8 漏了这个减法(把参数当成了"当前时刻"),M4-10 开工调研时
 *   查出来并改正 —— 记在退化清单 **D15**。宿主单测逐值钉住它。
 */
void sched_entity_init(tcb_t t, u64 base_vruntime, u64 now);

/* ← `queue_average_vruntime()` `scheduler.cpp:256`
 *
 * **它不只是算平均值** —— 它在遍历队列的同时:
 *   1. **唤醒到点的睡眠任务**(`wake_sleeping_task`,基准是 `current` 的 vruntime);
 *   2. 记下"可调度的候选里 deadline 最小的那个"(`*out_fallback`);
 *   3. 记下队列里的 idle 任务(`*out_idle`,源 OS 用它做最后的兜底);
 *   4. 把 `current`(若可运行)与各候选的 vruntime 一起计入平均。
 *
 * ★ 第 1 条是 M4-8.4 的关键:源 OS 里**唤醒就发生在这次扫描里**。
 *   所以这个函数不能简化成"求个平均" —— 那正是 M4-8 的 `sched_pick`
 *   (取队首)漏掉的那一步,也是睡眠任务醒不过来的原因。
 *
 * 队列为空(或没有可计入者)时返回 `fallback`。
 */
u64 sched_queue_scan(const sched_queue_t *q, tcb_t current, u64 now, tcb_t *out_fallback, tcb_t *out_idle);

/* 保留一个"只算平均"的只读版本,给不需要唤醒的场景用(宿主测与新建线程定起点)。
 * `ignore` 为 NULL 时统计全部。 */
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
 * ★ M4-8.4:选取下一个任务 ← `select_next_task_safe()` + `select_next_task()`
 *   (`scheduler.cpp:316-378`)
 *
 * 一次调用做完四件事(顺序与源 OS 一致):
 *   1. `wake_sleeping_task(current, now, current->vruntime)` —— current 自己到点也唤醒;
 *   2. `sched_queue_scan(...)` —— 扫描中唤醒所有到点的睡眠任务,并取回
 *      `avg_vruntime` / `fallback` / `idle`;
 *   3. 闸门:`fallback->vruntime <= avg` 就用它;否则在合格者里取 deadline 最小;
 *      都不合格就退回 `fallback`;
 *   4. 结果 = `best ? best : (current 可运行 ? current : idle)`,
 *      并对结果做 `mark_task_dispatched`;
 *   5. 最后 `select_next_task()` 的收尾:结果为 NULL 而 current 是
 *      RUNNING/START 时返回 current。
 *
 * ⚠ 第 1、2 条里的**唤醒**是副作用,不是查询 —— 这个函数会改任务状态
 *   (status / wakeup_time / vruntime / deadline / last_start)。
 *   名字叫 "select" 有点轻描淡写,但源 OS 就是这么放的。
 *
 * ⚠ 需要 `now`,而不是自己去读时钟:本层是纯逻辑,宿主上要能构造
 *   "过了 3ms" 这种场景。
 */
tcb_t sched_select_next(const sched_queue_t *q, tcb_t current, u64 now);

/*
 * ★ 破坏性 A/B:跳过扫描里的唤醒 ★
 *
 * 置 1 时 `sched_queue_scan()` 仍遍历队列但**不唤醒**到点的睡眠任务 ——
 * 也就是"睡下去就永远醒不过来"。这正是 M4-8 "取队首"那一版的等效行为,
 * 所以它是"唤醒发生在扫描里"这条判据承重的证明方式。
 *
 * ⚠ 定义在 `src/sched.c`(纯逻辑层自己持有),因为它必须能在宿主编译
 *   单元里解析。生产路径上恒为 0。
 */
extern u32 g_wake_skip;

/*
 * ★ 第二个纯逻辑层的破坏性 A/B(M4-8.5):**严格非抢占** ★
 *
 * **计数**,不是布尔:`g_pick_sticky = n` 表示"接下来 n 次选取都把 current
 * 粘住"(时间片到点也不换人),每粘一次减一,归零后恢复正常。生产路径恒为 0。
 *
 * 它是"无饥饿"这条判据的对照组:粘住一个真实线程之后,其余可运行线程
 * **必然饿死** —— 监视器必须报警。没有这一相,"无饥饿检查通过"只说明
 * 检查没报错,不说明它有区分能力。
 *
 * 三处语义细节(与计划原话的差别,理由见 src/sched.c):
 *   - 只对**真实线程**生效,current 是 idle 时照常选取 ——
 *     否则启动流程会把自己粘住,对照组一个线程都起不来;
 *   - 粘住发生在**扫描之后**,唤醒照做,于是对照组只改了"选谁";
 *   - 是计数而不是置 1,于是"粘住一段时间后会恢复",对照组能够收尾。
 */
extern u32 g_pick_sticky;

/* ← `mark_task_dispatched()` `scheduler.cpp:304` */
void sched_mark_dispatched(tcb_t t, u64 now);

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
/* ★ M4-10:选核(纯逻辑)★                                            */
/* ------------------------------------------------------------------ */

/*
 * ← `add_task()` `scheduler.cpp:537-549`:新线程该放进哪个核的队列。
 *
 * 参数是三个数组而不是"去读全局" —— 本层保持纯逻辑(宿主可穷尽测):
 *   `queue_len[cpu]` 该核就绪队列**当前长度**(源 OS 就是按 `queue->size` 挑);
 *   `ready[cpu]`     该核的调度器**就绪**吗(队列与 idle 都建好了);
 *   `cpu_num`        核数。
 *
 * 语义逐字照抄,三条:
 *   1. 起点 **CPU0**,比较用**严格小于** ⇒ **平局留给核号小的**;
 *   2. `TASK_APPLICATION_LEVEL` **跳过整个扫描** ⇒ 永远 CPU0;
 *   3. 只按队列长度挑 —— 源 OS 没有负载均值,也没有周期性迁移。
 *
 * ⚠ 没就绪的核不参与(`ready[i] == 0` 跳过);`ready[0] == 0` 时返回 0,
 *   由调用方决定怎么办 —— 往一个没人扫的队列里放线程**不会报任何错**,
 *   只是那个线程永远不跑,而那种静默正是本项目最贵的故障类型。
 */
u32 sched_pick_cpu(i32 task_level, const u32 *queue_len, const u32 *ready, u32 cpu_num);

/* ------------------------------------------------------------------ */
/* 内核侧(M4-8.3,实现在 src/sched_kern.c)                            */
/* ------------------------------------------------------------------ */

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
 * ★ M4-8.5:调度器**被人为关掉**的累计时长(纳秒)★
 *
 * "关调度"是 `console_excl_begin/end` 实现排他输出的手段(见 arch/console.h),
 * 于是自检报告那几秒钟里**整个系统是停摆的**:所有线程都醒不过来,
 * 那不是饥饿,是刻意的。没有这个量,饥饿监视器会把那段停顿报成饿死。
 *
 * 用法是**差值**:取一次、睡一个观测窗口、再取一次。
 *
 * ⚠ 拿到差值之后**不是**简单地把整个窗口一扔了事(第一版就是这么写的,
 *   上板立刻打回来,理由是它**与饥饿叠加在同一个窗口里**):
 *   饥饿本身也会让窗口变得很长,而在它前面又恰好有一段排他输出时,
 *   两者落在同一个窗口 —— 扔掉就等于漏报。
 *   所以监视器用的是 **`窗口长度 - 停摆时长 = 可执行时长`**,
 *   再拿"可执行时长"去判:自检报告那种(窗口 5s / 停摆 5s)自动落空,
 *   而"被粘住 1.2s / 停摆 0.1s"照样报警。完整推演在 src/kmain.c 的
 *   `starve_monitor` 里。
 */
u64 sched_off_total_ns(void);

/*
 * 造一个内核线程,并把它放进**挑出来的那个核**的队列(见 sched_pick_cpu)。
 *
 * TCB 从内核堆取(不是静态数组 —— 线程数不能是编译期常量),
 * 内核栈从 M4-5 的栈池取(带 guard page)。两者任一失败就整体回滚。
 */
tcb_t sched_kthread_create(void (*entry)(void *), void *arg, const char *name);

/*
 * 同上的**带任务等级**版本 —— 它是 M4-10.5 那条规则的判据入口。
 *
 * 源 OS 的等级来自 PCB(`pcb.h:113` 的 `task_level`),而 `add_task()` 里那句
 * `if (new_task->task_level != TASK_APPLICATION_LEVEL)` 决定了
 * **应用级线程永远落在 CPU0**。内核线程全是 `TASK_KERNEL_LEVEL`,所以:
 *
 *   - 平时走 `sched_kthread_create()`(它就是本函数 + `TASK_KERNEL_LEVEL`);
 *   - **自检需要**造一个应用级线程去验证那条规则 —— 否则它就是一段
 *     "编译过、今天不可能被执行"的代码(本项目对那种东西的规矩见 D10/D11)。
 *
 * ⚠ M7 有了用户进程之后,应用级线程就是走这条路创建的。
 */
tcb_t sched_kthread_create_level(void (*entry)(void *), void *arg, const char *name, i32 level);

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
 * ~~`sched_park_self()`~~ —— **M4-11.2 已删**。
 *
 * 它原来做的是"把自己挂起(status = WAIT、wakeup_time = 0)然后切走",
 * 是自检探针的便利设施。源 OS **没有**这个函数,而且它表达错了语义:
 * 探针干完活是"**退出**"(源 OS 的 `process_exit` → `kill_thread` → `DEATH`),
 * 不是"睡下去等一个永远不会来的唤醒"。
 * ⇒ 探针改走 `sched_thread_exit()`,本函数随之没有调用者,按 D10/D11 删除。
 */

/*
 * ★ M4-11.2:线程退出 —— 源 OS 两段式的**第一段** ★
 *
 * ← `process_exit()` `pcb.cpp:494-505` + `kill_thread()` `:447-458`:
 * 置 `DEATH`(唯一拒绝条件是 `TASK_IDLE_LEVEL`)、让出、然后永久停住
 * (`while (true) hlt` 的 ARM 对应物是 `wfi`)。
 *
 * ★★ **不释放任何资源** ★★ —— 这是**决定**,不是没做完:
 * 源 OS 的 `kill_thread0()`(还栈 + 摘队 + 由调用者 free TCB)对挂在
 * `kernel_group` 上的线程**没有任何可达路径**(`kill_proc` 直接拒绝
 * "Cannot kill System process."),而 2026-09-13 的决定是**不改源 OS 的行为**。
 * 完整证据链与代价见 `docs/PTASK.md` §2.5/§4.0/§9.3。
 *
 * ⇒ 后果(必须知道):内核栈池**只增不减**,`KSTACK_SLOTS` 就是
 *   "每次启动能创建的内核线程数上限"。容量判据是 `kstack_peak_used`
 *   与 `kstack_headroom`。
 *
 * ⚠ 本函数**不返回**(idle 调用时除外 —— 那时它被拒绝并计数)。
 */
void sched_thread_exit(void);

/* 走到过退出路径的线程数(判据的非空转条件:必须 > 0)*/
u32 sched_thread_exit_count(void);

/* "有代码想停 idle" 的次数 —— 源 OS 也会拒绝,这里做成可读的计数(必须 0)*/
u32 sched_thread_exit_refused(void);

/*
 * ★ 破坏性 A/B 开关 ★ 置 1 时退出路径写 `WAIT` 而不是 `DEATH`
 * (= M4-11.2 之前 `sched_park_self()` 的行为)。两边"线程停住了"一样,
 * 差别只在名册上的状态 ⇒ 它证明的是"退出语义真的接上了"。
 * 生产路径恒为 0。
 */
void sched_set_exit_legacy(u32 on);

/*
 * 数某个核名册上处于 `st` 状态的线程 —— 11.2 判据的**独立**数据源
 * (不看退出路径自己报的数,而是去名册里数;退出的线程按源 OS 留在名册上)。
 */
u32 sched_runq_count_status(u32 cpu_id, TaskStatus st);

/* ------------------------------------------------------------------ */
/* ★ 睡眠与唤醒(M4-8.4)★                                             */
/* ------------------------------------------------------------------ */

/*
 * ← `scheduler_sleep_ns()` `scheduler.cpp:468-493`,逐句照抄。
 *
 * 睡 `ns` 纳秒。实现是**轮询式**的:置 `status = WAIT` 之后反复 `yield()`,
 * 每次都被调度器的扫描看一眼醒没醒。
 *
 * ★ 源 OS 的三处调用点全是这个形状 ★
 *   `ipc.cpp:37,46` 与 `sys.cpp:618`:
 *       do { scheduler_sleep_ns(1000000ULL); …干活…; } while (!条件);
 *   也就是说"睡一会儿→干活→再看"就是源 OS 表达"周期性任务"的方式 ——
 *   M4-8.4 的 1Hz 状态行线程照这个写。
 *
 * ⚠ 谁会把它叫醒:`sched_select_next()` 里那次**全表扫描**
 *   (`sched_queue_scan` → `sched_wake_if_due`)。睡眠的任务**留在队列里**
 *   等着被扫到 —— 这正是 M4-8 的"取队首"版本做不到的事。
 */
void sched_sleep_ns(u64 ns);

/*
 * ← `scheduler_wake_task()` `scheduler.cpp:495-509`。
 *
 * 显式唤醒(不是"到点自动醒"):IPC / futex / 将来的可睡眠锁靠它。
 * 只对 WAIT 的任务有效;唤醒时给一次睡醒补偿(基准取当前任务的 vruntime)。
 */
void sched_wake_task(tcb_t t);

/*
 * ★ 两个破坏性 A/B 的开关 ★
 *
 * `g_reloc_skip`:置 1 时 `sched_tick` 照做完一切,只在最后一步
 * **不把新帧交出去** —— 决策说切走了,而执行流一步没动。
 * `g_vfp_skip`:置 1 时**跳过浮点现场的保存/恢复**,其余照做。
 *
 * 两者都只在自检报告**之后**短暂置 1,生产路径恒为 0。
 * (定义与详细理由在 src/sched_kern.c。)
 */
extern u32 g_reloc_skip;

/*
 * ★ 第二个破坏性 A/B:跳过浮点现场的保存/恢复 ★
 *
 * 置 1 时 `_vec_irq` 里那两条 `arch_vfp_save_current` /
 * `arch_vfp_restore_current` 变成空操作(它们自己在汇编里读这个变量 ——
 * 见 boot/context.S 的说明),其余一切照做。
 * 也就是 M4-9.5 之前的样子。
 *
 * 预期可观测的差别:两个都在用 `double` 的线程,**其中一个**会发现
 * 自己的 d0-d31 变成了对方的图案(它跑在"另一个线程的浮点现场"上)。
 * 这正是"浮点现场也是现场"这条判据承重的证明方式。
 *
 * ⚠ 它的**定义在汇编里**(`boot/context.S` 的 `.data`)—— 读者是必须待在
 *   C 调用链之外的那两条 `bl`,不能为了问一句"跳不跳"再调进 C。
 *
 * 生产路径上恒为 0。
 */
extern u32 g_vfp_skip;

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
 * ★ M4-8.5:tick 里的完整调度决策 —— 而且**真的搬帧** ★
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
 *
 * ★ M4-10:两个核都走这一条路 ★ 队列与计数器都是**本核**的,
 *   选取在本核队列的锁里做。
 */
arm_exc_frame_t *sched_tick(arm_exc_frame_t *frame);

/* ------------------------------------------------------------------ */
/* ★ M4-10:每核 idle / 每核队列 / 选核 ★                              */
/* ------------------------------------------------------------------ */

/*
 * 把**本核**的启动上下文注册成 idle(照源 OS)。
 *
 * BSP 版(`sched_register_boot_idle`)由 kmain 调,BSP 的 idle 就是启动流程自己;
 * AP 版(`sched_register_ap_idle`)由 `cpu1_main` 调,循环体就是那个
 * `while (true) pause`(本板是 `loops++ / stress / relax`)。
 *
 * ⚠ AP 版必须在**本核**调用,而且必须在 `online` 之前 —— 见 src/smp.c:
 *   那一步之后 `online == 1` 就蕴含"本核的调度器就绪"(源 OS 的
 *   `scheduler_is_ready == cpu_count` 握手,`main.cpp:581-585`)。
 */
void sched_register_ap_idle(void);

/* 指定核的 idle TCB(诊断/判据用),越界或未注册返回 NULL */
tcb_t sched_idle_of(u32 cpu_id);

/* 指定核的就绪队列长度。**不取锁** —— 它是诊断量,允许看到瞬态 */
u32 sched_cpu_runq_len(u32 cpu_id);

/*
 * ★ M4-10 的选核开关(对照组)★
 *
 * 置 1 时 `sched_kthread_create()` **永远把线程放进 CPU0 的队列** ——
 * 也就是 M4-10 之前的行为(那时只有 CPU0 参与调度)。
 *
 * 它有两个用途,而且是同一件事的两面:
 *   1. **既有判据的保护罩**:M4-9 的 `saw=(1,1)`、M4-9.5 的 VFP 现场、
 *      M4-8.4 的唤醒延迟、M4-8.5 的无饥饿都是**单核判据** ——
 *      线程一旦被分到 CPU1,它们量的就不再是同一件事(两个核各跑各的,
 *      "交错""被对方改掉寄存器"根本不会发生)。所以那几相显式把它置 1。
 *   2. **M4-10 自己的破坏性对照组**:正常是"挑最短队列"⇒ 两核都在跑;
 *      置 1 ⇒ CPU1 的 `switched` 恒为 0(而 CPU0 队列里堆着全部线程)。
 *
 * 生产路径上恒为 0。
 */
void sched_set_pick_cpu0(u32 on);

/* 诊断:本核累计切换次数(= 每核结构里的 `switched`)*/
u32 sched_switch_count(void);

/* 诊断:本核在 tick 里**真的完成**的切换次数(= 搬帧次数)*/
u32 sched_tick_switched(void);
/* 诊断:其中"被换下的是真实线程"(即真正意义上的抢占)的次数 */
u32 sched_tick_preempted(void);
/* 诊断:挑到了别人、但目标上下文不可切换而放弃的次数 */
u32 sched_tick_invalid_ctx(void);

/* ------------------------------------------------------------------ */
/* ★ 破坏性对照组专用:ctx 的快照 / 还原 ★                            */
/* ------------------------------------------------------------------ */

/*
 * 把**队列里每个线程的 `ctx`** 按遍历顺序抄进 `out`(最多 `max` 个),
 * 返回抄了几个;还原时按同样的顺序写回。
 *
 * 为什么对照组需要它:`g_reloc_skip = 1` 时"决策照做、执行流不动",
 * 而**收现场那一步照做** —— 于是每换一次 current,就把 kmain 的现场
 * 写进那个线程的 ctx。对对照组自己的线程那是目的;对别的常驻线程
 * (M4-8.4 的周期状态线程)就是污染:它的 `ctx.sp` 会指到 kmain 的启动栈,
 * 从此**再也切不进去**(`sched_ctx_switchable` 一直拒绝它)。
 *
 * 上板实测过:状态线程的 `ctx.sp` 变成 0x0018BF10、`invalid_ctx` 涨到 85139、
 * **状态行从此不再输出**。所以对照组必须自己把这段窗口罩起来。
 *
 * ⚠ 只抄队列里的线程 —— 只有它们可能被 `sched_select_next` 挑中。
 * ⚠ 窗口内不许创建/销毁线程(对照组自己保证)。
 */
u32  sched_ctx_snapshot_all(arm_task_ctx_t *out, u32 max);
void sched_ctx_restore_all(const arm_task_ctx_t *in, u32 n);

/* 一次快照能装下的线程数上限(队列不会比这更长;超出的会被忽略) */
#define SCHED_CTX_SNAPSHOT_MAX 32u

#ifdef __cplusplus
} /* extern "C" */
#endif