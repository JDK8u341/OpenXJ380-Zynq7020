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
 *   %c %s %d %i %u %o %x %X %b %p %n %%
 *   标志 - 0 #(以及被接受但**不生效**的 + 与空格)
 *   宽度(含 `*`)、精度(含 `.*`)、长度修饰符 hh h l ll L z t j
 *
 * ⚠ M4A-1.2 起这一族不再是"移植侧自己的小格式化器",而是**源 OS 的语义**:
 *   同一个核心(console.c 的 `format_core`)供三个入口共用 ——
 *   `console_printf`(控制台)、`write_serial_fmt`(串口)、`sprintf`(内存缓冲区),
 *   正如源 OS 的 `vwprintf(Writer *, ...)` 供 `write_serial_fmt`/`sprintf`/`printk` 共用
 *   (driver/serial/serial_port.cpp:463)。
 *
 *   为什么要做到这个程度:**长度修饰符缺失会让实参错位**。
 *   上游到处在打 `%llx`(kernel/memory/page.cpp:416 等),解析器若不认识 `l`,
 *   就不会取走那个 64 位实参,后面**每一个**实参都会取到别人的值 ——
 *   那不是"打得难看",是打出错误的数据。
 *
 * 不支持:浮点(`%f` 一族)。源 OS 支持,移植侧没有浮点格式化需求,
 *         需要时按同样的结构补(见 README 的退化清单)。
 *
 * 与源 OS 的**已知差异**(都记在 README 退化清单):
 *   1. 未知格式符:移植侧**原样吐出** `%x` 里的那个字符便于查错;
 *      源 OS 是**直接结束**(serial_port.cpp:382-384 的 `return 0`)。
 *   2. `+` 与空格标志:接受、但不产生正号。源 OS 生效。
 *   3. `%p`:形状与源 OS 一致(`0x` + 按指针宽度零填充),
 *      但宽度随架构 —— 32 位 ARM 上是 8 位,源 OS 的 x86_64 上是 16 位。
 */
/* ------------------------------------------------------------------ */
/* 格式化的**可变参数**入口                                            */
/* ------------------------------------------------------------------ */

/*
 * `va_list` 由编译器内建提供,不用 <stdarg.h>:本项目是 -nostdinc,
 * 而且上游与移植两侧都不该依赖 C 库头文件(上游那份
 * `include/stdarg.h` 也是同样一行 typedef)。
 * 两种写法是**同一个类型**,所以 C 侧与 C++ 侧的声明能对上。
 */
typedef __builtin_va_list va_list_t;

void console_printf(const char *fmt, ...);

/* 把已展开的可变参数打出去(console_printf 就是它的一层薄壳) */
int console_vprintf(const char *fmt, va_list_t args);

/* 把已展开的可变参数格式化进缓冲区,返回写入的字符数(不含结尾 NUL) */
int console_vsprintf(char *buf, const char *fmt, va_list_t args);

/*
 * ★ 有边界版本(M4A-1.5,给落地层定义上游 `snprintf` 用)★
 *
 * 语义照上游 `serial_port.cpp:761-790`(那也是 C99 的语义):
 *   - `size == 0` ⇒ **直接返回 0**,连 buf 都不碰;
 *   - 最多存 `size - 1` 个字符,**结尾永远是 NUL**;
 *   - 返回值是"**本该写多长**"(截断时**大于**实际存下的长度),不是存了多少。
 *
 * ⚠ 最后那一条是这一族函数最容易搞错的地方:按"存了多少"返回会让
 *   调用方以为写完了(上游 `pty.cpp:165` 就是拿它做 `slave_name`,
 *   截断与否必须能看出来)。
 */
int console_vsnprintf(char *buf, size_t size, const char *fmt, va_list_t args);

/* ------------------------------------------------------------------ */
/* ★ 源 OS 同名导出:sprintf / write_serial_fmt 为什么**不在**这里 ★   */
/* ------------------------------------------------------------------ */

