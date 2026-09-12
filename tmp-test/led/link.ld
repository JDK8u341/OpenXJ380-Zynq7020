/*
 * 裸机链接脚本：加载到 DDR 起始处（0x00100000，见设备树 memory@00100000）
 * 该地址在 ps7_init 完成 DDR 初始化后才可用。
 */
OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(_start)

SECTIONS
{
    . = 0x00100000;

    .text : {
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

    /* 64KB 栈，紧跟在 BSS 之后 */
    . = ALIGN(8);
    . += 0x10000;
    __stack_top = .;

    /DISCARD/ : {
        *(.ARM.exidx*)
        *(.ARM.extab*)
        *(.comment)
        *(.note*)
    }
}
