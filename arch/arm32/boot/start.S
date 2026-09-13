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
 * 本文件里有两个入口:
 *   _start      —— CPU0,由引导流程直接跳进来(见上)
 *   cpu1_entry  —— CPU1,由 CPU0 写 0xFFFFFFF0 + SEV 唤醒后跳进来(AM3-2)
 * 两者的共同点是"CPU 状态准备",区别见各自入口处的说明。
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

/*
 * 建立一个核的全部模式栈。
 *
 *   r0 = 该核栈区的顶部
 *   返回时处于 SVC 模式,sp 指向该核的 SVC 栈
 *
 * 做成子程序而不是给两个入口各抄一份:CPU0 与 CPU1 的栈布局必须
 * 逐字节一致,抄两份的结果必然是只改了其中一份。
 *
 * ⚠ 子程序内部**不能使用栈**、也不能碰 SVC 的 lr ——
 *   它靠 SVC 的 lr 返回。切到 IRQ/FIQ/ABT/UND 时那些模式的 lr 是
 *   banked 的,不会破坏 SVC 的 lr,所以这条路径是安全的。
 */
    .type   setup_mode_stacks, %function
setup_mode_stacks:
    mov     r1, r0

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
    bx      lr
    .size   setup_mode_stacks, . - setup_mode_stacks

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
     * 每个模式一块独立区域,从该核的栈区顶部往下依次分配。
     * IRQ/FIQ/ABT/UND 如果不设栈,异常发生时会直接踩坏 SVC 栈。
     *
     * 各栈大小由链接脚本以绝对符号定义(boot/kernel.ld),
     * 这里是唯一的使用点,避免两处各写一份数字而不同步。
     */
    ldr     r0, =__stack_top
    bl      setup_mode_stacks

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

/*
 * CPU1 入口(AM3-2)。
 *
 * 到达这里时的状态由 BootROM 的唤醒流程决定 —— 不是我们自己设的:
 *   - **ARM 态**(UG585 §6.1.10 明确:CPU1 的首次跳转只支持 ARM-32,
 *     如果这里被编译成 Thumb 指令,CPU1 会直接跑飞)
 *   - SVC 模式,IRQ/FIQ 屏蔽
 *   - **MMU 关、缓存关**(TTBR0/SCTLR 都是每核寄存器,CPU1 的是复位值)
 *
 * 与 _start 的三点关键区别:
 *
 *   1. **不清 BSS**。CPU0 早就清过了;CPU1 再清一遍会把 CPU0 已经
 *      写进 .bss 的所有状态(心跳指针、每 CPU 表、驱动状态)全部抹掉 ——
 *      而且是并发地抹,症状取决于时序,极难复现。
 *
 *   2. **用 __stack1_top**,不是 __stack_top。两个核各用自己那块栈区,
 *      否则第二个核一压栈就把第一个核的栈踩了。
 *
 *   3. **不写心跳阶段号**。那套标记记录的是 CPU0 的启动进度。
 *
 * 其余(建栈、装向量表)与 CPU0 完全一致,所以复用同一个子程序。
 */
    .global cpu1_entry
    .type   cpu1_entry, %function
cpu1_entry:
    /* 保险起见自己再确认一次 SVC + 中断屏蔽,不依赖 BootROM 的现场 */
    mrs     r0, cpsr
    bic     r0, r0, #(MODE_USR | MODE_FIQ | MODE_IRQ | MODE_ABT | MODE_UND)
    orr     r0, r0, #(MODE_SVC | I_BIT | F_BIT)
    msr     cpsr_c, r0

    ldr     r0, =__stack1_top
    bl      setup_mode_stacks

    /* VBAR 是每核寄存器,CPU1 必须自己装一次 */
    bl      vectors_install

    /* cpu1_main 永不返回;用 b 而不是 bl —— 没有可返回的地方 */
    b       cpu1_main

    .size cpu1_entry, . - cpu1_entry

    .end
