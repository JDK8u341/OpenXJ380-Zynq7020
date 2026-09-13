/*
 * 串口命令通道 —— 硬件侧
 *
 * 行编辑状态机与命令解析在 include/arch/shell.h(纯逻辑,宿主单测);
 * 本文件负责 UART 轮询、回显、命令表与具体命令。
 *
 * ====================================================================
 * 为什么必须是非阻塞的
 * ====================================================================
 *
 * shell_poll() 从主循环里调用,而主循环还负责:
 *   - 跑马灯(约 5 Hz)
 *   - PS LED 慢闪
 *   - 每秒一条状态行
 *   - fault_test_poll()
 *
 * 如果这里阻塞等一行输入,上面全部停摆 —— 而"串口没接"或"没敲回车"
 * 是完全正常的状态。所以:
 *   - 没有字符就立刻返回;
 *   - 单次最多处理 SHELL_POLL_BUDGET 个字符,防止有人把一大段文本
 *     灌进来把主循环饿死(那会表现为 LED 卡住、tick 计数变慢)。
 */

#include <arch/console.h>
#include <arch/heartbeat.h>
#include <arch/io.h>
#include <arch/led.h>
#include <arch/platform.h>
#include <arch/plat_device.h>
#include <arch/board.h>
#include <arch/board_devices.h>
#include <arch/selftest.h>
#include <arch/shell.h>
#include <arch/timer.h>
#include <arch/uart_ps.h>
#include <arch/types.h>

/* 单次 poll 最多吃多少字符。超出的留到下一次,不抢主循环的时间 */
#define SHELL_POLL_BUDGET 64u

static shell_line_t g_line;
static bool         g_initialized;

/* ------------------------------------------------------------------ */
/* 输出小工具                                                          */
/* ------------------------------------------------------------------ */

static void sh_puts(const char *s)
{
    console_puts(s);
}

static void sh_putc(char c)
{
    console_putc(c);
}

/* 按 0/1 打印十进制。console.c 的 printf 没有 %llu,所以大数要拆开打 */
static void sh_put_hex32(u32 value)
{
    console_put_hex32(value);
}

static void sh_prompt(void)
{
    sh_puts("> ");
}

/* ------------------------------------------------------------------ */
/* 命令实现                                                            */
/* ------------------------------------------------------------------ */

static void cmd_help(const char *args);

static void cmd_dump(const char *args)
{
    (void)args;
    board_dump_devices();
}

static void cmd_probe(const char *args)
{
    (void)args;
    board_dump_devices();
    (void)board_probe_all();
}

static void cmd_selftest(const char *args)
{
    (void)args;

    /*
     * 重跑自检。
     *
     * 这里只重跑那些**纯查询**性质的项(设备表、probe、中断计数),
     * 不重跑 UART 标定与 MMU 启用 —— 那两项会改变系统状态,
     * 在一个已经跑起来的系统上重做它们要么无意义(MMU 已开),
     * 要么有害(重新标定会打断串口,而命令通道正跑在它上面)。
     */
    selftest_begin();
    selftest_report("board_devices", g_board_device_count, 1u, SELFTEST_GE);
    selftest_report("probe_probed", HB[HB_SLOT_PROBED], 1u, SELFTEST_GE);
    (void)selftest_summary();
}

static void cmd_uptime(const char *args)
{
    u32 ms;

    (void)args;

    /*
     * timer_read_us() 是 64 位的,而 console.c 没有 %llu。
     * 本阶段开机时间远小于 2^32 微秒(约 71 分钟),所以取低 32 位够用 ——
     * 但要说清楚这是有意的,不是忘了。
     */
    ms = (u32)(timer_read_us() / 1000u);
    console_printf(" uptime: %u ms\n", ms);
}

static void cmd_ver(const char *args)
{
    (void)args;

    console_printf(" OpenXJ380 / ARMv7-A (Zynq-7020)\n");
    console_printf(" CPU       : %u Hz\n", PLAT_CPU_FREQ_HZ);
    console_printf(" Global tmr: %u Hz\n", PLAT_GLOBAL_TIMER_FREQ_HZ);
    console_printf(" UART base : 0x%08X @ %u baud\n", PLAT_CONSOLE_UART_BASE, PLAT_CONSOLE_BAUD);
    console_printf(" Switches  : 0x%02X\n", sw_read());
}

/*
 * peek <hex>
 *
 * 读一个 32 位字。**会 Data Abort 如果地址没映射** —— 这是有意的:
 * 页表把未列出的区域填成 fault(见 mmu.c),所以读哪儿是"有从设备"
 * 还是"没有"会被立刻区分出来。代价是内核会停机,
 * 所以帮助里必须写明。
 */
static void cmd_peek(const char *args)
{
    u32 addr = 0;
    u32 i = 0;
    u32 value;

    if (args == NULL || args[0] == '\0') {
        sh_puts(" usage: peek <hex-addr>   (警告:未映射地址会导致 Data Abort 停机)\n");
        return;
    }

    /* 手写十六进制解析:console.c 的 printf 是单向的,没有对应的 scanf */
    while (args[i] != '\0') {
        char c = args[i];
        u32  digit;

        if (c >= '0' && c <= '9') {
            digit = (u32)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = (u32)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            digit = (u32)(c - 'A' + 10);
        } else if (c == 'x' || c == 'X') {
            /* 允许 0x 前缀,跳过 */
            i++;
            continue;
        } else {
            sh_puts(" 不是合法的十六进制地址\n");
            return;
        }

        addr = (addr << 4) | digit;
        i++;
    }

    console_printf(" peek 0x%08X = ", addr);
    value = mmio_read32(addr);
    sh_put_hex32(value);
    console_printf("  (u32 %u)\n", value);
}

