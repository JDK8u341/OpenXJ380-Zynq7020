/*
 * 寄存器帧相关的 CPU 原语 —— M4-6
 *
 * 与 kstack_hw.c / mmu_hw.c / percpu_hw.c 同一条分层线:C 语法与
 * 内联汇编分开,好让 arch/taskctx.h 那部分能在宿主机上编译并单测。
 */

#include <arch/cpu.h>
#include <arch/taskctx.h>

/*
 * 读当前 SP。
 *
 * 为什么需要:x86 侧的 `get_rsp()`(include/cpu/regio.h:19)被用来给
 * idle 线程造初始上下文(kernel/main.cpp:527、kernel/smp/smp.cpp:156)。
 * ARM 侧同样需要 —— 而 SP 不能用 C 直接读(它不是个名字,是个寄存器)。
 *
 * ⚠ 读到的是**调用本函数那一刻**的 SP,不是调用者的 SP ——
 *   本函数自己也有栈帧。所以它只能用来回答"当前大致在栈的哪个位置",
 *   不能用来精确恢复某个上下文。真正精确的读法在汇编里(把 sp 存进帧)。
 */
u32 arch_read_sp(void)
{
    u32 sp;

    __asm__ volatile("mov %0, sp" : "=r"(sp));

    return sp;
}
