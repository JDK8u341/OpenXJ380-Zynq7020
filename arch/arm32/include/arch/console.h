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

/* ------------------------------------------------------------------ */
/* ★ 排他输出(M4-8.4)★                                               */
/* ------------------------------------------------------------------ */

/*
 * 进入/退出"这一段输出不许别人插进来"的区域。**可嵌套**(计数)。
 *
 * ## 为什么需要它
 *
 * M4-8.4 把 1Hz 状态行搬进了一个内核线程。于是**串口有了两个写者**:
 * 那个线程,以及 kmain(idle)里的自检报告 / shell / 各对照组。
 * 上板立刻就出事了 —— 状态行正好插进自检报告中间,把
 * `=== SELF-TEST END ===` 劈成了两半:
 *
 *     ==[XJ380/arm32] alive loop=0 led=0x00 ...
 *     = SELF-TEST END ===
 *
 * 而**自检报告是机器可读的判定通道**(`tmp-test/verify_board.py` 按行解析),
 * 所以这不是"看着乱",是"一条命令判定过不过"这件事被打断了。
 *
 * ## 它怎么做到互斥(以及为什么不是自旋锁)
 *
 * 实现是"进入时关掉调度、退出时按计数恢复"(`sched_disable`/`sched_enable`)。
 * 关键在于:**调度关着的时候没有别的上下文能跑起来**,于是"谁在打印"
 * 天然唯一 —— 不需要锁。
 *
 * ⚠ **绝对不能用自旋锁**:9600 波特下一行要 60~80ms,持锁者会被抢占,
 *   而等锁的一方若自旋(还关着中断)就再也没人能放锁 —— 死锁。
 *   可睡眠的互斥体要等 M4-11;在那之前"关调度"是这里唯一正确的做法。
 *
 * ⚠ 代价:排他期间**整个系统停摆**(状态线程醒不过来、别的线程不推进)。
 *   所以只包"必须成段"的输出,别包住会等很久的东西。
 *   也正因如此,它**不是**通用的 printf 锁,而是一个"这一段我说了算"的作用域。
 */
void console_excl_begin(void);
void console_excl_end(void);
