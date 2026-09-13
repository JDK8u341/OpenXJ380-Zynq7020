/*
 * 最小控制台实现
 *
 * 设计取舍:
 *   - 不引入 va_list 之外的依赖,保持 freestanding
 *   - 不做输出缓冲:内核早期阶段需要"写一条就出去一条",
 *     否则崩溃时会丢掉最后几条最有价值的日志
 *   - 不处理并发:启动阶段只有 BSP 在跑
 */

#include <arch/console.h>
#include <arch/uart_ps.h>

static uintptr_t g_uart_base = 0;

void console_init(uintptr_t uart_base, u32 uart_clk, u32 baud)
{
    g_uart_base = uart_base;
    uart_init(uart_base, uart_clk, baud, NULL);
}

void console_putc(char c)
{
    if (g_uart_base != 0) {
        uart_putc(g_uart_base, c);
    }
}

void console_puts(const char *str)
{
    if (g_uart_base != 0) {
        uart_puts(g_uart_base, str);
    }
}

/* ------------------------------------------------------------------ */
/* ★ 排他输出(M4-8.4;M4-11.1 起只做"转发")★                          */
/* ------------------------------------------------------------------ */

/*
 * 本文件**只把"进/出排他区"这件事转给调用方装进来的钩子**,自己不再维护
 * 任何状态。
 *
 * ★ M4-11.1:这里原来有一个**全局**嵌套计数(`g_excl_depth`),规则是
 *   "进两次只叫一次 begin""出到 0 才叫 end""多退一次不叫 end"。
 *   那条规则在一个前提下是对的:**同一时刻只有一个写者** —— 而它靠的正是
 *   "排他 = 关调度"(调度关着的时候别的上下文根本跑不起来)。
 *
 *   换成真正的锁之后,那个前提没了,计数反而成了**漏洞**:
 *   `g_excl_depth` 是**全局**的,不是每个持有者一份。于是 A 线程进了排他区
 *   (0→1,取到锁),B 线程进来时看到 `depth == 1`,**一声不吭地直接打印** ——
 *   锁形同虚设。
 *
 *   上板实测就是这个症状(2026-09-13,第一次带锁的 M4-11.1):
 *     - 状态行被劈进自检报告中间,`verify_board.py` 判"报告不完整"
 *       (声明 90 passed,只解析到 85 条);
 *     - `console_excl_wait`(那把锁被竞争过吗)恒为 **0**。
 *
 *   ⇒ 嵌套从"按**深度**计数"改回"按**持有者**计数",而那正是源 OS
 *     `mutex_create(mtx, true)` 的递归计数(`rcc`)在做的事(见 src/mutex.c
 *     与 src/mutex_kern.c)。三件事于是自动成立,而且**每个持有者一份**:
 *       进两次   ⇒ rcc=2,锁没放;
 *       内层退出 ⇒ rcc=1,锁还在;
 *       多退一次 ⇒ `mutex_unlock` 返回 -EPERM(没拿锁的人放不掉),
 *                  外层保护照样在,而且这个用法错误**会被计数**。
 *
 * ⚠ 所以本文件现在不"保护"任何东西:钩子没装时它是空操作(启动早期只有
 *   一个写者,那正是对的);装了钩子时,互斥成立与否由那把锁负责。
 *   ⚠ 为什么做成钩子而不是直接调调度器/锁:低层输出模块不能依赖调度器
 *   (宿主单测编译 console.c 时会因符号未定义而链接失败,实测过),
 *   而且本项目对同类问题已有先例 —— `kstack` 的 TLB 维护也是函数指针。
 */
static console_excl_fn g_excl_begin;
static console_excl_fn g_excl_end;

void console_set_excl_hooks(console_excl_fn begin, console_excl_fn end)
{
    g_excl_begin = begin;
    g_excl_end   = end;
}

void console_excl_begin(void)
{
    if (g_excl_begin != NULL) {
        g_excl_begin();
    }
}

void console_excl_end(void)
{
    if (g_excl_end != NULL) {
        g_excl_end();
    }
}

void console_put_hex32(u32 value)
{
    static const char digits[] = "0123456789ABCDEF";
    int shift;

    console_puts("0x");
    for (shift = 28; shift >= 0; shift -= 4) {
        console_putc(digits[(value >> shift) & 0xFu]);
    }
}

void console_put_dec32(u32 value)
{
    char buffer[11];
    int  index = 0;

    if (value == 0) {
        console_putc('0');
        return;
    }

    while (value > 0 && index < (int)sizeof(buffer)) {
        buffer[index++] = (char)('0' + (value % 10u));
        value /= 10u;
    }

    while (index > 0) {
        console_putc(buffer[--index]);
    }
}

