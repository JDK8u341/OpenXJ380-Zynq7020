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

/* ==================================================================== */
/* 陷阱式让出(M4-9)                                                    */
/* ==================================================================== */
/*
 * void arch_svc_yield(void)
 *
 * 一条 `svc`,然后回到调用者。
 *
 * ====================================================================
 * ★ `svc` 会把 LR 覆盖掉 —— 所以返回地址必须先挪个地方 ★
 * ====================================================================
 *
 * 第一版写的是:
 *
 *     arch_svc_yield:
 *         svc     #ARM_SVC_YIELD
 *         bx      lr              ← ★★ 错在这里 ★★
 *
 * 看起来天经地义,实际上 `bx lr` 会**跳到自己**:
 *
 *     `svc` 按架构做三件事:SPSR_svc <- CPSR、**LR_svc <- 下一条指令的地址**、
 *     切模式。也就是说 svc 一执行完,LR 里装的已经不是"调用者的返回地址",
 *     而是**下面那条 `bx lr` 自己的地址** —— 于是 `bx lr` 无条件跳回自己,
 *     变成一条不动声色的死循环。
 *
 * 上板症状:两个让出线程各输出一次计数就再也不动了(它们卡在这条死循环里),
 * 于是谁也到不了 `sched_park_self()`,就绪队列永远非空、idle 永远挑不到,
 * 启动流程再也回不来 —— **整块板子看起来像"卡在自检中间"**,而 tick 还在跑。
 * 从串口上完全看不出跟 `svc` 有关。
 *
 * ⇒ 正确做法:进 svc **之前**把 LR 挪到一个"帧会替我们保存和恢复"的寄存器里。
 *   r12(ip)正合适:AAPCS 里它是 caller-saved 的临时寄存器,谁都不能指望
 *   它跨调用保持不变;而**异常帧会把 r0-r12 全部存下来再全部还回去**
 *   (EXC_FRAME_ENTER 的 `push {r0-r12}` / EXC_FRAME_LEAVE 的 `ldmia`),
 *   所以放在 r12 里的东西恰好能穿过这次陷阱 —— 无论中间有没有发生切换。
 *
 * 两条路都对得上:
 *   - **没切换**:`_vec_svc` 原样返回传进来的帧,`ldmia` 把 r12 还回来 → `bx r12`;
 *   - **切换了**:现场被收进 ctx,将来恢复时同一句 `ldmia` 把同一个 r12 还回来
 *     → `bx r12` 落回 `sched_yield` 里 `bl` 的下一条。
 *
 * ⚠ 还有一个连带的后果值得写下来:这一次 `EXC_FRAME_ENTER` 的 `push {lr}`
 *   存进 `svc_lr` 槽的是**svc 之后的 LR**(也就是 `bx r12` 的地址),
 *   不是调用者的 LR。对 IRQ 来说那个槽装的是"被中断函数的活返回地址",
 *   必须原样还回去;而对 SVC 陷阱来说,它等于 `ret`,还回去也无害 ——
 *   前提正是这里的返回**不依赖 LR**。所以本函数用 r12,不用 lr。
 *
 * ⚠ 也不能把返回地址压栈(`push {lr}` / `pop {pc}`):这一段的全部意义就是
 *   **让"让出"与"被抢占"走同一条异常帧路径**。绕过帧去动栈,就回到了
 *   M4-8 那套"自己实现一套切换"的老问题上。
 *   (源 OS 的做法 `__asm__ volatile("int %0" ::"i"(32))` 之所以没这个麻烦,
 *    是因为 x86 的 `int n` **不碰** 任何通用寄存器。ARM 的 `svc` 碰 LR。)
 *
 * ====================================================================
 * 为什么是 SVC 而不是 GIC 的 SGI
 * ====================================================================
 *
 * x86 的 `int n` **不受 IF 屏蔽**;而 ARM 上"软件产生中断"只能靠 GIC 的 SGI,
 * 那是一条 IRQ —— CPSR.I=1 时会被挂起而不是立刻送达。差别是有后果的:
 * `scheduler_sleep_ns()` 是 `do { scheduler_yield(); } while (status == WAIT);`,
 * 若在关中断的上下文里调用它,SGI 版本会**空转不切换**,而 `int n` 版本照常进调度器。
 *
 * ⇒ SVC 是 ARM 上"不受 I/F 屏蔽的软件陷阱",与 `int n` 语义对齐。
 *   完整理由见 arch/taskctx_asm.h 的 ARM_SVC_YIELD。
 *
 * ⚠ 这一条**不能被编译器优化掉或内联**:它必须是一次真的异常入口,
 *   否则帧路径(以及"现场收进 ctx")就无从谈起。
 *   放在独立的 .S 文件里、由 C 以函数调用触达,是保证这件事的最简做法。
 */
    .global arch_svc_yield
    .type   arch_svc_yield, %function
