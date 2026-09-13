/*
 * Cortex-A9 全局定时器
 *
 * 寄存器(基址 0xF8F00200):
 *   0x00 Counter Low      0x04 Counter High
 *   0x08 Control          0x10 Compare Low   0x14 Compare High
 *
 * Control 位:
 *   bit0 使能   bit1 自动递增(必须置 1,否则计数器不跑)
 *   bit2 中断使能  bit3 比较器使能
 */

#include <arch/io.h>
#include <arch/percpu.h>
#include <arch/platform.h>
#include <arch/timer.h>

#define GT_ENABLE     0x00000001u
#define GT_AUTO_INCR  0x00000002u

/*
 * 全局定时器是只读计数器,没有需要屏障保护的写后读依赖。
 * 直接用 volatile 读,避免 mmio_read32 里每次读后的 dmb 拖慢
 * 64 位读取的一致性判定。
 */
static inline u32 gt_read(u32 offset)
{
    return *(volatile u32 *)(PLAT_GLOBAL_TIMER_BASE + offset);
}

void timer_init(void)
{
    /* 先关闭,清零计数,再以自动递增方式启动 */
    *(volatile u32 *)(PLAT_GLOBAL_TIMER_BASE + GT_CONTROL) = 0;
    *(volatile u32 *)(PLAT_GLOBAL_TIMER_BASE + GT_COUNTER_LOW) = 0;
    *(volatile u32 *)(PLAT_GLOBAL_TIMER_BASE + GT_COUNTER_HIGH) = 0;

    *(volatile u32 *)(PLAT_GLOBAL_TIMER_BASE + GT_COMPARE_LOW) = 0xFFFFFFFFu;
    *(volatile u32 *)(PLAT_GLOBAL_TIMER_BASE + GT_COMPARE_HIGH) = 0xFFFFFFFFu;

    *(volatile u32 *)(PLAT_GLOBAL_TIMER_BASE + GT_CONTROL) = GT_ENABLE | GT_AUTO_INCR;
}

u32 timer_read_ticks_low(void)
{
    return gt_read(GT_COUNTER_LOW);
}

u64 timer_read_ticks(void)
{
    u32 high_first;
    u32 low;
    u32 high_second;

    /*
     * 正确的一致性读法:读高 -> 读低 -> 再读高。
     * 若两次高 32 位相同,说明中间的读低没有被进位打断,结果有效。
     *
     * ⚠ 不要反过来要求"两次低 32 位相等" —— 计数器在 333MHz 递增,
     *   而一次 MMIO 读要几十个周期,低 32 位几乎必然在两次读之间变化,
     *   那样写会陷入无限重试。这是本项目在板上实测踩到的真实故障。
     */
    do {
        high_first  = gt_read(GT_COUNTER_HIGH);
        low         = gt_read(GT_COUNTER_LOW);
        high_second = gt_read(GT_COUNTER_HIGH);
    } while (high_first != high_second);

    return ((u64)high_second << 32) | (u64)low;
}

u64 timer_read_ns(void)
{
    /*
     * ns = ticks * 1e9 / freq
     * 先乘后除,用 64 位中间量避免溢出。ticks 在 333MHz 下
     * 需要约 553 秒才会让 ticks*1e9 超过 2^64,内核启动阶段足够。
     */
    u64 ticks = timer_read_ticks();

    return (ticks * 1000000000ull) / (u64)PLAT_GLOBAL_TIMER_FREQ_HZ;
}

u64 timer_read_us(void)
{
    u64 ticks = timer_read_ticks();

    return (ticks * 1000000ull) / (u64)PLAT_GLOBAL_TIMER_FREQ_HZ;
}

void timer_delay_us(u32 us)
{
    u64 start = timer_read_ticks();
    u64 delta = ((u64)us * (u64)PLAT_GLOBAL_TIMER_FREQ_HZ) / 1000000ull;

    while ((timer_read_ticks() - start) < delta) {
        /* 忙等 */
    }
}

void timer_delay_ms(u32 ms)
{
    while (ms-- > 0) {
        timer_delay_us(1000u);
    }
}

/* ------------------------------------------------------------------ */
/* 周期 tick:Cortex-A9 私有定时器                                      */
/* ------------------------------------------------------------------ */

/* 寄存器偏移(基址 0xF8F00600) */
#define PT_LOAD    0x00u
#define PT_COUNTER 0x04u
#define PT_CONTROL 0x08u
#define PT_ISR     0x0Cu

/* 控制位(取自 AMD xscutimer_hw.h) */
#define PT_CTRL_ENABLE     0x00000001u
#define PT_CTRL_AUTO_RELOAD 0x00000002u
#define PT_CTRL_IRQ_ENABLE 0x00000004u
#define PT_CTRL_PRESCALER_SHIFT 8u /* [15:8],分频 = prescaler + 1 */

static volatile u64 g_tick_count;

void a9_timer_start_tick(u32 hz)
{
    u32 interval;

    if (hz == 0) {
        hz = 1000u;
    }

    /* 递减到 0 触发一次,周期 = (load + 1) / 时钟 */
    interval = PLAT_GLOBAL_TIMER_FREQ_HZ / hz;
    if (interval == 0) {
        interval = 1;
    }

    /*
     * 先关掉再配置。prescaler = 0 表示不分频,
     * 定时器时钟与全局定时器同源(CPU_3x2x)。
     */
    mmio_write32(PLAT_PRIVATE_TIMER_BASE + PT_CONTROL, 0);
    mmio_write32(PLAT_PRIVATE_TIMER_BASE + PT_LOAD, interval - 1u);
    mmio_write32(PLAT_PRIVATE_TIMER_BASE + PT_ISR, 1u); /* 清掉可能残留的标志 */

    mmio_write32(PLAT_PRIVATE_TIMER_BASE + PT_CONTROL,
                 PT_CTRL_ENABLE | PT_CTRL_AUTO_RELOAD | PT_CTRL_IRQ_ENABLE);
}

void a9_timer_stop_tick(void)
{
    mmio_write32(PLAT_PRIVATE_TIMER_BASE + PT_CONTROL, 0);
}

void a9_timer_clear_irq(void)
{
    /* 写 1 清标志。不清的话电平式中断会立刻重新触发,表现为中断风暴 */
    mmio_write32(PLAT_PRIVATE_TIMER_BASE + PT_ISR, 1u);
    arch_dsb();

    /*
     * tick 计数按核分开(AM3-5)。
     *
     * 私有定时器本身是每核银行化的,所以两核各自 1kHz —— 共用一份
     * 全局计数会让它变成 2kHz,而"启动时长"这类基于它的推导会整体偏快,
     * 且看不出哪里不对。
     *
     * g_tick_count 保留为 **CPU0 的 ticks**:a9_timer_get_ticks() 的
     * 既有调用方(启动横幅里的 uptime)语义不变。
     */
    {
        percpu_t *pc = percpu_self();

        if (pc != NULL) {
            pc->ticks++;
            if (pc->cpu_id == 0u) {
                g_tick_count++;
            }
        } else {
            g_tick_count++; /* percpu 还没建好(极早期),按 CPU0 记 */
        }
    }
}

u64 a9_timer_get_ticks(void)
{
    return g_tick_count;
}
