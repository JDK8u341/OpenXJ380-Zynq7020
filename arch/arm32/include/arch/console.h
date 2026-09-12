#pragma once

/*
 * 最小控制台:把格式化输出送到 UARTPS。
 *
 * 对应 x86_64 侧的 driver/serial/serial_port.cpp 中的
 * write_serial_string / write_serial_fmt / printk 一族。
 * 这里只实现内核启动阶段够用的子集,不做缓冲区与并发处理。
 */

#include <arch/types.h>

/* 绑定控制台到某个 UART。uart_clk 见 arch/uart_ps.h 的说明 */
void console_init(uintptr_t uart_base, u32 uart_clk, u32 baud);

/* 原始输出 */
void console_putc(char c);
void console_puts(const char *str);

/*
 * 格式化输出。支持:
 *   %c %s %d %i %u %x %X %p %%  以及 %08x / %-8s 这类宽度与 0 填充
 * 不支持浮点、不支持 64 位长度修饰符(需要时再扩)。
 */
void console_printf(const char *fmt, ...);

/* 直接写入 32 位十六进制,便于打印寄存器 */
void console_put_hex32(u32 value);
void console_put_dec32(u32 value);
