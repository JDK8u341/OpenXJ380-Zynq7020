/*
 * ARMv7-A 异常向量表与入口
 *
 * 对应 x86_64 侧的 kernel/pctable/idt.cpp + kernel/intr/handler.S。
 * 两者的机制完全不同:
 *   x86: 256 项 IDT,每项一个描述符,异常/中断统一从 IDT 查表
 *   ARM: 固定 8 项的向量表,基址放进 VBAR,每项只有一条指令的位置
 *
 * ARMv7-A 的 8 个向量(顺序固定,不可改):
 *   offset 0x00  Reset
 *   offset 0x04  Undefined Instruction
 *   offset 0x08  Supervisor Call (SVC)   <- 将来的系统调用入口
 *   offset 0x0C  Prefetch Abort
 *   offset 0x10  Data Abort
 *   offset 0x14  Reserved
 *   offset 0x18  IRQ                     <- 外设中断
 *   offset 0x1C  FIQ
 *
 * 向量表必须 32 字节对齐;VBAR 的低 5 位被硬件忽略。
 */

    .syntax unified
    .arch armv7-a
    .arm

/* CPSR 模式位 */
    .equ    MODE_SVC, 0x13
    .equ    MODE_IRQ, 0x12
    .equ    I_BIT,    0x80
    .equ    F_BIT,    0x40

/* ------------------------------------------------------------------ */
/* 向量表                                                              */
/* ------------------------------------------------------------------ */

    .section .vectors, "ax"
    .align  5                       /* 32 字节对齐,VBAR 的硬性要求 */
    .global _vectors
    .type   _vectors, %function

_vectors:
    b       _vec_reset
    b       _vec_undefined
    b       _vec_svc
    b       _vec_prefetch_abort
    b       _vec_data_abort
    b       _vec_reserved
    b       _vec_irq
    b       _vec_fiq

    .size   _vectors, . - _vectors

/* ------------------------------------------------------------------ */
/* 各向量入口                                                          */
/* ------------------------------------------------------------------ */

    .section .text.vectors, "ax"

/*
 * Reset:正常启动不会走到这里(start.S 的 _start 是上电入口)。
 * 真到了这里说明发生了软复位,直接重新走启动流程。
 */
    .global _vec_reset
_vec_reset:
    ldr     pc, =_start

/*
 * Undefined Instruction:通常是踩到了未实现的指令或数据被当代码执行。
 * 早期内核里这是最有价值的诊断之一,所以打印现场而不是静默挂死。
 */
    .global _vec_undefined
_vec_undefined:
    sub     lr, lr, #4
    push    {r0-r12, lr}
    mov     r0, sp
    bl      c_undef_handler
1:  wfe
    b       1b

/*
 * SVC:系统调用入口。M1 阶段还没有用户态,先落到诊断处理。
 * M4 会把这里换成真正的 syscall 入口。
 */
    .global _vec_svc
_vec_svc:
    push    {r0-r12, lr}
    mov     r0, sp
    bl      c_svc_handler
    pop     {r0-r12, lr}
    movs    pc, lr

/*
 * Prefetch Abort:取指失败。MMU 打开后最常见的是权限/映射问题。
 */
    .global _vec_prefetch_abort
_vec_prefetch_abort:
    sub     lr, lr, #4
    push    {r0-r12, lr}
    mov     r0, sp
    bl      c_prefetch_abort_handler
1:  wfe
    b       1b

/*
 * Data Abort:访存失败。访问未映射的 PL 地址就会走到这里。
 */
    .global _vec_data_abort
_vec_data_abort:
    sub     lr, lr, #4
    push    {r0-r12, lr}
    mov     r0, sp
    bl      c_data_abort_handler
1:  wfe
    b       1b

    .global _vec_reserved
_vec_reserved:
1:  wfe
    b       1b

/*
 * IRQ:外设中断统一入口。
 *
 * 时序要点:
 *   进入 IRQ 时硬件已经把 CPSR 存进 SPSR_irq,返回地址放在 LR_irq。
 *   对 IRQ 而言 LR_irq 指向"被中断指令的下一条",所以要先减 4 才是
 *   真正被中断的指令地址 —— 少了这一步,返回后会跳过一条指令。
 *
 *   `movs pc, lr` 会在跳转的同时把 SPSR 拷回 CPSR,
 *   这正是异常返回的标准写法(等价于 x86 的 iretq)。
 *
 * 当前在 IRQ 模式自己的栈上保存现场,不支持嵌套。
 * 中断在 GIC 里已按优先级串行处理,等有了线程与优先级需求再改成
 * 切到 SVC 栈 + srsdb/rfeia 的完整形式。
 */
    .global _vec_irq
_vec_irq:
    sub     lr, lr, #4
    push    {r0-r12, lr}            /* 14 个字 = 56 字节的现场帧 */
    mov     r0, sp                  /* 帧指针作为第一个参数传给 C */
    bl      c_irq_handler
    pop     {r0-r12, lr}
    movs    pc, lr

/*
 * FIQ:本阶段不使用(所有中断都走 IRQ)。
 */
    .global _vec_fiq
_vec_fiq:
1:  wfe
    b       1b

/* ------------------------------------------------------------------ */
/* 安装向量表                                                          */
/* ------------------------------------------------------------------ */

/*
 * 把 _vectors 的地址写进 VBAR。
 * 必须在任何异常可能发生之前调用 —— 也就是 GIC 使能中断之前。
 */
    .global vectors_install
    .type   vectors_install, %function
vectors_install:
    ldr     r0, =_vectors
    mcr     p15, 0, r0, c12, c0, 0  /* VBAR = r0 */
    dsb
    isb
    bx      lr
    .size   vectors_install, . - vectors_install
