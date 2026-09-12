#pragma once

/*
 * MMIO 访问原语
 *
 * Zynq 的 PS 外设全部是 MMIO(Cadence UARTPS / GEM / SD / GIC ...),
 * 与 x86 的端口 I/O 不同。这里提供带屏障语义的读写,
 * 供驱动与平台代码使用。
 */

#include <arch/types.h>

/* 编译期屏障:阻止编译器跨越重排,不产生指令 */
#define arch_compiler_barrier() __asm__ volatile("" ::: "memory")

/* 数据存储屏障:保证屏障前的访存对其他观察者可见(ARMv7: DMB) */
static inline void arch_dmb(void)
{
    __asm__ volatile("dmb" ::: "memory");
}

/* 数据同步屏障:更强,等待所有访存真正完成(ARMv7: DSB) */
static inline void arch_dsb(void)
{
    __asm__ volatile("dsb" ::: "memory");
}

/* 指令同步屏障:用于修改指令流/系统寄存器之后(ARMv7: ISB) */
static inline void arch_isb(void)
{
    __asm__ volatile("isb" ::: "memory");
}

/*
 * 32 位 MMIO 读写。
 * volatile 保证每次访问都真实发生;末尾的 dmb 保证顺序,
 * 这在访问 GIC/外设寄存器时是必要的(ARMv7 是弱内存序)。
 */
static inline void mmio_write32(uintptr_t addr, u32 value)
{
    *(volatile u32 *)addr = value;
    arch_dmb();
}

static inline u32 mmio_read32(uintptr_t addr)
{
    u32 value = *(volatile u32 *)addr;
    arch_dmb();
    return value;
}

static inline void mmio_write16(uintptr_t addr, u16 value)
{
    *(volatile u16 *)addr = value;
    arch_dmb();
}

static inline u16 mmio_read16(uintptr_t addr)
{
    u16 value = *(volatile u16 *)addr;
    arch_dmb();
    return value;
}

static inline void mmio_write8(uintptr_t addr, u8 value)
{
    *(volatile u8 *)addr = value;
    arch_dmb();
}

static inline u8 mmio_read8(uintptr_t addr)
{
    u8 value = *(volatile u8 *)addr;
    arch_dmb();
    return value;
}

/*
 * 带位域掩码的读-改-写。
 * 外设寄存器常需要只改若干位而不影响同寄存器的其它位。
 */
static inline void mmio_set_bits32(uintptr_t addr, u32 mask)
{
    u32 value = mmio_read32(addr);
    mmio_write32(addr, value | mask);
}

static inline void mmio_clear_bits32(uintptr_t addr, u32 mask)
{
    u32 value = mmio_read32(addr);
    mmio_write32(addr, value & ~mask);
}
