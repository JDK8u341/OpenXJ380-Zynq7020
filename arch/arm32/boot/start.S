/*
 * ARMv7-A / Cortex-A9 内核启动代码
 *
 * 前提(由 JTAG 直载流程保证):
 *   - 已执行 ps7_init 完成 PLL/时钟/DDR/MIO 初始化
 *   - MMU 关闭(地址 = 物理地址),D/I cache 关闭
 *   - 从 PLAT_KERNEL_LOAD (0x00100000) 处开始执行
 *
 * 职责:
 *   1. 屏蔽中断
 *   2. 为各处理器模式建立独立栈(IRQ/FIQ/ABT/UND 需要,否则异常一来就踩栈)
 *   3. 清零 BSS
 *   4. 跳入 C 入口 kmain
 *
 * 注意:x86_64 侧的 boot/BOOTX64.efi 负责 UEFI 协议交互与 ELF 装载,
 * 在 Zynq 上这些工作由 FSBL/U-Boot 或 JTAG 流程承担,所以这里只做
 * 最纯粹的 CPU 状态准备。
 */

    .syntax unified
    .arch armv7-a
    .arm

/* CPSR 模式位 */
    .equ    MODE_USR, 0x10
    .equ    MODE_FIQ, 0x11
    .equ    MODE_IRQ, 0x12
    .equ    MODE_SVC, 0x13
    .equ    MODE_ABT, 0x17
    .equ    MODE_UND, 0x1B
    .equ    MODE_SYS, 0x1F

    .equ    I_BIT,   0x80       /* IRQ 屏蔽 */
    .equ    F_BIT,   0x40       /* FIQ 屏蔽 */

    .section .text.boot, "ax"
    .global _start
    .type _start, %function

_start:
    /* ---- 1. 先进入 SVC 模式并屏蔽所有中断 ---- */
    mrs     r0, cpsr
    bic     r0, r0, #(MODE_USR | MODE_FIQ | MODE_IRQ | MODE_ABT | MODE_UND)
    orr     r0, r0, #(MODE_SVC | I_BIT | F_BIT)
    msr     cpsr_c, r0

    /*
     * ---- 2. 建立各模式的栈 ----
     * 每个模式一块独立区域,从 __stack_top 往下依次分配。
     * IRQ/FIQ/ABT/UND 如果不设栈,异常发生时会直接踩坏 SVC 栈。
     *
     * 各栈大小由链接脚本以绝对符号定义(boot/kernel.ld),
     * 这里是唯一的使用点,避免两处各写一份数字而不同步。
     */
    ldr     r1, =__stack_top

    /* IRQ 栈 */
    msr     cpsr_c, #(MODE_IRQ | I_BIT | F_BIT)
    mov     sp, r1
    ldr     r2, =STACK_SIZE_IRQ
    sub     r1, r1, r2

    /* FIQ 栈 */
    msr     cpsr_c, #(MODE_FIQ | I_BIT | F_BIT)
    mov     sp, r1
    ldr     r2, =STACK_SIZE_FIQ
    sub     r1, r1, r2

    /* ABT 栈 */
    msr     cpsr_c, #(MODE_ABT | I_BIT | F_BIT)
    mov     sp, r1
    ldr     r2, =STACK_SIZE_ABT
    sub     r1, r1, r2

    /* UND 栈 */
    msr     cpsr_c, #(MODE_UND | I_BIT | F_BIT)
    mov     sp, r1
    ldr     r2, =STACK_SIZE_UND
    sub     r1, r1, r2

    /* 回到 SVC,剩下的作为 SVC 栈 */
    msr     cpsr_c, #(MODE_SVC | I_BIT | F_BIT)
    mov     sp, r1

    /* ---- 3. 尽早安装异常向量表 ---- */
    /*
     * 必须在任何可能出错的 C 代码之前完成:VBAR 复位值是 0,
     * 而 0x00000000 处是 OCM,不是向量表 —— 此时一旦取指/访存出错,
     * CPU 会跳到一片没有意义的代码上,连现场都抓不到。
     */
    bl      vectors_install

    /* ---- 4. 清零 BSS ---- */
    ldr     r0, =__bss_start
    ldr     r1, =__bss_end
    mov     r2, #0
1:
    cmp     r0, r1
    strlo   r2, [r0], #4
    blo     1b

    /* ---- 5. 进入 C ---- */
    bl      kmain

    /* kmain 不应返回。若返回则停机等待调试器 */
2:
    wfe
    b       2b

    .size _start, . - _start
    .end