/*
 * ⚠ `.section` 这一句不是装饰,是必需的。
 *
 * 本文件上面为了 `g_ctx_skip_sp` 切到了 `.data`,而 GNU as 的当前段
 * **一直保持到最后** —— 于是第一版把这段代码写在文件末尾时,它被汇编进了
 * **`.data`**(`nm` 显示 `D arch_svc_yield`),也就是一个不可执行、
 * 而且可写的段。
 *
 * 后果取决于该段的 XN 位:设了就是 Prefetch Abort,没设就是"悄悄能跑"——
 * 后者更糟,因为一个"能被写坏的函数"在正常测试里完全看不出来。
 * 所以显式声明段,不依赖"上一次 .section 是什么"。
 */
    .section .text.context, "ax"
arch_svc_yield:
    mov     r12, lr                     /* ★ 抢在 svc 覆盖 LR 之前 ★ */
    svc     #ARM_SVC_YIELD
    bx      r12                         /* r12 由异常帧保存/恢复,穿过陷阱 */
    .size   arch_svc_yield, . - arch_svc_yield

/* ==================================================================== */
/* VFP 上下文(M4-9.5)                                                  */
/* ==================================================================== */
/*
 * ← x86 侧的 `save_fpu_context()` / `restore_fpu_context()`
 *   (`include/cpu/fpu.h`),每次 `change_proccess` 都做(`scheduler.cpp:121-122`)。
 *
 * ★ 这一段是**补账**,不是新功能 ★
 *
 * README 的 M4-6 那节早就写明"M4-7 的切换点调用",而实现是零 ——
 * 全树搜 `vstmia|vldmia|vmrs|vmsr` 一条都没有,`TCB.vfp[64]`/`fpscr`
 * 是纯粹的**死字段**。M4-9 之前切换罕见,暴露面小;M4-9 之后是每 4ms
 * 一次的非自愿抢占 —— 任何线程里一个 `double` 就会被别的线程悄悄改掉,
 * 而症状是**算出错的数,不是崩溃**。
 *
 * ## 为什么要 .fpu vfpv3
 *
 * `.arch armv7-a` 隐含的是 VFPv3-**D16**(只有 d0-d15)。本内核用
 * `-mfpu=vfpv3`(32 个双字寄存器),要访问 d16-d31 必须显式声明,
 * 否则汇编器会直接拒绝那两条 `vstmia`。C 侧是编译器自己处理的,
 * 所以这个坑只有手写汇编会碰到。
 *
 * ## 对齐
 *
 * `vstmia`/`vldmia` 要求地址**4 字节对齐**。`TCB.vfp[64]` 在结构体里
 * 是 8 字节对齐的(TCB 的自然对齐是 8),所以天然满足。
 *
 * ## 寄存器约定
 *
 * 四个函数都只用 r0/r1/r2(r2 是 caller-saved 的临时寄存器),
 * 不动 r4-r11,也不碰条件码之外的状态 —— 于是它们可以在 `sched_tick`
 * 的中途安全调用。
 */
    .fpu    vfpv3

/*
 * void arch_vfp_save(u32 *d32, u32 *fpscr_out)
 *   d32[0..63] <- d0..d31   (32 个双字 = 64 个字 = 256 字节)
 *   *fpscr_out <- FPSCR
 *
 * ⚠ 一次 `vstmia` 最多搬 16 个双字(imm8 以"字"计数,上限 32 字),
 *   所以 d0-d31 要两条。
 */
    .global arch_vfp_save
    .type   arch_vfp_save, %function
arch_vfp_save:
    vstmia  r0!, {d0-d15}
    vstmia  r0!, {d16-d31}
    vmrs    r2, fpscr
    str     r2, [r1]
    bx      lr
    .size   arch_vfp_save, . - arch_vfp_save

