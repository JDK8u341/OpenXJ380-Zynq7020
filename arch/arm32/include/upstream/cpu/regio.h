#pragma once

/*
 * 上游 <cpu/regio.h> 的 **ARM 覆盖层**。
 *
 * 覆盖的规矩见 `arch/arm32/include/upstream/cpu/lock.h` 的说明。
 *
 * ====================================================================
 * 这个覆盖层**只提供一个函数**,而且这是有意的
 * ====================================================================
 *
 * 上游那个头是 x86 的控制寄存器/标志头:`get_cr0()` / `get_cr3()` /
 * `get_rsp()` / `get_rflags()` / `set_cr0()` / `flush_tlb()` / `invlpg`。
 * 其中 **CR0/CR3/TLB 失效在 ARMv7 上没有对应物**(ARM 是 TTBR0/TTBR1 +
 * TLBIALL 一类的系统指令,而且移植侧的页表模型完全不同)。
 *
 * 它们**刻意没有在这里提供假的替身**:
 *   - 编一个 `get_cr3()` 返回 0 只是把失败推迟到运行时,而且推得很远;
 *   - 而"未声明"会让任何真正需要它的上游文件**当场编不过** ——
 *     那正是我们想要的信号:那个文件是 **x86 专属**的,不该被搬过来。
 *
 * 唯一被上游 VFS 真正用到的是 `are_interrupts_enabled()`
 * (`driver/fs/vfs/vfs.cpp:1334`),它 2026-09-18 从 `include/krlibc.h`
 * 挪进了上游的 `cpu/regio.h`(那里本来就是它的家)。
 */

#include <arch/cpu.h>

/*
 * 中断是否开着 ← x86 读 `RFLAGS.IF`(`cpu/regio.h` 的 `get_rflags() & (1<<9)`)。
 * ARM 读 `CPSR.I` —— 同一语义、不同载体。
 *
 * 移植侧早就有这个判断(`arch/cpu.h:151` 的 `arch_irq_enabled()`),
 * 这里只是给它上游的名字,不重复实现。
 */
static inline bool are_interrupts_enabled(void)
{
    return arch_irq_enabled();
}
