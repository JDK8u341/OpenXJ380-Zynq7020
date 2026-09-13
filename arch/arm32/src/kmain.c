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
#include <arch/axi_gpio.h>
#include <arch/board.h>
#include <arch/board_devices.h>
#include <arch/console.h>
#include <arch/cpu.h>
#include <arch/fault_test.h>
#include <arch/heartbeat.h>
#include <arch/io.h>
#include <arch/irq.h>
#include <arch/led.h>
#include <arch/mmu.h>
#include <arch/palloc.h>
#include <arch/percpu.h>
#include <arch/platform.h>
#include <arch/selftest.h>
#include <arch/shell.h>
#include <arch/smp.h>
#include <arch/timer.h>
#include <arch/types.h>
#include <arch/uart_ps.h>

/*
 * CPU0 栈区顶部的链接符号(boot/kernel.ld)。
 * start.S 用同一个符号建 CPU0 的栈;这里读它是为了在 percpu 表里
 * 留下一条可核对的记录 —— 两个核的栈区必须不同。
 */
extern char __stack_top[];

/*
 * 等 CPU1 上线的上限。
 *
 * 取 200ms:CPU1 要做的事(TPIDRPRW、MMU、SCU、L1)在 666MHz 上是
 * 微秒级的,这个值宽松了三个数量级。超过就不该再等 ——
 * "无限等"会把一个可诊断的降级变成整机挂死,本项目在 UART 轮询上
 * 已经踩过一次同样的坑。
 */
#define CPU1_BOOT_TIMEOUT_US 200000u

/*
 * 物理页分配器实例与其 bitmap(M4-2)。
 *
 * 放在这里而不是模块内部:bitmap 是 64KB,必须是**静态分配**的 ——
 * 分配器不能给自己分配存储,那是先有鸡还是先有蛋。
 * 64KB 在 .bss 里,落在内核镜像之内,所以不会被池自己发出去。
 */
u32      g_palloc_bitmap[PALLOC_BITMAP_WORDS];
palloc_t g_palloc;

/* 物理页分配器的自检与冒烟结果,供自检报告使用 */
static u32 g_palloc_selftest;
static u32 g_palloc_smoke;

