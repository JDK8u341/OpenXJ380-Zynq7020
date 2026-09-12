/*
 * 裸机启动代码（ARMv7-A / Cortex-A9）
 * 职责：设置栈指针、清零 BSS、跳转 main
 */

    .syntax unified
    .arch armv7-a
    .arm

    .global _start
    .type _start, %function

_start:
    /* 关中断与快速中断，避免启动期被中断打断 */
    cpsid   if

    /* 设置各模式的栈指针（IRQ/FIQ/ABT/UND 用同一块栈空间顶部） */
    ldr     sp, =__stack_top

    /* 清零 BSS 段 */
    ldr     r0, =__bss_start
    ldr     r1, =__bss_end
    mov     r2, #0
1:
    cmp     r0, r1
    strlo   r2, [r0], #4
    blo     1b

    /* 进入 C 世界 */
    bl      main

    /* main 不应返回；若返回则原地停住 */
2:
    b       2b

    .size _start, . - _start
    .end
