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
