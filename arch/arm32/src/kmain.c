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

#include <arch/console.h>
#include <arch/fault_test.h>
#include <arch/io.h>
#include <arch/irq.h>
#include <arch/led.h>
#include <arch/platform.h>
#include <arch/timer.h>
#include <arch/types.h>
#include <arch/uart_ps.h>

/* 心跳区:与平台头文件约定一致,可用 xsdb mrd 读回 */
#define HB ((volatile u32 *)PLAT_HEARTBEAT_BASE)

#define HB_IDX_MAGIC     0u
#define HB_IDX_LOOP      1u
#define HB_IDX_LED       2u
#define HB_IDX_SW        3u
#define HB_IDX_GT        4u
#define HB_IDX_PSLED     5u
#define HB_IDX_UARTCLK   6u
#define HB_IDX_DIRM0     7u
#define HB_IDX_UARTOK    8u
#define HB_IDX_MEASBAUD  9u  /* 当前设定下实测的真实波特率(不外推) */
#define HB_IDX_TICKS     10u /* 周期 tick 计数 */
#define HB_IDX_IRQCOUNT  11u /* GIC 收到的中断总数 */
#define HB_IDX_CLKSRC    12u /* 1=闭环收敛成功 2=收敛失败走兜底 */
#define HB_IDX_CONVITER  13u /* 闭环实际迭代次数 */
#define HB_IDX_BAUDGEN   14u /* 最终生效的 BAUDGEN */
#define HB_IDX_BAUDDIV   15u /* 最终生效的 BAUDDIV */

/* ------------------------------------------------------------------ */
/* 周期 tick 处理函数                                                  */
/* ------------------------------------------------------------------ */

static volatile u32 g_tick_seen;

static void tick_handler(u32 intid, void *arg)
{
    (void)intid;
    (void)arg;

    /*
     * 必须先清定时器的中断标志再返回。
     * 私有定时器是电平式输出,不清标志的话 GIC 会立刻再报一次,
     * 表现为"进了中断就再也出不来",而且从寄存器上看一切正常。
     */
    a9_timer_clear_irq();

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
    bool     uart_present;

    /* static: 由 BSS 自动清零。这样在"无串口"路径下打印诊断也不会读未初始化值 */
    static uart_baud_result_t baud_result;

    /* ---- 1. 先把 LED 弄起来 ---- */
    /*
     * 串口依赖未知的参考时钟,而 LED 只依赖已经验证过的 GPIO 通路。
     * 所以先做 LED:即使后面串口标定失败,板上也一定有可见反馈。
     */
    led_init();
    HB[HB_IDX_MAGIC] = PLAT_HEARTBEAT_MAGIC;

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

    HB[HB_IDX_UARTCLK] = uart_clk;
    HB[HB_IDX_UARTOK]  = uart_present ? 1u : 0u;

    /*
     * 把"怎么得到这个 uart_clk 的"一并记录。
     * uart_clk 本身不足以判断可用性:收敛成功和兜底猜值可能都是同一个数字,
     * 而只有前者能保证线路上真的是 9600。
     */
    HB[HB_IDX_CLKSRC]   = clock_source;
    HB[HB_IDX_CONVITER] = uart_converge_last_iters();
    HB[HB_IDX_BAUDGEN]  = baud_result.baudgen;
    HB[HB_IDX_BAUDDIV]  = baud_result.bauddiv;

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

    HB[HB_IDX_DIRM0] = led_get_dirm0();

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

    HB[HB_IDX_TICKS] = g_tick_seen;

    /* ---- 6. 主循环 ---- */
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
            HB[HB_IDX_PSLED] = ps_ph;
        }

        if (step != last_step) {
            u32 sw  = sw_read();
            u32 bar = 1u << (step & 7u);

            /* 跑马灯;某位拨码开关闭合时该位取反 -> 占空比 1/8 变 7/8 */
            led_pl_set((u8)(bar ^ sw));

            last_step = step;
            loop_count++;

            HB[HB_IDX_LOOP] = loop_count;
            HB[HB_IDX_LED]  = bar ^ sw;
            HB[HB_IDX_SW]   = sw;
            HB[HB_IDX_GT]   = gt;
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
                HB[HB_IDX_TICKS]    = g_tick_seen;
                HB[HB_IDX_IRQCOUNT] = irq_get_stats()->irq_count;

                console_printf("[XJ380/arm32] alive loop=%u led=0x%02X ticks=%u irq=%u uptime=%u ms\n",
                               loop_count, (u32)(HB[HB_IDX_LED] & 0xFFu), g_tick_seen,
                               irq_get_stats()->irq_count, (u32)(timer_read_us() / 1000u));
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