/*
 * void arch_vfp_restore(const u32 *d32, u32 fpscr)
 *   d0..d31 <- d32[0..63]
 *   FPSCR   <- fpscr
 */
    .global arch_vfp_restore
    .type   arch_vfp_restore, %function
arch_vfp_restore:
    vldmia  r0!, {d0-d15}
    vldmia  r0!, {d16-d31}
    vmsr    fpscr, r1
    bx      lr
    .size   arch_vfp_restore, . - arch_vfp_restore

/*
 * FPSCR 的舍入模式(bit[23:22],RMode)。**只给自检用。**
 *
 * 为什么单独挑这两位来验:FPSCR 的绝大多数位是**累积状态标志**
 * (IOC/DZC/OFC/UFC/IXC),任何一次浮点运算都可能置位 —— 拿它们做
 * "我的值还在吗"的判据会误判。而 RMode 只有软件会改,是干净的地标。
 */
    .global arch_vfp_set_rmode
    .type   arch_vfp_set_rmode, %function
arch_vfp_set_rmode:
    mov     r2, #3
    vmrs    r1, fpscr
    bic     r1, r1, r2, lsl #22
    orr     r1, r1, r0, lsl #22
    vmsr    fpscr, r1
    bx      lr
    .size   arch_vfp_set_rmode, . - arch_vfp_set_rmode

    .global arch_vfp_get_rmode
    .type   arch_vfp_get_rmode, %function
arch_vfp_get_rmode:
    vmrs    r0, fpscr
    lsr     r0, r0, #22
    and     r0, r0, #3
    bx      lr
    .size   arch_vfp_get_rmode, . - arch_vfp_get_rmode

/* ==================================================================== */
/* ★ 切换浮点现场 —— 必须在 C 调用链**之外** ★(M4-9.5)                */
/* ==================================================================== */
/*
 * ★★ 为什么不放在 `sched_tick` 里 ★★
 *
 * 直觉做法是在 `sched_tick` 的搬帧那一段调 `arch_vfp_save/restore`。
 * **不行**,而且理由是实测出来的:
 *
 * 恢复完 `next` 的 d0-d31 之后,还要经过 `sched_tick` 的收尾、
 * `c_irq_handler` 的收尾、以及各自函数序言/尾声里可能存在的
 * `vpush/vpop {d8-d15}`。**只要那条链上任何一处碰了浮点寄存器,
 * 刚恢复的现场就被踩掉了。**
 *
 * 而"这条链会不会碰浮点"是**编译器说了算**的 —— 本内核里 GCC 已经在用
 * `vldr d16,[pc]; vstr d16,[rN]` 这个 **64 位清零惯用法**
 * (`palloc_selftest` / `percpu_table_reset` / `sched_entity_init` /
 *  `smp_release_cpu1` 四处),而 `sched_tick` 里那句
 * `pc->scheduler_ticks = 0u` 正是"64 位存零",今天编成两条整数 store,
 * 换个版本/改一行就可能变成上面那个惯用法。
 *
 * ⇒ 所以存与取都放进汇编,并且**卡在 C 的两端**:
 *
 *     _vec_irq:
 *         EXC_FRAME_ENTER …          @ 帧已建好
 *         bl  arch_vfp_save_current  @ ★ 任何 C 之前 ★
 *         mov r0, sp
 *         bl  c_irq_handler          @ 这一整段爱怎么用 VFP 就怎么用
 *         mov sp, r0
 *         bl  arch_vfp_restore_current  @ ★ 所有 C 返回之后 ★
 *         EXC_FRAME_LEAVE            @ 之后只剩纯整数指令
 *
 * 于是判据里**不再包含任何"编译器不会…"的前提**。
 *
 * ## 那两个指针从哪来
 *
 * 汇编不去算 TCB 的偏移(那要求 TCB 布局宿主与目标一致,而它有指针字段),
 * 而是读**每核结构**里的 `cur_vfp_d` / `cur_vfp_f` —— 那两个字段由
 * `sched_set_current()` 用 C 算好写进去。每核结构是"全 u32 字段"的
 * (percpu.h 有断言),所以它的偏移在宿主与目标上一致,汇编可以放心用。
 *
 * ## 为什么两个函数都要查 cpu_id
 *
 * 目前只有 CPU0 参与调度。CPU1 的 1kHz 定时器同样会走 `_vec_irq`,
 * 如果它去消费 CPU0 布下的状态,就会把 CPU0 的现场搬到 CPU1 上 ——
 * 两个核同时用一份 d0-d31。
 *
 * ## 为什么用 `ldmia` 而不是 `vldmia`
 *
 * 这里要读的是**每核结构里的普通内存**(不是 VFP 寄存器),所以是整数
 * `ldr`。下面 `vldmia`/`vstmia` 才是真的搬 d0-d31。
 */
    .global arch_vfp_save_current
    .type   arch_vfp_save_current, %function
