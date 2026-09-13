/*
 * 上下文相关的汇编入口 —— M4-6
 *
 * ====================================================================
 * 这个文件现在只有一件事:验证 `arm_task_ctx_t` 的**内部偏移**在汇编与 C
 * 之间一致
 * ====================================================================
 *
 * ⚠ 边界是"指向 ctx 的指针",**不是**"指向 TCB 的指针" ——
 *   理由写在 arch/taskctx_asm.h 里(TCB 有指针字段,宿主与目标布局不同;
 *   而汇编本来也不需要知道 ctx 在 TCB 里的位置)。
 *
 * 真正的上下文切换(`arch_ctx_save` / `arch_ctx_switch_to`)排在 M4-7 ——
 * 那时才有"切到谁"(就绪队列)这个东西可谈。现在先把**布局契约**立住,
 * 否则 M4-7 写的切换代码是在一个没验证过的偏移上建的。
 *
 * ====================================================================
 * 为什么 `_Static_assert` 还不够
 * ====================================================================
 *
 * `arch/tcb.h` 里的 `_Static_assert` 钉住的是"C 结构体的真实偏移 ==
 * `taskctx_asm.h` 里的宏"。它证明不了**汇编里用的是那个宏** ——
 * 如果这里手滑写成 `#ARM_CTX_OFF_R(3)` 而本该是 `#ARM_CTX_OFF_SP`,
 * 编译器不会有任何意见。
 *
 * 所以补一个**运行时**往返:链接期一切正常,但跑起来就会发现读到的东西
 * 与预期不符。这与 x86 那边"per-CPU 三个偏移硬编码、零保护、
 * 全靠算术偶然对上"形成对照(见计划 §4.7.6)。
 *
 * ====================================================================
 * 汇编侧的静态断言
 * ====================================================================
 *
 * GNU as 的 `.if` / `.error` 可以在汇编期报错。下面的几条钉住的是
 * **本文件的指令序列所依赖的前提** —— 这些前提在 C 侧是看不出来的。
 */

    .syntax unified
    .arch armv7-a
    .arm

#include <arch/taskctx_asm.h>

/*
 * 本文件用 `ldr r12, [r1, r2, lsl #2]` 遍历 ctx->r[],这要求
 * r[] 就在 ctx 的开头、而且逐字排列(4 字节步长)。
 * 一旦有人给 arm_task_ctx_t 前面加字段,这里必须同步 —— 汇编期就报错。
 */
.if ARM_CTX_OFF_R0 != 0
.error "ARM_CTX_OFF_R0 不为 0:context.S 的 r[] 遍历假设它在结构体开头"
.endif

.if ARM_CTX_OFF_R(1) != 4
.error "ARM_CTX_OFF_R(1) 不是 4:寄存器槽不是 4 字节步长"
.endif

    .section .text.context, "ax"

/*
 * u32 arch_ctx_layout_check(const arm_task_ctx_t *ctx)
 *
 * C 侧先把 ctx 的 17 个字段写成已知图案:
 *     r[i] = ARM_FRAME_CHECK_PATTERN + i     (i = 0..12)
 *     sp   = ARM_FRAME_CHECK_PATTERN + 100
 *     lr   = ARM_FRAME_CHECK_PATTERN + 101
 *     pc   = ARM_FRAME_CHECK_PATTERN + 102
 *     cpsr = ARM_FRAME_CHECK_PATTERN + 103
 *
 * 这里按**宏**把它们读回来核对。图案里带下标,所以"读到了别的槽"
 * 会被认出来,而不是碰巧相等。
 *
 * 返回:0 = 全对;1..13 = 第几个 r 槽不对;14 = sp;15 = lr;16 = pc;17 = cpsr。
 *
 * ⚠ 用返回码而不是"打印后停机":这是个自检,**它不该有能力把内核弄停** ——
 *   布局错了应该报 FAIL 然后继续跑,让人一次看到全部问题。
 */
    .global arch_ctx_layout_check
    .type   arch_ctx_layout_check, %function
arch_ctx_layout_check:
    mov     r1, r0                          /* r1 = ctx(由 C 侧传进来)*/
    ldr     r3, =ARM_FRAME_CHECK_PATTERN    /* 基准图案 */

    /* ---- r0..r12 ---- */
    mov     r2, #0