/* ------------------------------------------------------------------ */
/* 命令表                                                              */
/* ------------------------------------------------------------------ */

/*
 * 顺序即 help 的打印顺序。
 * 帮助文本写全:调试通道的价值全在"不用查文档就知道能干什么"。
 */
static const shell_cmd_t g_commands[] = {
    {"help", "列出全部命令", cmd_help},
    {"ver", "打印版本与板级信息", cmd_ver},
    {"uptime", "开机时长(毫秒)", cmd_uptime},
    {"dump", "打印设备描述表(全部节点与状态)", cmd_dump},
    {"probe", "重跑驱动匹配并打印统计", cmd_probe},
    {"selftest", "重跑可重复的那几项自检", cmd_selftest},
    {"peek", "peek <hex-addr>: 读 32 位;未映射地址会 Data Abort", cmd_peek},
};

static void cmd_help(const char *args)
{
    u32 i;

    (void)args;

    sh_puts(" commands:\n");
    for (i = 0; i < sizeof(g_commands) / sizeof(g_commands[0]); i++) {
        console_printf("   %s\n", g_commands[i].name);
        console_printf("       %s\n", g_commands[i].help);
    }
}

/* ------------------------------------------------------------------ */
/* 分发                                                                */
/* ------------------------------------------------------------------ */

static void shell_execute(void)
{
    const char        *name;
    const char        *args;
    const shell_cmd_t *cmd;

    /*
     * ⚠ 溢出过的行必须拒绝,不能执行截断版。
     *
     * 一条被截断的长命令,其前缀很可能恰好是另一条**合法**命令 ——
     * 于是执行了完全不同的操作,而且不报任何错。
     * 这是本通道唯一一处"宁可拒绝也不猜"的地方。
     */
    if (g_line.overflow) {
        sh_puts(" line too long (max 63 chars) - command ignored\n");
        return;
    }

    args = shell_split(g_line.buf, &name);

    if (name[0] == '\0') {
        return; /* 空行:只是又敲了一次回车 */
    }

    cmd = shell_lookup(g_commands, sizeof(g_commands) / sizeof(g_commands[0]), name);
    if (cmd == NULL) {
        console_printf(" unknown command: %s   (try 'help')\n", name);
        return;
    }

    cmd->run(args);
}

/* ------------------------------------------------------------------ */
/* 轮询                                                                */
/* ------------------------------------------------------------------ */

void shell_init(void)
{
    shell_line_reset(&g_line);
    g_initialized = true;
}

void shell_poll(void)
{
    u32 budget = SHELL_POLL_BUDGET;

    if (!g_initialized) {
        return;
    }

    /*
     * 预算限制不是可有可无的:如果对端持续灌数据(比如误把文件当输入),
     * 无上限的循环会把主循环整个饿死 —— 表现为 LED 卡住、tick 变慢,
     * 而"串口在收数据"这件事本身看起来完全正常。
     */
    while (budget > 0u && uart_rx_ready(PLAT_CONSOLE_UART_BASE)) {
        char         c = uart_getc(PLAT_CONSOLE_UART_BASE);
        shell_act_t  act = shell_line_feed(&g_line, c);

        budget--;

        switch (act) {
        case SHELL_ACT_ECHO:
            sh_putc(c);
            break;

        case SHELL_ACT_ERASE:
            /* 退格在终端上的标准写法:退一格、盖一个空格、再退一格 */
            sh_puts("\b \b");
            break;

        case SHELL_ACT_CANCEL:
            console_excl_begin();
            sh_puts("^C\n");
            sh_prompt();
            console_excl_end();
            shell_line_reset(&g_line);
            break;

        case SHELL_ACT_SUBMIT:
            /*
             * ★ 整段输出排他(M4-11.2 收尾时补上)★
             *
             * 一条命令的回显是好几十行(`ver` 5 行、`dump` 二十几行),
             * 而状态行每秒都会打一行 —— 实测它正好劈在
             * `Switches  : 0x..` 的中间:
             *
             *     Switches[XJ380/arm32] alive loop=4 led=0x02 …
             *       : 0x00
             *
             * 于是 `tmp-test/shell_test.py` 的 `ver` 用例假失败(它找的是
             * `"Switches  : 0x"`)。⚠ 这是 M4-11.1 的直接后果:排他从
             * "关调度"换成真锁之后,状态线程不再被冻住,交错的机会变多了。
             *
             * 排他包**整段**:"半行被劈开"对人也是废的,而对机器判据是致命的。
             * 提示符也包在里面 —— 它前面那个换行与 `> ` 必须连在一起
             * (`shell_test.py` 等的片段就是 `"\n> "`)。
             *
             * ⚠ 锁本身是递归的,命令内部再排他(例如 `selftest` 打报告)不会自锁。
             */
            console_excl_begin();
            sh_puts("\r\n");
            shell_execute();
            shell_line_reset(&g_line);
            sh_prompt();
            console_excl_end();
            break;

        case SHELL_ACT_IGNORED:
        default:
            break;
        }
    }
}

void shell_banner(void)
{
    /* ⚠ 提示符与它前面那句必须连在一起 —— 与 SUBMIT 那一段同一个理由 */
    console_excl_begin();
    sh_puts(" Serial command channel ready. Type 'help'.\n");
    sh_prompt();
    console_excl_end();
}
