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
 * 极简 va_list 支持:直接用编译器内建,
 * 不依赖 stdarg.h(本项目是 -nostdinc)。
 * ⚠ `va_list_t` 本身定义在 <arch/console.h> —— 因为 `console_vprintf` /
 *   `console_vsprintf` 要把它暴露给调用方(C++ 落地层也会用它,
 *   而上游那份 include/stdarg.h 里的 `va_list` 就是同一个内建类型)。
 */
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)
#define va_end(ap)         __builtin_va_end(ap)

/*
 * ★ 输出汇(sink):把"字符写到哪儿"和"怎么格式化"分开 ★
 *
 * 为什么要分:源 OS 的**同一次**格式化被三个入口复用 ——
 * `write_serial_fmt`(串口)、`sprintf`(内存缓冲区)、`printk`,
 * 它们都调 `vwprintf(Writer *, ...)`(driver/serial/serial_port.cpp:463),
 * 只是传入的 `Writer` 不同。移植侧若给每个入口各写一份格式化循环,
 * 三者迟早会不一致;而 M4A-1.2 起**上游的 VFS/tmpfs 代码会直接调 `sprintf`**,
 * 它必须和 `console_printf` 拿到**同一套语义**。
 */
typedef void (*fmt_sink_t)(void *ctx, char c);

/*
 * 控制台汇:顺带做 LF → CRLF。
 *
 * ⚠ 换行转换放在**汇**里,不放在格式化循环里 —— 理由是源 OS 就是这么分的:
 *   它的 `vwprintf` 原样写字符,CRLF 由**下层串口**补(经 `write_serial_string`)。
 *   移植侧对应的下层就是 `uart_puts`(src/uart_ps.c:217)。
 *
 *   放在汇里还**顺手修掉一个不一致**:换行转换原先写在格式化循环里,
 *   于是只有**格式串里的字面 '\n'** 会被补 CR,而 `%c` 传进来的 '\n'
 *   (以及经 `%s` 传进来的 '\n' —— 那条走的是 `console_puts`)行为各不相同。
 *   现在三条路径都由汇统一处理,`console_printf` 的可观测输出**不变**
 *   (tests/test_arm32_console.py 的 newline 判据就是钉这个的)。
 */
static void sink_console(void *ctx, char c)
{
    (void)ctx;

    if (c == '\n') {
        console_putc('\r');
    }
    console_putc(c);
}

/* 内存缓冲汇:**不**做换行转换(与源 OS 的 `UnsafeBufWriter` 一致) */
typedef struct {
    char *buf;
    int   len;
} buf_sink_t;

static void sink_buffer(void *ctx, char c)
{
    buf_sink_t *state = (buf_sink_t *)ctx;

    state->buf[state->len++] = c;
}

/* 一次格式化的状态:一个汇 + 已写字符数(作为 `sprintf` 的返回值) */
typedef struct {
    fmt_sink_t sink;
    void      *ctx;
    int        written;
} fmt_out_t;

static void out_char(fmt_out_t *out, char c)
{
    out->sink(out->ctx, c);
    out->written++;
}

static void out_repeat(fmt_out_t *out, char c, int count)
{
    while (count-- > 0) {
        out_char(out, c);
    }
}

/*
 * `%s`。pad 一律用**空格**(源 OS 的 `%s` 也是空格,不看 `0` 标志,
 * 见 serial_port.cpp:419);`prec` 限制最大长度。
 */
static void out_text(fmt_out_t *out, const char *str, int width, int prec, bool left_align)
{
    int len = 0;
    int pad;
    int index;

    while (str[len] != '\0' && (prec < 0 || len < prec)) {
        len++;
    }

    pad = (width > len) ? (width - len) : 0;

    if (!left_align) {
        out_repeat(out, ' ', pad);
    }

    for (index = 0; index < len; index++) {
        out_char(out, str[index]);
    }

    if (left_align) {
        out_repeat(out, ' ', pad);
    }
}