1:
    ldr     r12, [r1, r2, lsl #2]
    add     r0, r3, r2
    cmp     r12, r0
    bne     9f
    add     r2, r2, #1
    cmp     r2, #13
    blo     1b

    /* ---- sp ---- */
    ldr     r12, [r1, #ARM_CTX_OFF_SP]
    add     r0, r3, #100
    cmp     r12, r0
    bne     10f

    /* ---- lr ---- */
    ldr     r12, [r1, #ARM_CTX_OFF_LR]
    add     r0, r3, #101
    cmp     r12, r0
    bne     11f

    /* ---- pc ---- */
    ldr     r12, [r1, #ARM_CTX_OFF_PC]
    add     r0, r3, #102
    cmp     r12, r0
    bne     12f

    /* ---- cpsr ---- */
    ldr     r12, [r1, #ARM_CTX_OFF_CPSR]
    add     r0, r3, #103
    cmp     r12, r0
    bne     13f

    mov     r0, #0
    bx      lr

9:  add     r0, r2, #1                      /* 1..13:第几个寄存器槽 */
    bx      lr
10: mov     r0, #14                         /* sp */
    bx      lr
11: mov     r0, #15                         /* lr */
    bx      lr
12: mov     r0, #16                         /* pc */
    bx      lr
13: mov     r0, #17                         /* cpsr */
    bx      lr
    .size   arch_ctx_layout_check, . - arch_ctx_layout_check


/* ==================================================================== */
/* 协作式上下文切换(M4-7 后半段)                                        */
/* ==================================================================== */
/*
 * 与异常返回的分工 —— 这一条是设计决定,不是偷懒:
 *
 *   | 场景                | 谁负责保存/恢复 CPSR          |
 *   |---------------------|-------------------------------|
 *   | 异常返回(被抢占)| `rfeia sp!`,与跳转原子完成   |
 *   | 协作式切换(本文件)| **不碰 CPSR**                 |
 *
 * 协作式切换永远发生在内核态、SVC 模式里,双方模式位相同;而恢复 CPSR
 * 会**中途改变模式**,后面几条按 sp 寻址的指令立刻失去意义。
 * 所以 CPSR 只**存下来**(供初始上下文与诊断用),不在这里恢复。
 * 真正需要连 CPSR 一起换的路径是抢占(M4-9),那时走的是异常返回那条路。
 *
 * AAPCS 约定:r4-r11 是被调用者保存的,必须跨调用保持;r0-r3 与 r12 不用。
 * 所以本切换器恢复 r4-r11 —— 另外**额外恢复 r0**,理由见下面。
 */

    .section .text.context, "ax"

/*
 * u32 arch_ctx_save(arm_task_ctx_t *ctx)
 *
 * 把当前上下文存进 ctx,**首次返回 0**;之后若有别的上下文切回这里,
 * 会从本函数返回点继续,而返回值取自那个上下文里的 `r[0]`。
 *
 * 这就是 setjmp 的约定,也是"两个上下文来回切"能被验证的前提 ——
 * 调用方靠返回值区分"我是刚存完"还是"我被切回来了":
 *
 *     if (arch_ctx_save(&a) == 0) {
 *         a.r[0] = 1;                  // 告诉将来被切回来的自己:你是被切回来的
 *         arch_ctx_switch(&a, &b);     // 这一句之后不会立刻继续
 *     }
 *     // 到这里说明是从别处切回来的
 *
 * ⚠ 不碰 ctx->r[0]:它由调用方决定(见上)。也不恢复 CPSR(见文件顶部)。
 */
    .global arch_ctx_save
    .type   arch_ctx_save, %function
arch_ctx_save:
    add     r1, r0, #ARM_CTX_OFF_R(4)
    stmia   r1, {r4-r11}                    /* r4..r11 -> ARM_CTX_OFF_R(4..11) */
    str     sp,  [r0, #ARM_CTX_OFF_SP]
    str     lr,  [r0, #ARM_CTX_OFF_LR]
    str     lr,  [r0, #ARM_CTX_OFF_PC]      /* 恢复后从调用点之后继续 */
    mrs     r1, cpsr
    str     r1,  [r0, #ARM_CTX_OFF_CPSR]    /* 只存不恢复 —— 见文件顶部 */
    mov     r0, #0
    bx      lr
    .size   arch_ctx_save, . - arch_ctx_save

/*
 * void arch_ctx_switch(arm_task_ctx_t *from, arm_task_ctx_t *to)
 *
 * 保存当前上下文到 from,再从 to 恢复。**从 from 看,这一句之后不会立刻
 * 继续执行** —— 要等某个上下文切回 from 才继续。
 *
 * ⚠ `g_ctx_skip_sp` 是**破坏性 A/B 的对照组**:置 1 时故意不恢复 sp,
 *   于是被恢复的上下文会跑在**别人的栈**上。
 *   生产路径上它恒为 0;留着它是为了让"sp 恢复"这件事可以被证明是承重的
 *   (见 kmain 里的切换自检)。
 */
    .global arch_ctx_switch
    .type   arch_ctx_switch, %function
arch_ctx_switch:
    /* ---- 1. 保存 from ---- */
    add     r2, r0, #ARM_CTX_OFF_R(4)
    stmia   r2, {r4-r11}
    str     sp,  [r0, #ARM_CTX_OFF_SP]
    str     lr,  [r0, #ARM_CTX_OFF_LR]
    str     lr,  [r0, #ARM_CTX_OFF_PC]
    mrs     r2, cpsr
    str     r2,  [r0, #ARM_CTX_OFF_CPSR]

    /* ---- 2. 恢复 to ---- */
    ldr     r2,  [r1, #ARM_CTX_OFF_PC]      /* r2 = 跳转目标(r2 是 caller-saved)*/

    ldr     r3,  =g_ctx_skip_sp
    ldr     r3,  [r3]
    cmp     r3, #0
    ldreq   sp,  [r1, #ARM_CTX_OFF_SP]      /* ★ 换栈 —— 默认路径一定会做 ★ */

    ldr     lr,  [r1, #ARM_CTX_OFF_LR]
    add     r3,  r1, #ARM_CTX_OFF_R(4)
    ldmia   r3,  {r4-r11}
    ldr     r0,  [r1, #ARM_CTX_OFF_R(0)]    /* r0 最后恢复:见 arch_ctx_save 的约定 */
    bx      r2
    .size   arch_ctx_switch, . - arch_ctx_switch

/* 破坏性 A/B 的开关:0 = 正常恢复 sp(默认);1 = 故意不恢复 */
    .global g_ctx_skip_sp
    .data
    .align  2
g_ctx_skip_sp:
    .word   0