/*
 * 上游的 VFS/tmpfs 会直接调 `sprintf`(路径拼接)与 `write_serial_fmt`
 * (tty 输出),所以链接期必须有这两个符号。**但它们不能由 C 侧给出**:
 *
 *   `include/proto.hpp` 里这两条声明**不在** `extern "C"` 块内
 *   (proto.hpp:38 与 :42),所以上游调用点要的是**经过 C++ 名字修饰**的符号。
 *   实测(`arm-none-eabi-nm out/arm32/upstream/driver/fs/vfs/vfs.o`):
 *
 *       U _Z16write_serial_fmtPKcz
 *       U _Z7sprintfPcPKcz
 *
 *   ⇒ 在 C 里定义 `sprintf` 得到的是**未修饰**符号,链接器照样报
 *     `undefined reference to 'sprintf(char*, char const*, ...)'`,
 *     而这一点从源码上完全看不出来 —— 只有 nm 才知道。
 *
 * ⇒ 那两个符号由 **C++ 落地层**(`arch/arm32/src/upstream_api.cpp`)给出,
 *   在那里只是转发到本文件的 `console_vprintf` / `console_vsprintf`:
 *   **实现只有一份**(在这里),符号形状则由上游的声明决定。
 *   所以本头文件**刻意不声明** `sprintf` / `write_serial_fmt` ——
 *   一份 C 链接的同名声明在这里就是个陷阱,契约测试
 *   (tests/test_arm32_console.py)专门盯着"它没有被加回来"。
 *
 * ⚠ 无边界检查 —— 名字就叫 sprintf,和源 OS 一样危险。
 *   上游另有 `snprintf` 做安全版本(serial_port.cpp:761);移植侧暂时只有这一个,
 *   调用点必须自己保证缓冲区够大。
 */

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
 * ## 它怎么做到互斥(★ M4-11.1 起是一把锁,不再是"关调度"★)
 *
 * M4-8.4 到 M4-10 用的办法是"进入时关掉调度、退出时恢复":调度关着的时候
 * 没有别的上下文能跑起来,于是"谁在打印"天然唯一 —— 不需要锁。
 *
 * ⚠ 那个办法只在**单核 + 单写者**的前提下成立,代价是排他期间整个系统停摆
 *   (自检报告要打几秒钟,这几秒里所有线程都醒不过来)。而且它**不是锁**:
 *   两个核上的写者谁也拦不住谁 —— 之所以没出事,只是因为会打印的线程
 *   恰好都被钉在 CPU0。
 *
 * M4-11.1 换成源 OS 的 **yield 型递归互斥**(`kernel/task/mutex.cpp`,
 * 见 `arch/mutex.h`):报告期间调度器照常跑,状态线程照常醒、照常被调度,
 * 只是**打印要等锁**;而互斥是真的(跨核也成立)。
 *
 * ⚠ **绝不能用自旋锁**:9600 波特下一行要 60~80ms,持锁者会被抢占,
 *   而等锁的一方若关着中断自旋,就再也没人能放锁 —— 死锁。
 *
 * ⚠ 嵌套由**锁的递归计数**按**持有者**负责(源 OS 的 `rcc`):
 *   进两次 ⇒ rcc=2;内层退出 ⇒ rcc=1(保护还在);多退一次 ⇒ `mutex_unlock`
 *   返回 -EPERM(没拿锁的人放不掉),而且会被计数。
 *   ⇒ 本模块**不**自己做嵌套计数 —— 那个全局计数在"第二个写者"面前是漏洞
 *     (它不认识持有者,于是 B 线程看到 depth != 0 就直接打印)。
 *     这段经过写在 `src/console.c` 的排他段上面。
 *
 * ⚠ 代价与边界:排他**只保护被它包住的输出**。kmain 里那些没包住的
 *   `console_printf` 是"尽力而为"的,状态行有可能插进它们中间;报告通道
 *   (机器可读的那一段)是包住的,不受影响。
 *
 * ## ★ 为什么是钩子,而不是直接调调度器/锁 ★
 *
 * 直接调会让**低层输出模块依赖调度器** —— 宿主单测编译 `console.c` 时
 * 会因为符号未定义而链接失败(实测就是这样)。
 * 本项目对同类问题已有先例:`kstack` 的 TLB 维护也是做成函数指针由调用方
 * 传进来(见 `arch/kstack.h`),好处两头都占 —— 模块内部不会漏做,
 * 纯逻辑部分又能在宿主上编译,而且宿主单测能**断言钩子被调用过**。
 *
 * 没装钩子时(`begin == NULL`)排他**退化成空操作** —— 启动早期只有一个
 * 写者,那正是对的。
 */
typedef void (*console_excl_fn)(void);

/*
 * 装入排他钩子。内核在**第一次打印之前**装(`console_mutex_init()`,
 * 见 `arch/mutex.h`);那一刻起排他由那把递归互斥负责。
 */
void console_set_excl_hooks(console_excl_fn begin, console_excl_fn end);

void console_excl_begin(void);
void console_excl_end(void);