/*
 * ★ 长度修饰符 ★
 *
 * 为什么必须支持:上游代码**到处**在打 `%llx` / `%llu` / `%zu`
 * (例如 kernel/memory/page.cpp:416 的 `[pfregs] rax=0x%llx ...`)。
 * 不支持长度修饰符不只是"打印得难看" —— 解析器不认识 `l` 就**不会取走**
 * 那个 64 位实参,后续所有实参**全部错位**,打出来的是别人的值。
 * 所以这里的目标是:**源 OS 会取走的实参,移植侧一个不少地取走。**
 */
typedef enum {
    FMT_LEN_DEFAULT = 0, /* int / unsigned int(默认实参提升)*/
    FMT_LEN_CHAR,        /* hh */
    FMT_LEN_SHORT,       /* h  */
    FMT_LEN_LONG,        /* l  */
    FMT_LEN_LONGLONG,    /* ll 或 L */
    FMT_LEN_SIZE         /* z / t / j */
} fmt_len_t;

/*
 * 按长度修饰符取一个无符号实参。
 *
 * ⚠⚠ 形参是 `va_list_t *`(**指针**),不是 `va_list_t`。
 *   这不是风格问题:传值会让"取过几个实参"这件事**丢掉**,
 *   于是后面每一次转换都读到同一个槽位。
 *
 *   实测(宿主单测抓到的,2026-09-14):
 *     console_printf("%d|%s", 42, "ok")  ⇒ **访问违例**:
 *       `%d` 在 take_signed 里取走 42(但它拿的是自己的副本),
 *       回到 format_core 后游标**没动**,`%s` 于是把 42 当指针解引用。
 *     console_printf("%llx|%d", 0x1122334455667788ull, 42)
 *       ⇒ 打出 `1122334455667788|1432778632`,而 1432778632 = 0x55667788
 *         正是那个 64 位量的**低半部分** —— 又一次从同一个槽位取。
 *
 *   ⚠ 这条**不是宿主特有的**:ARM EABI 的 `va_list` 是
 *     `struct __va_list { void *__ap; }`(结构体,传值即复制),
 *     Windows x64 的是 `char *`(传值同样是复制)。
 *     两种 ABI 下"在子函数里推进游标"都会丢 —— 也就是说这个写法
 *     在**目标板**上一样会打出错值。宿主单测把它拦在了上板之前。
 *
 * ⚠ 这里用**类型本身**去取(`unsigned long` 而不是写死 4 字节),
 *   因为宽窄由宿主 ABI 决定:32 位 ARM 上 `long` 是 4 字节,
 *   x86_64 上 `long` 是 8 字节 —— 写死字节数会让宿主单测和产品行为分家。
 *   源 OS 用的是 `SIZE_T` / `LONG_2` 这套自己的枚举,意图相同。
 * ⚠ `size_t` 在 32 位 ARM 上是 `unsigned int`(见 arch/types.h:34),
 *   与 `unsigned long` 同宽,用后者取是安全的。
 */
static u64 take_unsigned(va_list_t *args, fmt_len_t len)
{
    switch (len) {
    case FMT_LEN_CHAR:
        return (u64)(unsigned char)va_arg(*args, unsigned int);
    case FMT_LEN_SHORT:
        return (u64)(unsigned short)va_arg(*args, unsigned int);
    case FMT_LEN_LONG:
        return (u64)va_arg(*args, unsigned long);
    case FMT_LEN_LONGLONG:
        return (u64)va_arg(*args, unsigned long long);
    case FMT_LEN_SIZE:
        return (u64)va_arg(*args, size_t);
    case FMT_LEN_DEFAULT:
    default:
        return (u64)va_arg(*args, unsigned int);
    }
}

