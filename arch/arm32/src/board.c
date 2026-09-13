/*
 * 板级胶水层 —— 驱动表与诊断输出
 *
 * 设备描述表本身是生成的(arch/arm32/src/board_devices.c),
 * 本文件是**手写**的那一半:驱动表、以及"把描述表打出来"的能力。
 *
 * 为什么驱动表要单独一个聚合点,而不是让生成器去生成:
 * 驱动是**代码**,不是数据。生成器只该产出"硬件是什么",
 * 一旦开始生成"哪些驱动在场",就把两个变化频率完全不同的东西绑在了一起 ——
 * 加一个驱动要重新跑硬件描述的生成器,这显然不对。
 *
 * 所以:
 *   board_devices.c(生成)  ── 硬件是什么
 *   board.c(手写)          ── 谁去驱动它
 * 两者在 board_probe_all() 里汇合。
 */

#include <arch/board.h>
#include <arch/board_devices.h>
#include <arch/console.h>
#include <arch/plat_device.h>
#include <arch/types.h>

/* ------------------------------------------------------------------ */
/* 驱动表                                                              */
/* ------------------------------------------------------------------ */

/*
 * 具体驱动在各自的源文件里定义 plat_driver_t,在这里汇总成**指针数组**。
 *
 * 为什么是指针数组而不是结构体数组:C 不允许用结构体变量初始化结构体数组,
 * 而驱动必须能各自住在自己的翻译单元里 —— 否则每加一个驱动都要把它的
 * 实现搬进本文件。见 plat_device.h 里 plat_probe_all 的说明。
 *
 * **顺序即优先级**:同一个 compatible 有多个驱动时,排在前面的先被尝试;
 * 它 probe 返回非 0 才轮到下一个。
 */
extern const plat_driver_t axi_gpio_driver;

const plat_driver_t *const g_board_drivers[] = {
    &axi_gpio_driver,
};

const u32 g_board_driver_count = sizeof(g_board_drivers) / sizeof(g_board_drivers[0]);

/* ------------------------------------------------------------------ */
/* PL 覆盖表                                                           */
/* ------------------------------------------------------------------ */

/*
 * 生成器把 PL 段(0x40000000-0xBFFFFFFF)的节点一律标为 enabled = false,
 * 理由是:xparameters.h 只说明"**设计里**有这些 IP",不说明"**比特流已加载**"。
 * 那是运行时事实,生成器不该替它做假设。
 *
 * 这张表就是补上那个运行时事实的地方 —— 当前实际加载的比特流
 * (tmp-test/jtag/run_kernel_uart.tcl 里的 System_wrapper.bit,
 * 来自 AXI_GPIO_1 设计)里有且仅有 AXI GPIO。
 *
 * ⚠ 换比特流之后必须改这里。忘了改的症状是:PL 上的驱动静默不 probe,
 *   而描述表里那个节点看起来一切正常(只是 enabled=false)。
 *   board_dump_devices() 会把每个节点的启用状态打出来,就是为这种情况准备的。
 */
typedef struct
{
    const char *compatible;
    uintptr_t   reg_base;
} pl_override_t;

static const pl_override_t g_pl_present[] = {
    {"xlnx,axi-gpio-2.0", 0x41200000u},
};

static void board_apply_pl_overrides(void)
{
    u32 i;
    u32 j;

    for (i = 0; i < g_board_device_count; i++) {
        plat_device_t *dev = &g_board_devices[i];

        if (!dev->enabled) {
            for (j = 0; j < sizeof(g_pl_present) / sizeof(g_pl_present[0]); j++) {
                if (plat_compatible_match(g_pl_present[j].compatible, dev->compatible) &&
                    dev->reg_base == g_pl_present[j].reg_base) {
                    dev->enabled = true;
                    break;
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* 打印                                                                */
/* ------------------------------------------------------------------ */

/*
 * 列宽。compatible 取 30 是因为最长的实际值
 * "xlnx,ps7-coresight-comp-1.00.a" 正好 30 字符 —— 截断了会看起来
 * 像数据出错,而去查一个其实没问题的东西。
 * 整行 3+5+16+11+7+30 = 72 字符,9600 波特下不折行。
 */
#define NAME_COL 16
#define COMPAT_COL 30

static void print_padded(const char *text, u32 width)
{
    u32 len = 0;

    if (text == NULL) {
        text = "-";
    }

    while (text[len] != '\0' && len < width) {
        console_putc(text[len]);
        len++;
    }
    while (len < width) {
        console_putc(' ');
        len++;
    }
}

/*
 * 状态标记的两种含义要分清:
 *   "pl " —— 节点在描述表里,但 enabled = false(比特流未加载)
 *   "on " —— 节点启用,会参与驱动匹配
 * 用两个字符是刻意的:一眼能看出"这个设备被跳过了",
 * 而不是像空字符串那样让人以为打印出了 bug。
 */
static void print_state(const plat_device_t *dev)
{
    if (dev->enabled) {
        console_puts("[on ]");
    } else {
        console_puts("[pl ]");
    }
}

void board_dump_devices(void)
{
    u32 i;
    u32 active = 0;

    board_apply_pl_overrides();

    for (i = 0; i < g_board_device_count; i++) {
        if (plat_device_is_active(&g_board_devices[i])) {
            active++;
        }
    }

    console_printf(" Board devices: %u nodes (%u active)\n", g_board_device_count, active);

    for (i = 0; i < g_board_device_count; i++) {
        const plat_device_t *dev = &g_board_devices[i];

        console_puts("   ");
        print_state(dev);
        console_putc(' ');
        print_padded(dev->name, NAME_COL);
        console_printf("0x%08X ", (u32)dev->reg_base);

        if (dev->irq >= 0) {
            console_printf("irq%-3u ", (u32)dev->irq);
        } else {
            console_puts("irq-   ");
        }

        print_padded(dev->compatible, COMPAT_COL);
        console_putc('\n');
    }
}

u32 board_probe_all(void)
{
    plat_probe_stats_t st;

    board_apply_pl_overrides();

    plat_probe_all(g_board_devices, g_board_device_count, g_board_drivers, g_board_driver_count, NULL, &st);

    /*
     * 四个数一起报,而不是只报"成功几个"。
     *
     * 它们的关系 total = disabled + probed + unclaimed + failed 是排查时的
     * 第一道交叉验证:对不上说明匹配循环本身有问题,而不是驱动有问题。
     * 另外 unclaimed 与 failed 必须分开看 ——
     *   unclaimed:描述表里的设备没有任何驱动声明认识它(驱动还没写);
     *   failed   :有驱动声明认识,但全都放弃了(驱动写了但不认这个硬件)。
     * 两者的排查方向完全不同。
     */
    console_printf(" Board probe  : total=%u disabled=%u probed=%u unclaimed=%u failed=%u\n", st.total,
                   st.disabled, st.probed, st.unclaimed, st.failed);

    if (st.total != (st.disabled + st.probed + st.unclaimed + st.failed)) {
        console_puts(" Board WARN   : probe stats do not add up - matching loop is broken\n");
    }

    return st.probed;
}
