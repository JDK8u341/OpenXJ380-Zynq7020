#pragma once

#include <stdint.h>

static inline uint64_t get_cr0()
{
    uint64_t cr0 = 0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    return cr0;
}

static inline uint64_t get_cr3()
{
    uint64_t cr3 = 0;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

static inline uint64_t get_rsp()
{
    uint64_t rsp = 0;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    return rsp;
}

static inline uint64_t get_rflags()
{
    uint64_t rflags = 0;
    __asm__ volatile("pushfq\n"
                     "pop %0\n"
                     : "=r"(rflags)
                     :
                     : "memory");
    return rflags;
}

static inline void set_cr0(uint64_t cr0)
{
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));
}

static inline void flush_tlb(uint64_t addr)
{
    __asm__ volatile("invlpg (%0)" ::"r"(addr) : "memory");
}

/*
 * 中断是否开着 —— **从 `include/krlibc.h` 挪过来的**(平台中立重构,2026-09-18)。
 *
 * 它读的是 x86 的 `RFLAGS.IF`,是架构相关的东西,却原先住在平台中立的
 * "libc" 头里。后果不是"不优雅"而是**编不过**:ARM 侧编到它时汇编器报
 * `Error: bad instruction 'pushfq'`;而它是 `static inline`,只有被调用
 * 才生成代码 ⇒ 这个雷一直埋着,直到上游 `driver/fs/vfs/vfs.cpp` 被编进
 * ARM 才响(那个文件第 1334 行调用它)。
 *
 * 新家就是这里 —— `get_rflags()` 本来就在上面几行,同一个寄存器读两遍是
 * 没道理的。
 *
 * ARM 侧对应物:`arch/arm32/include/upstream/cpu/regio.h`(读 CPSR.I,
 * 即移植侧的 `arch_irq_enabled()`)。同一语义、不同载体 ——
 * 与 `spin_t` 的 `cpsr`/`rflags` 是同一个道理。
 */
static inline bool are_interrupts_enabled()
{
    return (get_rflags() & (1u << 9)) != 0; /* bit9 = IF */
}