/* 有符号取参:返回**绝对值**,`negative` 报告符号(形参同样必须是指针,理由见上)*/
static u64 take_signed(va_list_t *args, fmt_len_t len, bool *negative)
{
    long long value;

    switch (len) {
    case FMT_LEN_CHAR:
        value = (signed char)va_arg(*args, int);
        break;
    case FMT_LEN_SHORT:
        value = (short)va_arg(*args, int);
        break;
    case FMT_LEN_LONG:
        value = (long)va_arg(*args, long);
        break;
    case FMT_LEN_LONGLONG:
        value = (long long)va_arg(*args, long long);
        break;
    case FMT_LEN_SIZE:
        /* `%zd` 的有符号对应物。移植侧没有 ptrdiff_t,`long` 在两种宿主上
           都与指针同宽,正是这里要的宽度。 */
        value = (long)va_arg(*args, long);
        break;
    case FMT_LEN_DEFAULT:
    default:
        value = (int)va_arg(*args, int);
        break;
    }

    *negative = (value < 0);
    if (value < 0) {
        /* 先 +1 再取负,避免 LLONG_MIN 直接取负溢出 */
        return (u64)(-(value + 1)) + 1u;
    }
    return (u64)value;
}

static void out_digits(fmt_out_t *out, const char *prefix, int prefix_len,
                       const char *reversed, int count)
{
    int index;

    for (index = 0; index < prefix_len; index++) {
        out_char(out, prefix[index]);
    }
    while (count > 0) {
        out_char(out, reversed[--count]);
    }
}

/*
 * 整数输出。覆盖 `%d %i %u %o %x %X %b %p`。
 *
 * 前缀(符号、`0x`/`0`/`0b`)与数字**作为一个整体**参与宽度计算,
 * 并且 0 填充插在**前缀之后** —— 否则 `%#08x` 会打成 `00000x1f`。
 */
static void out_number(fmt_out_t *out, u64 value, bool negative, u32 radix, bool upper,
                       int width, int prec, bool zero_pad, bool left_align, bool alt_prefix)
{
    static const char lower_digits[] = "0123456789abcdef";
    static const char upper_digits[] = "0123456789ABCDEF";
    const char       *digits = upper ? upper_digits : lower_digits;
    char              reversed[70]; /* 64 位二进制(64)+ 余量 */
    char              prefix[4];
    int               count      = 0;
    int               prefix_len = 0;
    int               pad;
    bool              pad_zero;

    if (value == 0) {
        /* 精度为 0 且值为 0 ⇒ 不输出任何数字(C 的 `printf("%.0d", 0)`) */
        if (prec != 0) {
            reversed[count++] = '0';
        }
    } else {
        while (value > 0 && count < (int)sizeof(reversed)) {
            reversed[count++] = digits[(u32)(value % (u64)radix)];
            value /= (u64)radix;
        }
    }

    /* 精度 = 最少位数 */
    while (prec > count && count < (int)sizeof(reversed)) {
        reversed[count++] = '0';
    }

    if (negative) {
        prefix[prefix_len++] = '-';
    }
    /*
     * 前缀**无条件**加(值是不是 0 都一样)。
     * ⚠ 这一条与 ISO C 在 `%#x` 打 0 的角落不同(标准要求那时省掉 `0x`),
     *   取源 OS 的行为:它的 `#`/`%p` 都只看标志,不看值。
     *   `%p` 打空指针时也就能得到 `0x00000000` 而不是 `00000000`。
     */
    if (alt_prefix) {
        if (radix == 16) {
            prefix[prefix_len++] = '0';
            prefix[prefix_len++] = upper ? 'X' : 'x';
        } else if (radix == 8) {
            prefix[prefix_len++] = '0';
        } else if (radix == 2) {
            prefix[prefix_len++] = '0';
            prefix[prefix_len++] = upper ? 'B' : 'b';
        }
    }

    pad      = (width > count + prefix_len) ? (width - count - prefix_len) : 0;
    pad_zero = zero_pad && (prec < 0); /* 给了精度就不能再用 0 填充(C 的规定)*/

    if (left_align) {
        out_digits(out, prefix, prefix_len, reversed, count);
        out_repeat(out, ' ', pad);
    } else if (pad_zero) {
        int index;

        for (index = 0; index < prefix_len; index++) {
            out_char(out, prefix[index]);
        }
        out_repeat(out, '0', pad);
        while (count > 0) {
            out_char(out, reversed[--count]);
        }
    } else {
        out_repeat(out, ' ', pad);
        out_digits(out, prefix, prefix_len, reversed, count);
    }
}