arch_vfp_save_current:
    ldr     r3, =g_vfp_skip
    ldr     r3, [r3]
    cmp     r3, #0
    bxne    lr                          /* 破坏性 A/B:跳过 */

    mrc     p15, 0, r0, c13, c0, 4      /* r0 = TPIDRPRW = 本核 percpu_t* */
    cmp     r0, #0
    bxeq    lr
    ldr     r1, [r0, #ARM_PERCPU_OFF_CPU_ID]
    cmp     r1, #0
    bxne    lr                          /* 只有 CPU0 参与调度 */
    ldr     r0, [r0, #ARM_PERCPU_OFF_CUR_VFP_D]
    cmp     r0, #0
    bxeq    lr                          /* 还没有 current,或已声明不处理 */

    ldr     r1, =g_vfp_save_f
    ldr     r1, [r1]                    /* r1 = &cur->fpscr */
    vstmia  r0!, {d0-d15}
    vstmia  r0!, {d16-d31}
    vmrs    r2, fpscr
    str     r2, [r1]
    bx      lr
    .size   arch_vfp_save_current, . - arch_vfp_save_current

    .global arch_vfp_restore_current
    .type   arch_vfp_restore_current, %function
arch_vfp_restore_current:
    ldr     r3, =g_vfp_skip
    ldr     r3, [r3]
    cmp     r3, #0
    bxne    lr

    mrc     p15, 0, r0, c13, c0, 4
    cmp     r0, #0
    bxeq    lr
    ldr     r1, [r0, #ARM_PERCPU_OFF_CPU_ID]
    cmp     r1, #0
    bxne    lr
    ldr     r0, [r0, #ARM_PERCPU_OFF_CUR_VFP_D]
    cmp     r0, #0
    bxeq    lr

    ldr     r1, =g_vfp_save_f
    ldr     r1, [r1]
    ldr     r1, [r1]                    /* r1 = fpscr 的值 */
    vldmia  r0!, {d0-d15}
    vldmia  r0!, {d16-d31}
    vmsr    fpscr, r1
    bx      lr
    .size   arch_vfp_restore_current, . - arch_vfp_restore_current

/*
 * C 侧把"当前线程的浮点现场在哪"写进每核结构,汇编读的是同一对地址。
 *
 * `g_vfp_save_f` 是给 C 用的便利变量:`sched_set_current()` 一次写两个
 * 指针,而 `_vec_irq` 里那两条 `bl` 之间**不会有别的写者**
 * (中断全程 I=1,而且只有 CPU0 走这条路),所以一个全局量够了。
 * 换成两个每核字段也可以,但那就多两个偏移要维护、换不来什么。
 *
 * ⚠ 它不是"第二份真相":真值在 `percpu_t.cur_vfp_d`(汇编直接读),
 *   这里只放同一对象的 `&fpscr` 一半,而且与前者同生同死
 *   (`sched_set_current()` 一起写、一起清)。
 */
    .global g_vfp_save_f
    .data
    .align  2
g_vfp_save_f:
    .word   0

/*
 * ★ 破坏性 A/B 的开关:置 1 时**完全跳过**浮点现场的存与取 ★
 *
 * 定义在汇编里(而不是像 `g_reloc_skip` 那样在 C 里),是因为它的读者是
 * 上面两个汇编函数 —— 它们必须在 C 调用链之外,"先调一个 C 函数问一句
 * 跳不跳"就等于又把 C 拉回了窗口里。C 侧用 `extern` 声明它来置位。
 */
    .global g_vfp_skip
    .data
    .align  2
g_vfp_skip:
    .word   0
