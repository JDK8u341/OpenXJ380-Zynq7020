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
            u32 value = (u32)va_arg(args, void *);
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