/* ------------------------------------------------------------------ */
/* 格式化输出                                                          */
/* ------------------------------------------------------------------ */

/*
 * 极简 va_list 支持:直接使用编译器内建,
 * 不依赖 stdarg.h(本项目是 -nostdinc)。
 */
typedef __builtin_va_list va_list_t;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)
#define va_end(ap)         __builtin_va_end(ap)

static void out_padded(const char *str, int width, bool zero_pad, bool left_align)
{
    int len = 0;
    int pad;

    while (str[len] != '\0') {
        len++;
    }

    pad = (width > len) ? (width - len) : 0;

    if (!left_align) {
        while (pad-- > 0) {
            console_putc(zero_pad ? '0' : ' ');
        }
    }

    console_puts(str);

    if (left_align) {
        while (pad-- > 0) {
            console_putc(' ');
        }
    }
}

static void out_unsigned(u32 value, u32 radix, bool upper, int width, bool zero_pad, bool left_align)
{
    static const char lower_digits[] = "0123456789abcdef";
    static const char upper_digits[] = "0123456789ABCDEF";
    const char       *digits = upper ? upper_digits : lower_digits;
    char              buffer[32];
    int               index = 0;
    int               pad;

    if (value == 0) {
        buffer[index++] = '0';
    } else {
        while (value > 0 && index < (int)sizeof(buffer)) {
            buffer[index++] = digits[value % radix];
            value /= radix;
        }
    }

    pad = (width > index) ? (width - index) : 0;
    if (!left_align) {
        while (pad-- > 0) {
            console_putc(zero_pad ? '0' : ' ');
        }
    }

    while (index > 0) {
        console_putc(buffer[--index]);
    }

    if (left_align) {
        while (pad-- > 0) {
            console_putc(' ');
        }
    }
}

void console_printf(const char *fmt, ...)
{
    va_list_t args;

    va_start(args, fmt);

    while (*fmt != '\0') {
        bool left_align = false;
        bool zero_pad   = false;
        int  width      = 0;

        if (*fmt != '%') {
            if (*fmt == '\n') {
                console_putc('\r');
            }
            console_putc(*fmt++);
            continue;
        }

        fmt++; /* 跳过 '%' */

        /* 解析标志 */
        if (*fmt == '-') {
            left_align = true;
            fmt++;
        }
        if (*fmt == '0') {
            zero_pad = true;
            fmt++;
        }

        /* 解析宽度 */
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        switch (*fmt) {
        case 'c': {
            char c = (char)va_arg(args, int);
            console_putc(c);
            fmt++;
            break;
        }
        case 's': {
            const char *str = va_arg(args, const char *);
            out_padded(str != NULL ? str : "(null)", width, zero_pad, left_align);
            fmt++;
            break;
        }
        case 'd':
        case 'i': {
            int value = va_arg(args, int);
            u32 magnitude;

            if (value < 0) {
                console_putc('-');
                magnitude = (u32)(-(value + 1)) + 1u; /* 避免 INT_MIN 取负溢出 */
                if (width > 0) {
                    width--;
                }
            } else {
                magnitude = (u32)value;
            }
            out_unsigned(magnitude, 10, false, width, zero_pad, left_align);
            fmt++;
            break;
        }
        case 'u': {
            out_unsigned(va_arg(args, u32), 10, false, width, zero_pad, left_align);
            fmt++;
            break;
        }
        case 'x': {
            out_unsigned(va_arg(args, u32), 16, false, width, zero_pad, left_align);
            fmt++;
            break;
        }
        case 'X': {
            out_unsigned(va_arg(args, u32), 16, true, width, zero_pad, left_align);
            fmt++;
            break;
        }
        case 'p': {
            /*
             * 经由 uintptr_t 转换,而不是直接 (u32)va_arg(args, void *)。
             * 直接截断指针在 32 位 ARM 上恰好能用,但在指针更宽的环境里
             * 会被编译器判为危险转换(-Wvoid-pointer-to-int-cast)。
             * 这个写法在两种环境下都表达同样的意图:取指针的数值。
             */
            u32 value = (u32)(uintptr_t)va_arg(args, void *);
            console_puts("0x");
            out_unsigned(value, 16, false, 8, true, false);
            fmt++;
            break;
        }
        case '%': {
            console_putc('%');
            fmt++;
            break;
        }
        case '\0': {
            /* 格式串以 '%' 结尾,直接结束 */
            goto done;
        }
        default: {
            /* 未知格式符:原样输出,便于发现拼写错误 */
            console_putc('%');
            console_putc(*fmt++);
            break;
        }
        }
    }

done:
    va_end(args);
}