/* ------------------------------------------------------------------ */
/* 格式化核心:与"写到哪儿"无关                                          */
/* ------------------------------------------------------------------ */

static void format_core(fmt_out_t *out, const char *fmt, va_list_t args)
{
    while (*fmt != '\0') {
        bool      left_align = false;
        bool      zero_pad   = false;
        bool      alt_prefix = false;
        int       width      = 0;
        int       prec       = -1; /* -1 = 没给精度 */
        fmt_len_t len        = FMT_LEN_DEFAULT;

        if (*fmt != '%') {
            out_char(out, *fmt++);
            continue;
        }

        fmt++; /* 跳过 '%' */

        /*
         * 解析标志。
         * `+` 与空格(强制正号)**被接受但不生效** —— 源 OS 认这两个标志
         * (serial_port.cpp:265-266),而移植侧至今没有一处需要它们。
         * 之所以要"吃掉"它们:`+` 若不认,就会被当成未知格式符,
         * **实参不取走**,后面全部错位。
         */
        for (;;) {
            if (*fmt == '-') {
                left_align = true;
            } else if (*fmt == '0') {
                zero_pad = true;
            } else if (*fmt == '#') {
                alt_prefix = true;
            } else if (*fmt == '+' || *fmt == ' ') {
                /* 接受但忽略:见上 */
            } else {
                break;
            }
            fmt++;
        }

        /* 宽度。`*` 从实参取,负宽度 = 左对齐(与 C 一致) */
        if (*fmt == '*') {
            int arg_width = va_arg(args, int);

            if (arg_width < 0) {
                left_align = true;
                arg_width  = -arg_width;
            }
            width = arg_width;
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9') {
                width = width * 10 + (*fmt - '0');
                fmt++;
            }
        }

        /* 精度。`.` 开头,`.*` 从实参取,负精度当作"没给" */
        if (*fmt == '.') {
            fmt++;
            if (*fmt == '*') {
                int arg_prec = va_arg(args, int);

                prec = (arg_prec < 0) ? -1 : arg_prec;
                fmt++;
            } else {
                prec = 0;
                while (*fmt >= '0' && *fmt <= '9') {
                    prec = prec * 10 + (*fmt - '0');
                    fmt++;
                }
            }
        }

        /* 长度修饰符 */
        if (*fmt == 'h') {
            fmt++;
            len = FMT_LEN_SHORT;
            if (*fmt == 'h') {
                fmt++;
                len = FMT_LEN_CHAR;
            }
        } else if (*fmt == 'l') {
            fmt++;
            len = FMT_LEN_LONG;
            if (*fmt == 'l') {
                fmt++;
                len = FMT_LEN_LONGLONG;
            }
        } else if (*fmt == 'L') {
            fmt++;
            len = FMT_LEN_LONGLONG;
        } else if (*fmt == 'z' || *fmt == 't' || *fmt == 'j') {
            fmt++;
            len = FMT_LEN_SIZE;
        }

        switch (*fmt) {
        case 'c': {
            char c = (char)va_arg(args, int);

            if (width > 1 && !left_align) {
                out_repeat(out, ' ', width - 1);
            }
            out_char(out, c);
            if (width > 1 && left_align) {
                out_repeat(out, ' ', width - 1);
            }
            fmt++;
            break;
        }
        case 's': {
            const char *str = va_arg(args, const char *);

            out_text(out, str != NULL ? str : "(null)", width, prec, left_align);
            fmt++;
            break;
        }
        case 'd':
        case 'i': {
            bool negative  = false;
            u64  magnitude = take_signed(&args, len, &negative);

            out_number(out, magnitude, negative, 10, false, width, prec, zero_pad, left_align, false);
            fmt++;
            break;
        }
        case 'u': {
            out_number(out, take_unsigned(&args, len), false, 10, false, width, prec, zero_pad, left_align, false);
            fmt++;
            break;
        }
        case 'o': {
            out_number(out, take_unsigned(&args, len), false, 8, false, width, prec, zero_pad, left_align, alt_prefix);
            fmt++;
            break;
        }
        case 'x': {
            out_number(out, take_unsigned(&args, len), false, 16, false, width, prec, zero_pad, left_align, alt_prefix);
            fmt++;
            break;
        }
        case 'X': {
            out_number(out, take_unsigned(&args, len), false, 16, true, width, prec, zero_pad, left_align, alt_prefix);
            fmt++;
            break;
        }
        case 'b': {
            /* 二进制。源 OS 有(serial_port.cpp:379),移植侧跟着有 */
            out_number(out, take_unsigned(&args, len), false, 2, false, width, prec, zero_pad, left_align, alt_prefix);
            fmt++;
            break;
        }
        case 'p': {
            /*
             * 指针。源 OS 的规则是"`0x` + 按**指针宽度**零填充"
             * (serial_port.cpp:367-373:`special` + `zeropad` + `size = 16`)。
             * 那个 16 是 x86_64 的指针宽度;**32 位 ARM 上等价的宽度是 8**,
             * 于是 0x1234 打成 `0x00001234` —— 形状与源 OS 一致,位数随架构。
             * tests/test_arm32_console.py 钉住了这个输出。
             *
             * ⚠ 经由 uintptr_t 转换,而不是直接 (u32)va_arg(args, void *)。
             *   直接截断指针在 32 位 ARM 上恰好能用,但在指针更宽的环境里
             *   会被编译器判为危险转换(-Wvoid-pointer-to-int-cast)。
             *   这个写法在两种环境下都表达同样的意图:取指针的数值。
             */
            u64 value = (u64)(uintptr_t)va_arg(args, void *);

            /*
             * ⚠ 宽度要**连 `0x` 一起**算:`out_number` 的 width 是"整个字段宽",
             *   而 `0x` 占两格。所以下限是 10(= 2 + 8 位数字),不是 8。
             *   这条是宿主单测(RESULT 0x001234 ≠ 0x00001234)抓出来的。
             */
            out_number(out, value, false, 16, false, width > 10 ? width : 10, -1, true, false, true);
            fmt++;
            break;
        }
        case 'n': {
            /*
             * ⚠ 源 OS 的 `%n` 是**空操作**:它只把指针取走、不写回
             *   (serial_port.cpp:380)。移植侧保持一致。
             *   **绝不能**做成 glibc 那个"写回已输出字符数"的语义 ——
             *   那既是安全隐患,也和源 OS 不一致。这里取走实参只为不错位。
             */
            (void)va_arg(args, void *);
            fmt++;
            break;
        }
        case '%': {
            out_char(out, '%');
            fmt++;
            break;
        }
        case '\0': {
            /* 格式串以 '%' 结尾,直接结束 */
            return;
        }
        default: {
            /*
             * 未知格式符:原样输出,便于发现拼写错误。
             * ⚠ 这一条与源 OS **不同**:它的 default 是直接 `return 0`,
             *   后面的内容一个都不打(serial_port.cpp:382-384);
             *   移植侧选"原样吐出来",因为格式串写错时"看得见"比
             *   "静默截断"好查。差异记在 arch/console.h 里。
             */
            out_char(out, '%');
            out_char(out, *fmt++);
            break;
        }
        }
    }
}

