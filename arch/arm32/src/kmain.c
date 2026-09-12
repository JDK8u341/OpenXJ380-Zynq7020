/*
 * ARMv7-A / Zynq-7020 内核入口(M0 骨架)
 *
 * 对应 x86_64 侧的 KernelMain(kernel/main.cpp)。
 * 当前是移植计划 M0 阶段的骨架:目标是证明
 *   工具链 -> 构建系统 -> 链接 -> JTAG 直载 -> 板级可见输出
 * 这条链路在 ARM 侧成立。
 *
 * 与 x86 侧 KernelMain 的差异:
 *   - 不接收 FrameBufferConfig / EFI_SYSTEM_TABLE / BOOT_CONFIG
 *     (Zynq 上没有 UEFI,资源来自静态板级描述符,见 arch/platform.h)
 *   - 启动顺序:定时器 -> 串口自标定 -> 控制台 -> 外设
 *     而不是 x86 的 CPU -> IDT/GDT -> HPET/APIC -> ...
 *   - 通过 OCM 心跳向 JTAG 汇报状态,不依赖串口
 */

#include <arch/cache.h>
#include <arch/console.h>
#include <arch/cpu.h>
#include <arch/fault_test.h>
#include <arch/heartbeat.h>
#include <arch/io.h>
#include <arch/irq.h>
#include <arch/led.h>
#include <arch/mmu.h>
#include <arch/platform.h>
#include <arch/timer.h>
#include <arch/types.h>
#include <arch/uart_ps.h>

/*
 * 心跳槽位定义在 arch/heartbeat.h —— 它是跨模块契约:
 * 写这个区的除了本文件,还有那些"中途可能回不来、必须自己留标记"
 * 的模块(如 MMU 启动)。槽位定义只能有一处。
 */

/* ------------------------------------------------------------------ */
/* 周期 tick 处理函数                                                  */
/* ------------------------------------------------------------------ */

static volatile u32 g_tick_seen;

/*
 * 中断延迟测量。
 *
 * 为什么需要:私有定时器是**电平触发**的。如果 CPU 有一段时间没去响应,
 * 定时器连expire 多次也只会被合并成一次中断 —— 计数上表现为"丢了 N 个 tick",
 * 但寄存器、GIC 状态全都正常,光看现场分不出"丢中断"和"时钟变慢"。
 *
 * 直接记录相邻两次中断之间全局定时器走过的拍数,就能把这件事量化:
 * 正常是 1ms 对应的 333333 拍,一旦出现远大于它的值,
 * 说明确实有一段中断没有被及时响应,而且能直接读出停了多久。
 */
static volatile u32 g_tick_last_gt;
static volatile u32 g_tick_max_gap;
static volatile u32 g_tick_first;

/* 全局定时器 333333343Hz -> 拍数换算成微秒 */
#define GT_TICKS_TO_US(t) ((u32)(((u64)(t) * 1000000ull) / (u64)PLAT_GLOBAL_TIMER_FREQ_HZ))

static void tick_handler(u32 intid, void *arg)
{
    u32 now;

    (void)intid;
    (void)arg;

    /*
     * 必须先清定时器的中断标志再返回。
     * 私有定时器是电平式输出,不清标志的话 GIC 会立刻再报一次,
     * 表现为"进了中断就再也出不来",而且从寄存器上看一切正常。
     */
    a9_timer_clear_irq();

    now = timer_read_ticks_low();

    /*
     * ⚠ 第一次中断必须单独处理,不能拿它去更新 max_gap。
     * g_tick_last_gt 初值是 0,而此刻全局定时器早已跑了上千万拍,
     * 于是第一次算出来的"间隔"其实是"从定时器归零到现在"的绝对值 ——
     * 实测会读出 1013343us 这种数字,看上去像一次长达 1 秒的中断丢失,
     * 实际什么都没发生。这个伪影第一次就把我误导了一轮。
     */
    if (g_tick_first == 0u) {
        g_tick_first = 1u;
        g_tick_last_gt = now;
        g_tick_seen++;
        return;
    }

    {
        u32 gap = now - g_tick_last_gt;

        g_tick_last_gt = now;

        if (gap > g_tick_max_gap) {
            g_tick_max_gap = gap;
        }
    }

    g_tick_seen++;
}