/* SMP 压力测试的结果,供自检报告使用 */
static smp_stress_result_t g_smp_stress;

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
    u32       now;
    percpu_t *pc;

    (void)intid;
    (void)arg;

    /*
     * 必须先清定时器的中断标志再返回。
     * 私有定时器是电平式输出,不清标志的话 GIC 会立刻再报一次,
     * 表现为"进了中断就再也出不来",而且从寄存器上看一切正常。
     *
     * 这一步同时也把 tick 记到**本核**的 percpu 上(a9_timer_clear_irq 内部做)。
     */
    a9_timer_clear_irq();

    /*
     * ⚠⚠ 下面这些全局量(g_tick_seen / g_tick_last_gt / g_tick_max_gap)
     *    是 **CPU0 的诊断状态**,而这个中断处理函数是两核共用的 ——
     *    CPU1 的 tick 也会走到这里。不隔离的话 CPU1 会把 CPU0 的计数
     *    覆盖掉,而症状是"ticks 和 irq_count 对不上"这种看起来像
     *    丢中断的现象(实测踩到,自检里的 irq_ticks_eq_irq 直接报 FAIL)。
     *
     *    本核自己的 tick 计数已经在 percpu 里,不受影响。
     */
    pc = percpu_self();
    if (pc != NULL && pc->cpu_id != 0u) {
        return;
    }

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
    bool     uart_loopback_ok = false;

    /* static: 由 BSS 自动清零。这样在"无串口"路径下打印诊断也不会读未初始化值 */
    static uart_baud_result_t baud_result;

    /* ---- 1. 先把板级反馈弄起来 ---- */
    /*
     * 串口依赖未知的参考时钟,而 LED 只依赖已经验证过的 GPIO 通路。
     * 所以先做 LED:即使后面串口标定失败,板上也一定有可见反馈。
     *
     * ⚠ M3 之后 led_init() **只初始化 PS 侧的 MIO7/MIO8**。
     *   PL 侧的 AXI GPIO 归设备描述层管 —— 它的方向寄存器要按描述
     *   给出的位宽来配,而那要等 board_probe_all() 跑完。
     *   这个拆分让 led_init() 能保持"足够早",不依赖任何发现机制。
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

    /* PS LED 自检。PL LED 的自检在描述层跑完之后(见第 4 步) */
    led_ps_set(true);
    timer_delay_ms(150);
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

        /*
         * ---- 收发通路自检(必须在这里做,不能挪到自检报告那一节)----
         *
         * 用它把"驱动/寄存器有问题"与"外部线缆有问题"分开:
         * 内部环回把发送端在芯片内直接接到接收端,不经过外部引脚。
         *
         * ⚠ 位置是有讲究的,放在这里有两个硬理由:
         *
         * (a) 它会临时把 MR 切成环回再切回来。放在报告打印中途做,
         *     会把已经排队但还没发出去的输出冲掉(实测:整条
         *     `CHECK uart_baud_ppm` 消失、上一行只剩半行)。
         * (b) 本地环回模式下 TX 引脚**仍然在输出**,探针字节会漏到线上。
         *     放在横幅之前,漏出来的那个字符落在报告区间之外,
         *     不会把 `CHECK` 行首污染成 `UCHECK` 而让解析脚本漏读一项。
         *
         * 结果缓存下来给后面的自检报告用。
         */
        uart_loopback_ok = uart_loopback_selftest(PLAT_CONSOLE_UART_BASE);
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
    console_puts(" M3 - device description layer + driver probe\n");
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

    /* ---- 5. 设备描述层 ---- */
    /*
     * 位置:在控制台之后、中断与 MMU 之前。
     *
     *   - 必须在控制台之后:probe 的结果要能打出来。**没有枚举的 ARM 上,
     *     "打印全部节点"是唯一能在驱动没起来时区分"驱动写错了"和
     *     "描述表里根本没有这个节点"的手段**;
     *   - 必须在中断之前:驱动的 probe 目前都是纯 MMIO 轮询,
     *     不需要中断;放前面可以让"中断没配好"与"驱动没 probe 上"
     *     两类问题互不干扰;
     *   - 必须在 MMU 之前:probe 里访问的都是物理地址。
     *
     * 顺序本身也是有意义的:先 dump 再 probe,所以日志里能同时看到
     * "描述了哪些设备"和"哪些被认领了",而不是只看到结果。
     */
    console_puts(" Device description layer\n");
    board_dump_devices();
    HB[HB_SLOT_PROBED] = board_probe_all();
    console_puts("\n");

    /*
     * PL LED 自检。
     *
     * **挪到这里是有原因的**:PL 侧的 AXI GPIO 现在归描述层管,
     * 它的方向寄存器由驱动在 probe 时按描述里给出的位宽配置。
     * 在此之前往 ch2 写数据是无效的 —— 通道还是输入。
     * 这也正是 M3 想验证的事情之一:自检能跑,就说明
     * "描述表 -> 匹配 -> probe -> 驱动配置硬件"这条链路真的通了。
     */
    if (axi_gpio_ready()) {
        /*
         * 自检分两步,而且**第二步才是真正的验证**。
         *
         * 第一步(全亮再全灭)只是把肉眼可见的反馈做出来,能说明的信息
         * 很有限 —— 即使驱动把值写到了错误的地址,这一步也照样"跑完"了,
         * 只是板上什么都不亮。
         *
         * 第二步做写回读:把几个固定图案写进 LED 通道再读回来比对。
         * AXI GPIO 的输出通道 DATA 寄存器是可读的,所以这条链路
         * (驱动基址 -> 寄存器 -> 读回)能被直接观测。
         *
         * 这是唯一能区分"代码跑了"与"硬件真的动了"的手段。本项目在
         * SCU/ACTLR 上就因为少了这类观测而误判过一次:缓存使能位读回是 1,
         * 系统也照常跑,但实际上完全没有加速。
         */
        static const u8 patterns[] = {0x00u, 0xFFu, 0xA5u, 0x5Au, 0x01u, 0x80u};
        u32             i;
        u32             mismatches = 0;

        console_printf(" LED self-test: AXI GPIO at 0x%08X, %u-bit, dual-channel\n",
                       (u32)axi_gpio_get_base(), axi_gpio_get_width());

        led_pl_set(0xFFu);
        timer_delay_ms(150);
        led_pl_set(0x00u);

        for (i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++) {
            u8 readback;

            led_pl_set(patterns[i]);
            readback = axi_gpio_led_read();

            if (readback != patterns[i]) {
                mismatches++;
                console_printf("   write/readback mismatch: wrote 0x%02X, read 0x%02X\n", patterns[i],
                               readback);
            }
        }

        console_printf(" LED check    : write/readback %s (%u patterns)\n",
                       (mismatches == 0u) ? "PASS" : "FAIL",
                       (u32)(sizeof(patterns) / sizeof(patterns[0])));
        console_printf(" LED switches : ch1 = 0x%02X\n", axi_gpio_switch_read());

        HB[HB_SLOT_LEDCHECK] = mismatches;

        /* 留给主循环一个干净的起点 */
        led_pl_set(0x00u);
    } else {
        console_puts(" LED WARN     : AXI GPIO was not claimed - PL LEDs unavailable\n");
        HB[HB_SLOT_LEDCHECK] = 0xFFFFFFFFu;
    }
    console_puts("\n");

    /* ---- 6. 中断子系统(GIC + 周期 tick) ---- */
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

    /* ---- 7. MMU ---- */
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

    /* ---- 8. 缓存几何 ---- */
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

    /* ---- 9. 使能 L1 缓存 ---- */
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

    /*
     * L2(PL310)。
     *
     * 顺序与 Xilinx boot.S 一致:L1 先开,再初始化 L2。
     * 它不在 CLIDR 里,所以上面那套 CP15 几何循环完全覆盖不到它 ——
     * 必须走它自己的寄存器(0xF8F02000),配置也完全是另一套
     * (延迟参数、SLCR 里的 RAM 配置)。
     */
    {
        u32 l2_id = l2_cache_id();

        console_puts(" Cache: configuring PL310 L2...\n");
        timer_delay_ms(20);

        l2_cache_init();

        console_printf(" Cache L2    : %s  ID=0x%08X TYPE=0x%08X CTRL=0x%08X\n",
                       l2_cache_is_enabled() ? "ON" : "off", l2_id, l2_cache_type(), l2_cache_control());

        /*
         * L2 有效性实验 —— 受控 A/B。
         *
         * 为什么必须单独做这一步:上面那行只证明"使能位被置上了",
         * 而**不能证明 L2 真的在缓存任何东西**。本项目在 L1 上就吃过
         * 这个亏(SCU/ACTLR 没设,C 位读回是 1 但缓存完全无效),
         * 所以 L2 不能只靠寄存器交差。
         *
         * 工作集取 128KB:远超 32KB 的 L1,又远小于 512KB 的 L2。
         * 于是在 L2 真的工作时访问基本命中;L2 一关就退化成每次去 DDR,
         * 耗时差一个数量级,不可能看不出来。
         */
        {
            const volatile u32 *buf = cache_l2_bench_buf();
            u32                 words = cache_l2_bench_words();
            u32                 on_us;
            u32                 off_us;
            u32                 reon_us;

            /* 三遍:开 -> 关 -> 再开。中间那次是唯一的变量 */
            on_us = cache_bench_us_on(buf, words, 16u);

            l2_cache_disable();
            off_us = cache_bench_us_on(buf, words, 16u);
            l2_cache_enable();

            reon_us = cache_bench_us_on(buf, words, 16u);

            console_printf(" L2 bench    : 128KB working set  on=%u us  off=%u us  on-again=%u us\n",
                           on_us, off_us, reon_us);

            /*
             * 判据来自**实测**,不是估计。
             *
             * 本板实测:on=11211us  off=15987us  on-again=11214us
             * 也就是 L2 带来约 1.43 倍,而不是我最初想当然的"一个数量级" ——
             * 顺序访问会被 PL310 的预取掩盖掉相当一部分延迟,纯读循环
             * 又比真实负载更友好。第一版判据按 2 倍写,于是误报了 WARN。
             *
             * 真正能证明"L2 在缓存"的是这两条:
             *   1. 关掉之后明显更慢(不是噪声);
             *   2. 重新打开能精确回到原来的水平。
             * 第 2 条尤其关键 —— 它同时验证了 disable/enable 没有副作用
             * (直接清使能位会丢掉脏行,是最容易出错的地方),
             * 而加速比的**绝对大小**反而不是判据:它取决于工作集、
             * 访问模式与预取行为,拿来当阈值只会误导。
             */
            if (off_us > (on_us + (on_us / 5u)) && reon_us < (off_us - (off_us / 5u))) {
                console_printf(" L2 check    : effective (off/on = %u.%02ux)\n", off_us / (on_us ? on_us : 1u),
                               ((off_us * 100u) / (on_us ? on_us : 1u)) % 100u);
            } else {
                console_puts(" L2 WARN     : no measurable benefit - L2 may not be caching\n");
            }

            /*
             * 心跳槽里存**百分比**,不是比值。
             *
             * 实测比值是 1.42,取整后是 1 —— 而"1"这个数字写不出任何
             * 有意义的判据:自检里若写 ">= 1" 就永远成立,等于没判。
             * 存成 142 才能写出 ">= 120" 这种真正会失败的阈值。
             *
             * 这个不一致是被 tmp-test/verify_board.py 第一次跑就抓出来的:
             * 判据写 ">= 100" 而槽里存 1,直接报 FAIL。
             * 在此之前它不会以任何形式表现出来 —— 打印出来的
             * "effective (off/on = 1.42x)" 看着完全正常。
             */
            HB[HB_SLOT_L2BENCH] = (on_us > 0u) ? ((off_us * 100u) / on_us) : 0u;
        }
    }

    /*
     * 缓存维护自检。
     *
     * 这一步验证的是**DMA 路径所依赖的语义**,而不是缓存快不快:
     * clean 是否真的把数据写回了内存、invalidate 是否真的丢弃了缓存副本。
     * 两者任一不成立,DMA 就会出现"偶尔错几个字节"这类最难查的问题。
     *
     * 不需要任何外设参与 —— 靠的是"缓存与内存是两份副本"这个事实。
     * L2 使能后,这一套操作的语义要在**两级缓存**上都成立,
     * 而 cache_*_range() 正好同时维护 L1 与 L2,所以这个自检
     * 同时也是对两级联动的验证。
     */
    {
        u32 selftest = cache_selftest();

        console_printf(" Cache maint : line=%u B  clean/invalidate selftest=%s\n", cache_line_bytes(),
                       (selftest == 0u) ? "PASS" : "FAIL");
        if (selftest != 0u) {
            console_printf("               failed at check %u (see cache_hw.c)\n", selftest);
        }
        HB[HB_SLOT_CACHESELFTEST] = selftest;
    }
    console_puts("\n");

    /* ---- 9.4 物理页分配器(M4-2) ---- */
    /*
     * 池的范围 = [_kernel_end 向上对齐, DDR 末尾)。
     *
     * 为什么从 _kernel_end 开始而不是从 DDR 基址:内核镜像、页表、两个核的
     * 栈、L2 基准缓冲区全都在镜像之内(链接脚本定义的符号),所以镜像之后
     * 的才是真正没主的物理内存。**不需要**再逐个 palloc_reserve ——
     * 那些区域根本不在池范围内,reserve 它们只会是空操作。
     *
     * ⚠ 但这个"不需要"依赖一件事:**任何将来新增的静态占用都必须落在
     *   镜像之内**(即通过链接脚本而不是运行时分配)。将来若有人把某个
     *   大缓冲区放在镜像之外的固定物理地址上,必须在这里补一条 reserve。
     */
    {
        extern char         _kernel_end[];
        extern u32          g_palloc_bitmap[PALLOC_BITMAP_WORDS];
        extern palloc_t     g_palloc;

        uintptr_t   pool_base;
        size_t      pool_size;
        palloc_err_t e;

        pool_base = ((uintptr_t)_kernel_end + PALLOC_PAGE_SIZE - 1u) & ~(uintptr_t)(PALLOC_PAGE_SIZE - 1u);
        pool_size = (size_t)(PLAT_DDR_END - pool_base);
        pool_size &= ~(size_t)(PALLOC_PAGE_SIZE - 1u);

        e = palloc_init(&g_palloc, pool_base, pool_size, g_palloc_bitmap, PALLOC_BITMAP_WORDS);

        if (e == PALLOC_OK) {
            console_printf(" Page alloc  : %u pages (%u MB) at 0x%08X\n",
                           g_palloc.page_count,
                           (u32)(((uintptr_t)g_palloc.page_count * PALLOC_PAGE_SIZE) >> 20), (u32)pool_base);
        } else {
            console_printf(" Page alloc  : FAILED err=%u\n", (u32)e);
        }

        g_palloc_selftest = palloc_selftest();

        /*
         * 真实内存冒烟:分配一页、写一个图案、读回来、释放。
         *
         * 自检跑的是**合成实例**(基址 0x10000000,在宿主上也能跑),
         * 它证明分配器的逻辑对,但证明不了"这段物理内存真的能用"。
         * 这一步才是对真实 RAM 的读写 —— 与缓存那节"写回读"同一个道理。
         */
        g_palloc_smoke = 0u;
        if (e == PALLOC_OK) {
            uintptr_t page = 0;

            if (palloc_alloc(&g_palloc, &page) == PALLOC_OK) {
                volatile u32 *w = (volatile u32 *)page;
                u32           i;
                u32           ok = 1u;

                for (i = 0; i < (PALLOC_PAGE_SIZE / 4u); i++) {
                    w[i] = 0xA5A50000u ^ i;
                }
                for (i = 0; i < (PALLOC_PAGE_SIZE / 4u); i++) {
                    if (w[i] != (0xA5A50000u ^ i)) {
                        ok = 0u;
                        break;
                    }
                }

                if (palloc_free(&g_palloc, page) != PALLOC_OK) {
                    ok = 0u;
                }

                g_palloc_smoke = ok;
                console_printf(" Page smoke  : alloc/write/readback/free at 0x%08X = %s\n", (u32)page,
                               ok ? "PASS" : "FAIL");
            }
        }
    }

    /* ---- 9.5 第二个核(AM3-1/2/3) ---- */
    /*
     * 位置:MMU 与缓存都已就绪之后。
     *
     * 为什么必须在这个位置:CPU1 要复用的正是 CPU0 刚建好的那份页表
     * (mmu_enable_secondary() 只写它自己的 TTBR0,不重建表)。
     * 放早了读到的是空表,放晚了无非多等一会儿。
     *
     * 顺序也是硬的:先 per-CPU 表就绪 -> 再放 CPU1 -> 再等它报到。
     * 反过来的话 CPU1 会在 percpu 表还没准备好时就去读自己的结构体。
     */
    {
        bool cpu1_ok;

        percpu_table_reset();

        /*
         * CPU0 自己的表项。传 __stack_top 让记录完整 ——
         * 这条信息之后会被打印出来,用于确认两个核的栈区确实不同。
         */
        /*
         * CPU0 这里可以直接 publish:它的缓存早就开了(见上面缓存那一节),
         * 所以这次写入走 SCU 一致性路径,CPU1 起来后看到的是干净的值。
         */
        if (percpu_init_self((uintptr_t)__stack_top) != NULL) {
            percpu_publish_self();
        }

        HB[HB_SLOT_CPU1_STAGE]  = HB_CPU1_STAGE_IDLE;
        HB[HB_SLOT_CPU1_ONLINE] = 0u;

        console_puts(" SMP: releasing CPU1 (write 0xFFFFFFF0 + SEV)...\n");

        smp_release_cpu1();
        cpu1_ok = smp_wait_online(1u, CPU1_BOOT_TIMEOUT_US);

        if (cpu1_ok) {
            console_printf(" SMP         : CPU1 online  id=%u mpidr=0x%08X stack=0x%08X\n",
                           g_percpu[1].cpu_id, g_percpu[1].mpidr, (u32)g_percpu[1].stack_top);
        } else {
            /*
             * 不等成功也要如实报出来,而且**不能就此停机** ——
             * 单核状态下其它功能都是好的,把整机拦在这里并不能多查出什么。
             * 心跳里的 stage 会告诉 JTAG 它停在哪一步。
             */
            console_printf(" SMP WARN    : CPU1 did not come online in %u us (stage=%u)\n",
                           CPU1_BOOT_TIMEOUT_US, HB[HB_SLOT_CPU1_STAGE]);
        }

        HB[HB_SLOT_CPU1_ONLINE] = cpu1_ok ? 1u : 0u;
        HB[HB_SLOT_CPU1_LOOPS]  = g_percpu[1].loops;

        /*
         * IPI 的处理函数注册在**共享**的中断表里,所以只需注册一次;
         * 但 SGI 的使能位是按核银行化的,CPU0 要自己开一次。
         * (CPU1 在 cpu1_main 里开它自己那份。)
         */
        if (smp_register_ipi() != 0) {
            console_puts(" SMP WARN    : SGI 0 已被占用,IPI 未启用\n");
        }
        smp_enable_ipi_this_cpu();

        if (cpu1_ok) {
            smp_stress_result_t r = smp_stress_run();

            console_printf(" SMP stress  : lock counter=%u/%u violations=%u cpu1_ran=%u\n", r.counter,
                           2u * SMP_STRESS_ROUNDS, r.violations, r.cpu1_ran);
            console_printf(" SMP IPI     : sent=%u seen=%u\n", r.ipi_sent, r.ipi_seen);

            g_smp_stress = r;
        }
    }

    /* ---- 10. 启动自检总账 ---- */
    /*
     * 位置:所有自检都跑完之后、主循环之前。
     *
     * 在此之前,"上板验证"靠人读日志判断"看起来没问题"——
     * 既不可自动化(改一行就要重看一遍),判据也模糊
     * ("speedup=6x 算不算通过"没有写在任何地方)。
     * 这一段把判据写成代码并以固定格式回传,由 tmp-test/verify_board.py
     * 解析并给出退出码。人只需要看最后那行 SUMMARY。
     *
     * 注意这里**只报告、不停机**:自检的意义是给出信息,
     * 而不是把一个本来能跑的系统拦在启动阶段。
     * 失败项数会写进心跳,挂死时用 JTAG 也能读到结论。
     */
    selftest_begin();

    selftest_report("uart_present", uart_present ? 1u : 0u, 1u, SELFTEST_EQ);
    selftest_report("uart_clock_source", clock_source, 1u, SELFTEST_EQ);
    selftest_report("uart_baud_ppm", baud_result.error_ppm, 50u, SELFTEST_LE);
    /*
     * 收发通路自检的结果在串口初始化之后就已经拿到(见前面的说明):
     * 它必须在没有任何待发输出、且报告区间之外的时刻执行。
     */
    selftest_report("uart_loopback", uart_loopback_ok ? 1u : 0u, 1u, SELFTEST_EQ);

    selftest_report("mmu_stage", HB[HB_SLOT_MMUSTAGE], HB_MMU_STAGE_ON, SELFTEST_EQ);
    selftest_report("mmu_enabled", mmu_is_enabled() ? 1u : 0u, 1u, SELFTEST_EQ);

    selftest_report("cache_dcache_on", cache_dcache_enabled() ? 1u : 0u, 1u, SELFTEST_EQ);
    selftest_report("cache_icache_on", cache_icache_enabled() ? 1u : 0u, 1u, SELFTEST_EQ);
    selftest_report("cache_speedup", HB[HB_SLOT_CACHEBENCH], 2u, SELFTEST_GE);
    selftest_report("cache_maint_fail", HB[HB_SLOT_CACHESELFTEST], 0u, SELFTEST_EQ);
    selftest_report("l2_enabled", l2_cache_is_enabled() ? 1u : 0u, 1u, SELFTEST_EQ);
    selftest_report("l2_effect_pct", HB[HB_SLOT_L2BENCH], 120u, SELFTEST_GE);

    /*
     * 设备描述层。
     *
     * 判据是"**至少有一个设备被认领**",而不是硬编码的设备总数 ——
     * 总数会随 XSA 变,写死它等于把一次硬件改动变成一次测试失败,
     * 而那种失败没有任何信息量。
     */
    selftest_report("board_devices", g_board_device_count, 1u, SELFTEST_GE);
    selftest_report("probe_probed", HB[HB_SLOT_PROBED], 1u, SELFTEST_GE);
    selftest_report("led_writeback_fail", HB[HB_SLOT_LEDCHECK], 0u, SELFTEST_EQ);

    /*
     * 中断子系统。
     *
     * ticks 与 irq_count 必须相等:前者是处理函数里自增的,
     * 后者是 GIC 实际转发次数。两者不符说明有中断被吞或未被 EOI,
     * 而那种情况从单个计数器上看一切正常。
     */
    selftest_report("irq_ticks_eq_irq", (g_tick_seen == irq_get_stats()->irq_count) ? 1u : 0u, 1u,
                    SELFTEST_EQ);
    selftest_report("irq_spurious", irq_get_stats()->spurious_count, 0u, SELFTEST_EQ);

    /*
     * ---- 第二个核(AM3) ----
     *
     * 这几项刻意分开报,而不是合成一个 "smp_ok":
     * CPU1 起不来有若干种截然不同的原因(没被唤醒 / 卡在 MMU / 卡在缓存 /
     * 起来了但没置 online),而心跳里的 stage 槽正好区分它们。
     * 合成一项会把这条线索丢掉。
     */
    selftest_report("palloc_selftest", g_palloc_selftest, 0u, SELFTEST_EQ);
    selftest_report("palloc_smoke", g_palloc_smoke, 1u, SELFTEST_EQ);
    /* 池至少要有 100000 页(约 390MB)—— 数字写小了等于没判 */
    selftest_report("palloc_pages_ok", (g_palloc.page_count >= 100000u) ? 1u : 0u, 1u, SELFTEST_EQ);

    selftest_report("smp_cpu1_online", g_percpu[1].online, 1u, SELFTEST_EQ);
    selftest_report("smp_cpu1_stage", HB[HB_SLOT_CPU1_STAGE], HB_CPU1_STAGE_ONLINE, SELFTEST_EQ);

    /*
     * 每核 1kHz tick(AM3-5)。
     *
     * 判据是"两核各自在推进",而不是"两核计数相等" ——
     * 相等的判据会在 CPU1 的 tick 恰好停下时也成立(两边都冻住)。
     * 这里发两次采样、要求 CPU1 的计数确实增长了。
     */
    {
        u32 t0 = g_percpu[1].ticks;

        timer_delay_us(20000u); /* 20ms -> 1kHz 下应当涨约 20 */

        selftest_report("smp_cpu1_ticks_advance", (g_percpu[1].ticks > t0) ? 1u : 0u, 1u, SELFTEST_EQ);
    }

    /*
     * IPI 判据是 sent == seen,不是 ">= 某个数"。
     * 每个 IPI 都是等目标核处理完才发下一个,所以漏掉任何一个都是真问题。
     */
    selftest_report("smp_ipi_received", g_smp_stress.ipi_seen, g_smp_stress.ipi_sent, SELFTEST_EQ);
    selftest_report("smp_lock_violations", g_smp_stress.violations, 0u, SELFTEST_EQ);

    /*
     * 计数必须正好是两倍轮数。
     * 少了说明丢了更新(锁没起作用),多了说明有核重复计数 ——
     * 两种都是真问题,所以用等号而不是"大于等于"。
     */
    selftest_report("smp_lock_counter", g_smp_stress.counter, 2u * SMP_STRESS_ROUNDS, SELFTEST_EQ);
    HB[HB_SLOT_CPU1_TICKS]    = g_percpu[1].ticks;
    HB[HB_SLOT_IPI_COUNT]     = g_percpu[1].ipi_count;
    HB[HB_SLOT_SMP_VIOLATION] = g_smp_stress.violations;
    /*
     * ⚠ 这里查 MPIDR,不查 cpu_id。
     *
     * 破坏性 A/B 抓出来的:cpu_id 由 CPU0 在放 CPU1 起来**之前**就填好了,
     * 所以即使 CPU1 根本没被唤醒它也是 1 —— 这一项永远不会失败,
     * 等于没判。MPIDR 只有 CPU1 自己能写进去,才是它真的跑过的证据。
     *
     * 0x80000001:Cortex-A9 的 MPIDR,bit31=0b1 表示多核系统,
     * bits[1:0] = 核号,所以 CPU1 就是 0x80000001。
     */
    selftest_report("smp_cpu1_mpidr", g_percpu[1].mpidr, 0x80000001u, SELFTEST_EQ);

    /*
     * loops >= 1 而不是 "> 0 就通过":判据写成 1/0 是为了让它和别的项
     * 一样是等值判定,避免"永远成立"的阈值(那种判据等于没判)。
     */
    selftest_report("smp_cpu1_loops", (g_percpu[1].loops >= 1u) ? 1u : 0u, 1u, SELFTEST_EQ);

    /*
     * 两个核的栈区必须不同。
     *
     * 这一项防的是"链接脚本改错了但两个核都能跑"的情况 ——
     * 共享栈区不会立刻崩,只会在负载上来之后随机踩栈,
     * 那时再回头怀疑到栈上要花很久。
     */
    selftest_report("smp_distinct_stacks",
                    (g_percpu[0].stack_top != 0u && g_percpu[1].stack_top != 0u &&
                     g_percpu[0].stack_top != g_percpu[1].stack_top)
                        ? 1u
                        : 0u,
                    1u, SELFTEST_EQ);

    HB[HB_SLOT_SELFTEST_FAILED] = selftest_summary();
    console_puts("\n");
    /* 自检之后才开命令通道:在此之前串口还在标定,回显会乱 */
    shell_init();
    shell_banner();

    /* ---- 11. 主循环 ---- */
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

            /*
             * 第二个核的计数同步到心跳。
             *
             * ⚠ 这里只是**搬运** CPU1 自己维护的计数,
             *   不是代它生成。loops 由 CPU1 的主循环自增,
             *   所以这个值在变化本身就证明 CPU1 真的在独立推进 ——
             *   如果 CPU0 代写,数字照样会变,但那什么也证明不了。
             */
            HB[HB_SLOT_CPU1_LOOPS]  = g_percpu[1].loops;
            HB[HB_SLOT_CPU1_ONLINE] = g_percpu[1].online;
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

        /*
         * 串口命令通道。**非阻塞** —— 没有输入就立刻返回,
         * 所以跑马灯与周期状态行完全不受影响。
         *
         * 放在主循环末尾是有意的:命令可能很慢(比如 dump 要打十几行),
         * 放前面会让这一轮的 LED 更新被推迟。放末尾则最坏情况只是
         * 下一轮稍微晚一点,节奏仍然由全局定时器决定。
         */
        shell_poll();
    }

    /* 不会到这里 */
    fail_stop();
}