/* ------------------------------------------------------------------ */
/* 入口:同一个核心,不同的汇                                          */
/* ------------------------------------------------------------------ */

int console_vprintf(const char *fmt, va_list_t args)
{
    fmt_out_t out;

    out.sink    = sink_console;
    out.ctx     = NULL;
    out.written = 0;

    format_core(&out, fmt, args);
    return out.written;
}

int console_vsprintf(char *buf, const char *fmt, va_list_t args)
{
    fmt_out_t  out;
    buf_sink_t state;

    state.buf = buf;
    state.len = 0;

    out.sink    = sink_buffer;
    out.ctx     = &state;
    out.written = 0;

    format_core(&out, fmt, args);

    buf[state.len] = '\0';
    return state.len;
}

/*
 * ★ 有边界的缓冲区汇(M4A-1.5,给上游的 `snprintf` 用)★
 *
 * 与上面那个 `buf_sink_t` 的差别只有一个:**写满就不写了**,但**照样计数**。
 * 计数是关键 —— 上游 `snprintf` 的返回值在截断时返回的是"**本该写多长**"
 * (`serial_port.cpp:761-790`:`return (int)(data.truncated ? result : data.idx);`
 * 而 `result` 是格式化出的总字符数)。C99 也是这个语义,两者一致。
 */