static void fail_stop(void)
{
    /*
     * 不可恢复的早期失败:停在这里并闪烁 PS LED,
     * 让板上能看出"停住了"而不是"没跑"。
     */
    for (;;) {
        led_ps_set(true);
        timer_delay_ms(200);
        led_ps_set(false);
        timer_delay_ms(200);
    }
}

void kmain(void)
{
    u32      uart_clk = 0;
    u32      clock_source = 0;
    u32      loop_count = 0;
    u32      last_step  = 0xFFFFFFFFu;
    u32      last_ps    = 0xFFFFFFFFu;
    u32      before_us  = 0; /* 使能缓存前的基准耗时 */
    bool     uart_present;

    /* static: 由 BSS 自动清零。这样在"无串口"路径下打印诊断也不会读未初始化值 */
    static uart_baud_result_t baud_result;

    /* ---- 1. 先把 LED 弄起来 ---- */
    /*
     * 串口依赖未知的参考时钟,而 LED 只依赖已经验证过的 GPIO 通路。
     * 所以先做 LED:即使后面串口标定失败,板上也一定有可见反馈。
     */
    led_init();
    HB[HB_SLOT_MAGIC] = PLAT_HEARTBEAT_MAGIC;

    /* ---- 2. 时间基准 ---- */
    /*
     * ⚠ 必须在任何 timer_delay_* 之前初始化。
     * 自检延时和 fail_stop() 都依赖全局定时器在跑;
     * 若顺序反了,计数器不递增,延时函数会永远等下去。
     */
    timer_init();

    /* 上电自检:确认 8 个 LED 通路 */
    led_pl_set(0xFFu);
    led_ps_set(true);
    timer_delay_ms(150);
    led_pl_set(0x00u);
    led_ps_set(false);

    /* ---- 3. 控制台 ---- */
    /*
     * 先探测 UART 是否真的在响应。
     *
     * 本板实测:现有 AXI_GPIO_1 设计的 ps7_init 没有打开 UART 的 APER 外设
     * 时钟门控(APER_CLK_CTRL bit12/bit13 = 0),导致 UART 寄存器读回恒为 0。
     * 此时若直接轮询 TX FIFO,内核会在标定阶段死循环。
     * 所以"有没有串口"必须是可探测的,而不是靠轮询去撞。
     */
    uart_present = uart_probe(PLAT_CONSOLE_UART_BASE);

    if (uart_present) {
        /*
         * 闭环收敛参考时钟。
         *
         * 不用单点外推(见 uart_converge_ref_clk 注释):本板上那条外推
         * 偏低 7.033 倍,真值约 100.5MHz。闭环不依赖模型,直接在工作点
         * 反复"设定->实测->修正"直到实际波特率与目标相符。
         */
        uart_clk = uart_converge_ref_clk(PLAT_CONSOLE_UART_BASE, PLAT_CONSOLE_BAUD);

        if (uart_clk != 0) {
            clock_source = 1; /* 闭环收敛成功 */
        } else {
            uart_clk     = 100500000u; /* 本板实测值,收敛失败时的兜底 */
            clock_source = 2;
        }

        /*
         * 先用 uart_init 拿到波特率协商结果用于打印诊断,
         * 再让 console 绑定到同一个 UART(console_init 内部会再初始化一次,
         * 参数相同,是幂等的)。
         */
        uart_init(PLAT_CONSOLE_UART_BASE, uart_clk, PLAT_CONSOLE_BAUD, &baud_result);
        console_init(PLAT_CONSOLE_UART_BASE, uart_clk, PLAT_CONSOLE_BAUD);
    }

    HB[HB_SLOT_UARTCLK] = uart_clk;
    HB[HB_SLOT_UARTOK]  = uart_present ? 1u : 0u;

    /*
     * 把"怎么得到这个 uart_clk 的"一并记录。
     * uart_clk 本身不足以判断可用性:收敛成功和兜底猜值可能都是同一个数字,
     * 而只有前者能保证线路上真的是 9600。
     */
    HB[HB_SLOT_CLKSRC]   = clock_source;
    HB[HB_SLOT_CONVITER] = uart_converge_last_iters();
    HB[HB_SLOT_BAUDGEN]  = baud_result.baudgen;
    HB[HB_SLOT_BAUDDIV]  = baud_result.bauddiv;

    /*
     * ---- 4. 启动横幅 ----
     * 到这里 uart_clk 已经过闭环验证:实际波特率与 PLAT_CONSOLE_BAUD 相符,
     * 所以下面这些文字应当是终端上可以直接读到的。
     */
    console_puts("\n");
    console_puts("================================================\n");
    console_puts(" OpenXJ380 / ARMv7-A (Zynq-7020)\n");
    console_puts(" M0 skeleton - build, link, JTAG load, run\n");
    console_puts("================================================\n");

    console_printf(" CPU          : %u Hz\n", PLAT_CPU_FREQ_HZ);
    console_printf(" Global timer : %u Hz\n", PLAT_GLOBAL_TIMER_FREQ_HZ);
    console_printf(" UART ref clk : %u Hz (source=%u iters=%u, %s)\n", uart_clk, clock_source,
                   uart_converge_last_iters(),
                   clock_source == 1 ? "self-calibrated" : "fallback");
    console_printf(" Baud         : requested=%u actual=%u (BAUDGEN=%u BAUDDIV=%u err=%u ppm)\n",
                   baud_result.requested, baud_result.actual, baud_result.baudgen, baud_result.bauddiv,
                   baud_result.error_ppm);
    console_printf(" DDR base     : 0x%08X size 0x%08X\n", PLAT_DDR_BASE, PLAT_DDR_SIZE);
    console_printf(" Kernel       : 0x%08X\n", PLAT_KERNEL_LOAD);
    console_printf(" PS GPIO DIRM0: 0x%08X OEN0: 0x%08X\n", led_get_dirm0(), led_get_oen0());
    console_puts("------------------------------------------------\n");
    console_printf(" Uptime at banner end: %u us\n", (u32)timer_read_us());
    console_puts("\n");

    if (baud_result.valid) {
        console_puts(" If you can read this, the serial console works.\n\n");
    } else {
        console_puts(" WARNING: baud error out of range, output may be garbled.\n\n");
    }

    HB[HB_SLOT_DIRM0] = led_get_dirm0();

    /* ---- 5. 中断子系统(GIC + 周期 tick) ---- */
    /*
     * 顺序有讲究:
     *   1) gic_init() 在关中断状态下配置 Distributor/CPU Interface
     *   2) 注册处理函数(注册与使能分离,这是与 x86 侧最大的结构差异)
     *   3) 启动定时器并让 GIC 放行该 INTID
     *   4) 最后才打开 CPU 的中断响应
     * 反过来的话,中断可能在处理函数登记之前就打进来。
     */
    console_puts(" IRQ init: vectors installed, configuring GIC...\n");

    gic_init();

    if (irq_register(GIC_INTID_A9_PRIVATE_TIMER, tick_handler, NULL) != 0) {
        console_puts(" WARN: 定时器 INTID 29 已被占用,周期 tick 未启用\n");
    }

    a9_timer_start_tick(1000u);                            /* 1 ms 一次 */
    gic_set_priority(GIC_INTID_A9_PRIVATE_TIMER, 0x80u);   /* 数值越小优先级越高 */
    gic_enable_irq(GIC_INTID_A9_PRIVATE_TIMER);

    irq_global_enable();

    /* 给中断一点时间跑起来,再报告结果 */
    timer_delay_ms(20);

    console_printf(" IRQ status  : ticks=%u irq_count=%u last_intid=%u spurious=%u\n",
                   g_tick_seen, irq_get_stats()->irq_count, irq_get_stats()->last_intid,
                   irq_get_stats()->spurious_count);

    if (g_tick_seen > 0) {
        console_puts(" Periodic tick is RUNNING (1 kHz, Cortex-A9 private timer).\n\n");
    } else {
        console_puts(" WARNING: no tick observed - GIC or timer not delivering.\n\n");
    }

    HB[HB_SLOT_TICKS] = g_tick_seen;

    /* ---- 6. MMU ---- */
    /*
     * 位置是有意选的:放在中断子系统验证**之后**。
     *
     * 这样串口上就有了明确的前后对照 ——
     * 横幅与 IRQ 状态行证明"开 MMU 之前一切正常",
     * 之后主循环的状态行证明"开 MMU 之后仍然正常"。
     * 若把 MMU 放在最前面,一旦出错就只能看到"什么都没输出",
     * 无法区分是页表错了还是别的地方本来就坏了。
     *
     * 真正的兜底是心跳槽 HB_SLOT_MMUSTAGE:
     * 打开地址转换后 C 代码未必还能跑,但那个槽是开 MMU 的代码
     * 自己在每一步之前写的,JTAG 一定能读到。
     */
    console_puts(" MMU: building page table (1MB sections, identity map)...\n");
    timer_delay_ms(20);

    mmu_enable();

    /*
     * 能执行到这里本身就说明了一件事:上面的 mmu_enable() 里
     * 打开 SCTLR.M 之后,**取指与访存都经过了页表**并且成功了。
     * 否则根本回不到这个函数。
     */
    console_printf(" MMU         : %s  stage=%u SCTLR=0x%08X TTBR0=0x%08X\n",
                   mmu_is_enabled() ? "ENABLED" : "disabled", HB[HB_SLOT_MMUSTAGE],
                   arch_read_sctlr(), arch_read_ttbr0());
    console_printf(" MMU regions : OCM/心跳=\"%s\"  DDR=\"%s\"  UART=\"%s\"  GIC=\"%s\"\n",
                   mmu_region_name_for(PLAT_HEARTBEAT_BASE), mmu_region_name_for(PLAT_KERNEL_LOAD),
                   mmu_region_name_for(PLAT_CONSOLE_UART_BASE), mmu_region_name_for(0xF8F01000u));

    if (!mmu_is_enabled()) {
        console_puts(" WARNING: MMU did not enable - see heartbeat slot 16\n");
    } else {
        /*
         * 自检:地址转换开着的情况下,读回一个已知常量。
         * 这是在验证"数据访问经页表后仍然取到正确的值",
         * 而不只是"没崩"。用区域名函数的返回值做样本 ——
         * 它来自 .rodata,落在 DDR 段里。
         */
        const char *probe = mmu_region_name_for(PLAT_KERNEL_LOAD);
        console_printf(" MMU selftest: .rodata readback \"%s\" (%s)\n", probe,
                       probe[0] == 'D' ? "OK" : "MISMATCH");
    }
    console_puts("\n");

    /* ---- 7. 缓存几何 ---- */
    /*
     * 这里只是**读取并打印**,不使能缓存 —— 使能是下一步(M2-5b)的事。
     *
     * 之所以要先单独做这一步:几何解码里三个字段全是"减一/减四"存储的
     * (见 arch/cache.h),少加一个 1 不会报错,只会让之后的整块失效
     * 少覆盖一行/一路,于是残留脏行在某个时刻被写回、静默覆盖正确数据。
     * 先把真实芯片的 CLIDR/CCSIDR 读出来对照,比等缓存开了之后
     * 再出问题去猜要容易得多。
     *
     * 已知答案:Cortex-A9 的 L1 是 32KB / 4 路 / 32 字节行。
     */
    {
        cache_geometry_t dgeo  = cache_discover(false, 0u);
        cache_geometry_t igeo  = cache_discover(true, 0u);
        u32              clidr = arch_read_clidr();
        bool             d_ok  = (dgeo.total_bytes == 32768u) && (dgeo.ways == 4u) && (dgeo.sets == 256u) &&
                                 (dgeo.line_bytes == 32u);
        bool             i_ok  = (igeo.total_bytes == 32768u) && (igeo.ways == 4u);

        console_printf(" Cache CLIDR : 0x%08X  (LoC=%u LoUIS=%u L1type=%u)\n", clidr, cache_loc(clidr),
                       cache_louis(clidr), cache_level_type(clidr, 0u));
        console_printf(" Cache L1 D  : %u B, %u-way, %u sets, %u B/line\n", dgeo.total_bytes, dgeo.ways,
                       dgeo.sets, dgeo.line_bytes);
        console_printf(" Cache L1 I  : %u B, %u-way, %u sets, %u B/line\n", igeo.total_bytes, igeo.ways,
                       igeo.sets, igeo.line_bytes);
        console_printf(" Cache check : %s (expect D and I both 32KB 4-way 32B/line 256 sets)\n",
                       (d_ok && i_ok) ? "MATCH" : "MISMATCH");

        /*
         * 使能前先跑一遍基准。
         *
         * 这一段本身在**无缓存**下执行(代码从 DDR 取指),所以耗时很长 ——
         * 这正是我们要拿来当对照的值。200 遍 × 4KB 读。
         */
        before_us = cache_benchmark_us(200u);
    }
    console_puts("\n");

    /* ---- 8. 使能 L1 缓存 ---- */
    /*
     * 顺序不能变:先做 coherency 前置条件(SCU + ACTLR),
     * 再使能缓存。
     *
     * ⚠ 漏掉 coherency_init 的症状极具迷惑性 —— 地址转换照常、系统照常跑、
     *   SCTLR 的 C/I 位读回来都是 1,唯独缓存完全不起作用。
     *   原因是 DDR 映射为 Shareable(S=1),而 SCU 未使能、ACTLR.SMP 未置位时
     *   Cortex-A9 不会把可共享访问放进 L1。本项目踩过一次,
     *   靠下面的耗时基准才发现 —— 光看寄存器是发现不了的。
     */
    console_puts(" Cache: enabling SCU + ACTLR (coherency preconditions)...\n");
    cortexa9_coherency_init();

    console_puts(" Cache: invalidating and enabling L1 D/I-cache...\n");
    timer_delay_ms(20);

    cache_enable_l1();

    /*
     * 能执行到这里就已经说明了一部分事情:cache_enable_l1() 里写 SCTLR
     * 之后,**取指与访存都经过了缓存**并且成功了。否则回不到这一行。
     */
    {
        u32 after_us = cache_benchmark_us(200u);

        console_printf(" Caches      : D=%s I=%s  SCTLR=0x%08X\n",
                       cache_dcache_enabled() ? "ON" : "off", cache_icache_enabled() ? "ON" : "off",
                       arch_read_sctlr());
        console_printf(" Coherency   : SCU=0x%08X ACTLR=0x%08X\n", cortexa9_scu_status(),
                       cortexa9_actlr_status());
        console_printf(" Cache bench : off=%u us  on=%u us  speedup=%ux\n", before_us, after_us,
                       (after_us > 0u) ? (before_us / after_us) : 0u);

        /*
         * 判断依据是**加速比**,不是寄存器位。
         *
         * 缓存使能后如果耗时几乎没变,说明缓存没有真正覆盖到这条路径
         * (最常见的原因是内存属性被写成了不可缓存)。这种情况下
         * SCTLR 的 C 位照样是 1,系统也照样跑 —— 只有这个比值能揭穿。
         */
        if (after_us > 0u && before_us > (after_us * 2u)) {
            console_puts(" Cache check : caches are demonstrably effective\n");
        } else {
            console_puts(" Cache WARN  : little speedup - caches may not be covering this memory\n");
        }

        HB[HB_SLOT_CACHEBENCH] = (after_us > 0u) ? (before_us / after_us) : 0u;
    }
    console_puts("\n");

    /* ---- 9. 主循环 ---- */
    /*
     * 节奏完全由全局定时器决定,不依赖软件延时循环 ——
     * 这样即使 CPU 频率变化,闪烁频率也保持一致。
     *
     * 有串口时约每秒打印一条状态行并持续输出。
     * 在"不确定哪个 COM 口 / 波特率对不对"的阶段,一个稳定可预期的
     * 周期信号比一次性的启动横幅好找得多。
     */
    u32 last_print = 0xFFFFFFFFu;

    for (;;) {
        u32 gt    = timer_read_ticks_low();
        u32 step  = gt >> 26;                  /* 333.33MHz >> 26 约 5 Hz */
        u32 ps_ph = (gt >> 28) & 1u;           /* 约 1.2 Hz */

        if (ps_ph != last_ps) {
            led_ps_set(ps_ph != 0);
            last_ps = ps_ph;
            HB[HB_SLOT_PSLED] = ps_ph;
        }

        if (step != last_step) {
            u32 sw  = sw_read();
            u32 bar = 1u << (step & 7u);

            /* 跑马灯;某位拨码开关闭合时该位取反 -> 占空比 1/8 变 7/8 */
            led_pl_set((u8)(bar ^ sw));

            last_step = step;
            loop_count++;

            HB[HB_SLOT_LOOP] = loop_count;
            HB[HB_SLOT_LED]  = bar ^ sw;
            HB[HB_SLOT_SW]   = sw;
            HB[HB_SLOT_GT]   = gt;
        }

        if (uart_present) {
            /* gt >> 29 在 333.33MHz 下约 1.6 秒一次 */
            u32 tick = gt >> 29;

            if (tick != last_print) {
                last_print = tick;

                /*
                 * 同时汇报两套计数,便于交叉验证:
                 *   ticks     —— 周期中断次数,应当约等于 uptime(ms)
                 *   irq_count —— GIC 实际转发的中断总数
                 * 两者若明显不符,说明有中断被吞或未被 EOI。
                 */
                HB[HB_SLOT_TICKS]    = g_tick_seen;
                HB[HB_SLOT_IRQCOUNT] = irq_get_stats()->irq_count;
                HB[HB_SLOT_TICKGAP]  = GT_TICKS_TO_US(g_tick_max_gap);

                /*
                 * uptime 与 ticks 的差就是"累计欠下的 tick 数":
                 * 若中断一次不丢,两者应当始终相差一个固定值。
                 * maxgap 是最大单次延迟,正常应贴着 1000us。
                 */
                console_printf("[XJ380/arm32] alive loop=%u led=0x%02X ticks=%u irq=%u lag=%u ms maxgap=%u us\n",
                               loop_count, (u32)(HB[HB_SLOT_LED] & 0xFFu), g_tick_seen,
                               irq_get_stats()->irq_count,
                               (u32)(timer_read_us() / 1000u) - g_tick_seen,
                               GT_TICKS_TO_US(g_tick_max_gap));
            }
        }

        /*
         * 故障注入钩子:JTAG 往 PLAT_FAULT_SEL_ADDR 写码即触发对应异常,
         * 用来验证异常诊断路径。详见 arch/fault_test.h。
         */
        fault_test_poll();
    }

    /* 不会到这里 */
    fail_stop();
}
