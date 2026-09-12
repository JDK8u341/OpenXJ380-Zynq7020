/*
 * Zynq-7020 内核链接脚本 (ARMv7-A / Cortex-A9)
 *
 * 与 x86_64 侧 linker.ld 的关键差异:
 *   - OUTPUT_FORMAT 为 elf32-littlearm,而非 elf64-x86-64
 *   - 没有高半区内核基址(x86 用 0xFFFFFFFF80000000)。ARMv7-A 是 32 位,
 *     且 Cortex-A9 复位后 MMU 关闭、按物理地址取指,
 *     所以内核直接链接在物理地址 PLAT_KERNEL_LOAD = 0x00100000 处运行。
 *   - 该地址必须与 ps7_init 初始化的 DDR 区间一致(设备树 memory@00100000)。
 */

OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(_start)

/*
 * 各异常模式的栈大小。start.S 通过同名符号读取,
 * 保持这里为唯一定义处。
 */
STACK_SIZE_IRQ = 0x2000;   /*  8 KB */
STACK_SIZE_FIQ = 0x1000;   /*  4 KB */
STACK_SIZE_ABT = 0x1000;   /*  4 KB */
STACK_SIZE_UND = 0x1000;   /*  4 KB */
STACK_TOTAL    = STACK_SIZE_IRQ + STACK_SIZE_FIQ + STACK_SIZE_ABT + STACK_SIZE_UND;

SECTIONS
{
    . = 0x00100000;
    _kernel_start = .;

    /*
     * 启动代码必须排在最前:JTAG/引导流程跳到 _start 时,
     * 第一条指令就得在 0x00100000 上。
     */
    /*
     * 异常向量表放最前,并单独成段。
     * VBAR 要求 32 字节对齐;虽然 start.S 里用 bl vectors_install 取地址,
     * 不依赖固定位置,但显式对齐能让布局意图一目了然。
     */
    . = ALIGN(32);
    .vectors : {
        KEEP(*(.vectors))
    }

    . = ALIGN(4);
    .text : {
        KEEP(*(.text.boot))
        KEEP(*(.text.vectors))
        *(.text)
        *(.text.*)
    }

    . = ALIGN(4);
    .rodata : {
        *(.rodata)
        *(.rodata.*)
    }

    . = ALIGN(4);
    .data : {
        *(.data)
        *(.data.*)
    }

    . = ALIGN(4);
    __bss_start = .;
    .bss : {
        *(.bss)
        *(.bss.*)
        *(COMMON)
    }
    . = ALIGN(8);
    __bss_end = .;

    /*
     * 一级页表(16KB)。
     *
     * 对齐是硬性要求而非优化:TTBR0 的低 14 位是属性字段而不是基址的一部分,
     * 表若没对齐到 16KB,硬件会从错位的地址取描述符 ——
     * 症状是随机把别的数据当成地址转换项,完全不可预测。
     * 下面的 ASSERT 把这条约束钉在链接期。
     *
     * 声明为 NOLOAD:这 16KB 全是零,没必要通过 JTAG 下载一遍;
     * 反正 mmu_build_l1_table() 会把 4096 项逐项写入。
     * (因此它不在 __bss_start..__bss_end 范围内,start.S 的清零不覆盖它,
     *  这是有意的 —— 页表在 BSS 清零之前就被使用是不允许的。)
     */
    . = ALIGN(16384);
    .mmu_tbl (NOLOAD) : {
        KEEP(*(.mmu_tbl))
    }
    ASSERT((g_mmu_l1_table & 0x3FFF) == 0,
           "一级页表必须 16KB 对齐:TTBR0 低 14 位是属性字段,不是基址")

    /* .mmu_tbl 占 4096 项 × 4 字节 */
    ASSERT(SIZEOF(.mmu_tbl) == 16384,
           "一级页表大小必须是 4096 项 × 4 字节 = 16KB")

    /*
     * 栈区:SVC 栈 + 四个异常模式栈。
     * start.S 从 __stack_top 向下依次分配各模式栈,
     * 断言保证分配总量不超过预留区。
     */
    . = ALIGN(8);
    __stack_bottom = .;
    . += STACK_TOTAL;
    . += 0x8000;                /* SVC 栈 32 KB */
    __stack_top = .;

    ASSERT(__stack_top - __stack_bottom >= STACK_TOTAL + 0x8000,
           "栈区预留不足:请检查 STACK_SIZE_* 与预留量的关系")

    _kernel_end = .;

    /DISCARD/ : {
        *(.ARM.exidx*)
        *(.ARM.extab*)
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