typedef struct {
    char *buf;
    int   len;
    int   limit; /* 最多能存几个字符(不含结尾 NUL)*/
} bounded_buf_sink_t;

static void sink_buffer_bounded(void *ctx, char c)
{
    bounded_buf_sink_t *state = (bounded_buf_sink_t *)ctx;

    if (state->len < state->limit) {
        state->buf[state->len] = c;
    }
    state->len++;
}

int console_vsnprintf(char *buf, size_t size, const char *fmt, va_list_t args)
{
    fmt_out_t          out;
    bounded_buf_sink_t state;
    int                stored;

    /* ⚠ 与上游一致:`size == 0` **直接返回 0**,连碰都不碰 buf */
    if (buf == NULL || size == 0u) {
        return 0;
    }

    state.buf   = buf;
    state.len   = 0;
    state.limit = (int)size - 1;

    buf[0] = '\0';

    out.sink    = sink_buffer_bounded;
    out.ctx     = &state;
    out.written = 0;

    format_core(&out, fmt, args);

    /* 截断时结尾照样要有 NUL —— 位置是"实际存下的那几个字符之后" */
    stored         = (state.len < state.limit) ? state.len : state.limit;
    buf[stored]    = '\0';

    return state.len; /* ← 截断时是"本该多长",不是"存了多少" */
}

void console_printf(const char *fmt, ...)
{
    va_list_t args;

    va_start(args, fmt);
    (void)console_vprintf(fmt, args);
    va_end(args);
}

/*
 * ★ 为什么这里**没有** `sprintf` / `write_serial_fmt` ★
 *
 * 上游确实需要这两个符号(它的 VFS/tmpfs 直接调),但**不能由 C 侧定义**:
 * `include/proto.hpp:38,42` 把它们声明在 `extern "C"` **之外**,
 * 于是上游调用点要的是名字修饰过的符号 ——
 * `arm-none-eabi-nm out/arm32/upstream/driver/fs/vfs/vfs.o` 实测:
 *
 *     U _Z16write_serial_fmtPKcz
 *     U _Z7sprintfPcPKcz
 *
 * 这两条只能由 **C++ 落地层**(arch/arm32/src/upstream_api.cpp)给出,
 * 那里转发到本文件的 `console_vprintf` / `console_vsprintf`。
 * ⇒ 格式化逻辑只有这一份;符号形状由上游的声明决定。
 *   详细理由见 arch/console.h 的同名小节。
 */
