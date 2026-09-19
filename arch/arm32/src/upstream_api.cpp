/*
 * 上游 API 在 ARM 上的落地层 —— M4A-1.2b
 *
 * ====================================================================
 * 这个文件为什么是 C++(而移植侧的其它文件都是 C)
 * ====================================================================
 *
 * 它存在的**唯一理由**就是"把上游的函数名接到移植侧的架构原语上",
 * 因此它必然同时站在两个世界里 —— 这正是本项目那条边界规矩
 * (见 docs/ZYNQ7020_INTEGRATION_PLAN.md §7 第 4 条)说的"交界"。
 *
 * 而交界必须是 **C++**:上游的 `include/task/scheduler.h:10-12` 声明这三个
 * 函数时**没有** `extern "C"`,所以上游调它们用的是 C++ 名字修饰;
 * 一份 C 实现给出的是未修饰符号,链不上(实测报的是
 * `undefined reference to 'scheduler_yield()'`)。
 *
 * ⇒ 移植侧要满足它们,只有两条路:
 *   (a) 给上游两个头都加 `extern "C"` —— 但 `proto.hpp:104-105` 与
 *       `task/scheduler.h` **各声明了一份**,两处都得改,面更大;
 *   (b) 就是这个文件:**用 C++ 写**,签名与上游逐字一致。
 * 选 (b):改动面最小,而且"哪些上游名字需要 ARM 实现"这件事一眼可见。
 *
 * ⚠ 这里**只声明我们真正实现的那几个**,签名逐字照抄上游。
 *   由 `tests/test_arm32_upstream_api.py` 对着上游头做机械比对 ——
 *   上游改了签名而这里没跟,测试会报。
 *
 * ⚠ 也**只**做"转发",不写任何逻辑:逻辑在 `arch/sched.h` 与 `arch/cpu.h`
 *   里(它们已被板上自检覆盖)。转发层掺进逻辑 = 两个地方各有一半真相。
 */

#include <arch/cpu.h>
#include <arch/sched.h>

#include <krlibc.h>

/* ---- 调度器:上游名字 → 移植侧实现 ---- */

/*
 * ← `include/task/scheduler.h:10`
 *
 * 移植侧的 `sched_yield()`(`arch/sched.h:434`)语义与上游一致:
 * 让出当前任务,走**同一条切换路径**(在 ARM 上是 `svc #ARM_SVC_YIELD`)。
 */
void scheduler_yield(void)
{
    sched_yield();
}

/*
 * ← `include/task/scheduler.h:11`
 *
 * `uint64_t` 与移植侧的 `u64` 是同一个类型(都是 `unsigned long long`,
 * 32 位 ARM 上 8 字节)—— 这里刻意照抄上游的写法,让两侧一眼可比。
 */
void scheduler_sleep_ns(uint64_t nano)
{
    sched_sleep_ns(nano);
}

/*
 * ← `include/task/scheduler.h:12`
 *
 * ★ **这个名字移植侧早就有一样的**(`arch/sched.h:515` 的
 * `sched_wake_task`),只是前缀不同。所以本文件对它而言只是改名转发。
 */
void scheduler_wake_task(tcb_t task)
{
    sched_wake_task(task);
}

/* ---- 中断开关:上游名字 → 移植侧实现 ---- */

/*
 * ← `include/proto.hpp:104-105`(上游把这两句放在那个"万能头"里,
 *   而不是某个 cpu/ 头 —— 这里照抄签名,不引用那份头)
 *
 * 上游:`close_interrupt`/`open_interrupt` 是它们的宏别名
 * (`proto.hpp:23-24`),ARM 侧没有那两个宏 —— 用到的话会当场编不过,
 * 那是对的:宏是编译期的,不能靠链接期补。
 */
void disable_intr(void)
{
    arch_irq_disable();
}

void enable_intr(void)
{
    arch_irq_enable();
}
