#pragma once

/*
 * SMP:引导第二个核(AM3-2 / AM3-3)
 *
 * ── Zynq 上为什么是 SEV 而不是 x86 的 INIT-SIPI-SIPI ──────────────────
 * x86 靠 APIC 发 INIT 再发两次 SIPI 来拉起 AP。ARM 上没有这套,
 * Cortex-A9 MPCore 的做法是(UG585 §6.1.10):
 *
 *   1. BootROM 把 CPU1 停在 0xFFFFFE00..0xFFFFFFF0 的 WFE 上;
 *   2. CPU0 把入口地址写进 **0xFFFFFFF0**;
 *   3. CPU0 执行 **SEV**,CPU1 被唤醒后读 0xFFFFFFF0 并跳过去。
 *
 * 两个硬约束:
 *   - **入口必须是 ARM-32 指令集**(不能是 Thumb)。所以 cpu1_entry 在
 *     start.S 里,文件头就 `.arm`,不能改成 thumb 编译。
 *   - 0xFFFFFFF0 里如果放的是无效地址,CPU1 会跑飞。BootROM 预置的
 *     "安全网"值(指向它自己的 WFE)就是防这个的,我们覆盖它之前
 *     必须确保 cpu1_entry 已经就绪。
 *
 * ── 为什么不需要 OCM 跳板 ────────────────────────────────────────────
 * 移植计划原文写的是"OCM 跳板 + sev",那是照搬"AP 需要一个实模式
 * 可达的低地址"的 x86 思路。ARM 不需要:
 *   0xFFFFFFF0 可以放**任意 32 位地址**,而内核链接在物理 0x00100000、
 *   CPU1 起来时 MMU 关着 —— 物理地址本来就直接可达。
 *   所以直接指向内核里的 cpu1_entry 即可,跳板纯属多余。
 *   (这正是计划里"与原文的偏差"该记一笔的地方。)
 */

#include <arch/percpu.h>
#include <arch/types.h>

/*
 * CPU1 的入口地址槽。UG585 §6.1.10。
 * 0xFFFFFE00..0xFFFFFFF0 是 BootROM 保留区,不要往那里面放东西。
 */
#define SMP_CPU1_START_ADDR ((volatile u32 *)0xFFFFFFF0u)

/*
 * start.S 里的 CPU1 入口。
 *
 * 声明成无参无返回的函数只是为了拿到符号地址;
 * 它的 ABI 与真正的 C 调用约定无关(CPU1 跳过去时并不知道自己从哪来)。
 */
extern void cpu1_entry(void);

/*
 * CPU1 的 C 侧主函数(start.S 的 cpu1_entry 尾调用进入)。
 * 永不返回。
 */
void cpu1_main(void) __attribute__((noreturn));

/*
 * 放 CPU1 起来。
 *
 * 只负责"发信号",不等待 —— 等待交给 smp_wait_online(),
 * 这样调用方可以决定超时后是继续跑还是报错。
 */
void smp_release_cpu1(void);

/*
 * 等待指定核置 online 位。返回 true 表示等到了。
 *
 * ⚠ 必须带超时。CPU1 若因任何原因起不来,"无限等"会把一个可诊断的
 *   降级变成整机挂死 —— 本项目在 UART 轮询上已经踩过一次同样的坑。
 */
bool smp_wait_online(u32 cpu_id, u32 timeout_us);

/* ------------------------------------------------------------------ */
/* AM3-4 / AM3-5:每核中断、SGI 做 IPI、spinlock 互斥性验证            */
/* ------------------------------------------------------------------ */

/* 每核压力测试的轮数。两核各跑这么多,最终计数应当是它的两倍 */
#define SMP_STRESS_ROUNDS 20000u

/*
 * IPI 往返次数。每次都要等目标核处理完再发下一个(SGI 不排队),
 * 所以这个数字直接决定耗时:8 次 x 最坏 5ms 上限,正常只要几十微秒。
 */
#define SMP_IPI_ROUNDS 8u

typedef struct
{
    u32 counter;    /* 临界区里累加的值,正确时应等于 2 x SMP_STRESS_ROUNDS */
    u32 violations; /* 持锁期间发现 owner 非空闲的次数,正确时应为 0 */
    u32 cpu1_ran;   /* CPU1 是否参与了(1 = 参与) */
    u32 ipi_sent;   /* CPU0 发出去的 IPI 数 */
    u32 ipi_seen;   /* CPU1 收到的 IPI 数 */
} smp_stress_result_t;

/*
 * 跑一次互斥性压力测试:两核各自在同一个自旋锁下累加同一个计数器。
 *
 * 为什么判据是"两次持锁"而不只是"计数对不对":
 * 计数值对不上只能说明**丢了更新**;而 owner 判据能直接抓到
 * **两个核同时进临界区** —— 后者才是自旋锁坏掉的定义,前者只是它的后果之一。
 * (计数器恰好没丢更新但互斥已经破了,是可能的。)
 */
smp_stress_result_t smp_stress_run(void);

/* 注册 SGI 0 的处理函数(共享中断表,只需一次)。返回 0 表示成功 */
int smp_register_ipi(void);

/* 打开**本核**的 SGI 0 使能位(SGI/PPI 的使能位是按核银行化的)*/
void smp_enable_ipi_this_cpu(void);

/* 从本核向 CPU1 发一个 IPI */
void smp_send_ipi_to_cpu1(void);
