#pragma once

/*
 * 时间基准:Cortex-A9 全局定时器
 *
 * 对应 x86_64 侧的 kernel/intr/hpet.cpp。原实现依赖 ACPI HPET 表,
 * 而且 init_hpet() 会无条件解引用该指针(见移植计划 §2.12 地雷 1)。
 * Zynq 上没有 ACPI,时间基准来自 SCU 内的全局定时器:
 *   - 基址 0xF8F00200,64 位自由运行计数器
 *   - 递增频率 = CPU_3x2x = CPU_6x4x / 2
 *
 * 实测:CPU 666.667 MHz 时,定时器 2 秒递增 689,673,023 次 -> 333.3 MHz,吻合。
 */

#include <arch/types.h>

/* 初始化并启动全局定时器(清零计数、使能) */
void timer_init(void);

/* 读取 64 位原始计数 */
u64 timer_read_ticks(void);

/* 读取低 32 位。用于差值测量,注意 333MHz 下约 12.9 秒回绕一次 */
u32 timer_read_ticks_low(void);

/* 自启动以来的纳秒数 */
u64 timer_read_ns(void);

/* 自启动以来的微秒数 */
u64 timer_read_us(void);

/* 忙等指定的微秒数 */
void timer_delay_us(u32 us);

/* 忙等指定的毫秒数 */
void timer_delay_ms(u32 ms);

/* ------------------------------------------------------------------ */
/* 周期 tick:Cortex-A9 私有定时器(PPI 29)                              */
/* ------------------------------------------------------------------ */

/*
 * 启动周期中断,取代 x86 侧的 LAPIC 定时器(那里是在
 * kernel/intr/apic.cpp 的 init_lApic() 里对着 HPET 标定出来的)。
 *
 * 私有定时器每个 CPU 一个,时钟为 CPU_3x2x(与全局定时器同源),
 * 递减计数到 0 触发 PPI 29。
 *
 * @param hz  期望的中断频率(如 1000 表示 1ms 一次)
 */
void a9_timer_start_tick(u32 hz);

/* 停止周期中断 */
void a9_timer_stop_tick(void);

/*
 * 清中断标志。必须在每次 tick 处理函数里调用 ——
 * 私有定时器的中断状态是电平式的,不清就会一直重新触发。
 */
void a9_timer_clear_irq(void);

/* tick 计数,用于验证中断确实在按频率发生 */
u64 a9_timer_get_ticks(void);
