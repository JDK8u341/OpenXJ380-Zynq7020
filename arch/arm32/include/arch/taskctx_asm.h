#pragma once

/*
 * 寄存器帧的**偏移宏** —— 汇编与 C 共用的唯一一份定义。
 *
 * ====================================================================
 * 为什么单独一个头文件
 * ====================================================================
 *
 * 本文件只允许出现**预处理指令**(#define),不允许出现任何 C 语法 ——
 * 因为它会被 `boot/vectors.S` 用 cpp 直接包含。C 那边的结构体定义、
 * `_Static_assert`、内联函数都在 `arch/taskctx.h` 里,那个文件
 * **汇编不能包含**(`typedef struct` 会让 as 报 bad instruction)。
 *
 * 分层的好处不只是"编得过":
 *
 *   - 汇编用的偏移**只有一个来源**,不在 `.S` 里写裸数字;
 *   - `taskctx.h` 里的 `_Static_assert` 会把 C 结构体的 `sizeof`/布局
 *     与这些宏对上,于是**改结构体而忘了改汇编也会在编译期报错**。
 *
 * ★ 这正是 x86 侧缺的那一环 ★
 *   `kernel/intr/handler.S:29-31` 把 PROCESSOR_INFO 的三个偏移
 *   (0x4c0 / 0x4e0 / 0x4e8)硬写在汇编里,**一个 static_assert 都没有**,
 *   全靠算术偶然对上 —— 改一次字段顺序就静默错位。
 *   见 docs/ZYNQ7020_PORT_PLAN.md §4.7.6。
 *
 * ====================================================================
 * 帧布局速查
 * ====================================================================
 *
 *   异常帧 arm_exc_frame_t(16 字 = 64 字节)
 *     0x00  r[0..12]   r0..r12
 *     0x34  ret        异常返回地址(`movs pc, ret`)
 *     0x38  pc         出错/被中断的**指令地址**(仅诊断)
 *     0x3C  spsr       被中断时的 CPSR
 *
 *   任务上下文 arm_task_ctx_t(17 字 = 68 字节)
 *     0x00  r[0..12]   r0..r12
 *     0x34  sp
 *     0x38  lr
 *     0x3C  pc
 *     0x40  cpsr
 *
 * 实测的 LR 偏移表(本板 XC7Z020 / Cortex-A9,由 tmp-test/exc_frame_probe.py 测出)
 * 写在 `arch/taskctx.h` 顶部的注释里 —— 那里能写中文说明,这里只放数字。
 */

/*
 * `offsetof` 的替代。freestanding 下没有 <stddef.h>,而 C11 的经典写法
 * ((type*)0)->member 是"未定义但所有编译器都支持"的形式 ——
 * GCC/Clang 都给 `__builtin_offsetof`,直接用它,把"靠编译器"写在明处。
 *
 * 定义放在这里(而不是 taskctx.h)是因为 percpu.h 也要用,
 * 而 percpu.h 只包含本文件。它只是个 #define,汇编看到也不会用。
 */
#ifndef offsetof_arm
#    define offsetof_arm(type, member) __builtin_offsetof(type, member)
#endif

/* ------------------------------------------------------------------ */
/* CPSR / SPSR 的位与模式                                              */
/* ------------------------------------------------------------------ */

#define ARM_CPSR_MODE_MASK 0x1Fu
#define ARM_MODE_USR       0x10u
#define ARM_MODE_FIQ       0x11u
#define ARM_MODE_IRQ       0x12u
#define ARM_MODE_SVC       0x13u
#define ARM_MODE_ABT       0x17u
#define ARM_MODE_UND       0x1Bu
#define ARM_MODE_SYS       0x1Fu

#define ARM_CPSR_T_BIT 0x20u /* Thumb 状态 */
#define ARM_CPSR_F_BIT 0x40u /* FIQ 屏蔽 */
#define ARM_CPSR_I_BIT 0x80u /* IRQ 屏蔽 */
#define ARM_CPSR_A_BIT 0x100u

/*
 * 从**入口 LR** 到两个不同目标各要减多少。实测表在 arch/taskctx.h 顶部。
 *
 * 分两步,不再是一个数:
 *
 *   RET_FIX  入口 LR -> **首选返回地址**(`movs pc, lr` 要跳的地方)
 *   PC_FIX   首选返回地址 -> **出错/被中断的那条指令**(只用于诊断)
 *
 * 于是 pc 有没有可能与 ret 不同,一眼可见:
 *   SVC   RET_FIX 0  PC_FIX 4   ← 不同:返回是 SVC 的下一条,pc 是 SVC 自己
 *   IRQ   RET_FIX 4  PC_FIX 4   ← 不同:返回是下一条,pc 是被中断的那条
 *   UND   RET_FIX 4  PC_FIX 0   ← 相同
 *   PABT  RET_FIX 4  PC_FIX 0   ← 相同
 *   DABT  RET_FIX 8  PC_FIX 0   ← 相同
 *
 * ★ 旧代码把这两件事当成一个值(统一的 LR-4),于是 SVC 与 Data Abort
 *   打印的 pc 各偏 4 字节。M4-6 用实测把它拆开了。
 */
#define ARM_EXC_RET_FIX_SVC  0u
#define ARM_EXC_RET_FIX_IRQ  4u
#define ARM_EXC_RET_FIX_UND  4u
#define ARM_EXC_RET_FIX_PABT 4u
#define ARM_EXC_RET_FIX_DABT 8u

#define ARM_EXC_PC_FIX_SVC   4u
#define ARM_EXC_PC_FIX_IRQ   4u
#define ARM_EXC_PC_FIX_UND   0u
#define ARM_EXC_PC_FIX_PABT  0u
#define ARM_EXC_PC_FIX_DABT  0u

/*
 * "返回"那一路。只有 SVC 与 IRQ 会返回,所以只有这两个有值:
 *   SVC  LR_svc = SVC 指令 + 4,首选返回地址就是 LR 本身 → 减 0
 *   IRQ  LR_irq = 被中断指令 + 8,首选返回地址是下一条     → 减 4
 * 其余三个(Undefined / Prefetch / Data Abort)处理完就停机,不返回。
 *
 * ★ 名字必须给全,别在 vectors.S 里写裸 0 ★
 *   裸 0 看不出"这里刻意不减",而它和 pc 那一路的 4 正好构成
 *   "ret 与 pc 不是同一个值"这件事 —— 那正是旧代码出错的地方。
 */
#define ARM_EXC_LR_TO_RET_SVC   0u
#define ARM_EXC_LR_TO_RET_IRQ   4u

/* ------------------------------------------------------------------ */
/* 帧一:异常帧的偏移                                                  */
/* ------------------------------------------------------------------ */

/*
 * 布局由 `srsdb` + 一串 push 的实际顺序决定,不是随便定的:
 *
 *   0x00  r0 .. r12       push {r0-r12}
 *   0x34  svc_lr          ★ 被中断者的 LR ★(见下面那一段,这是 M4-7 的关键)
 *   0x38  ret             返回地址(RFEIA 要用 [sp]=PC、[sp+4]=CPSR)
 *   0x3C  spsr            被中断时的 CPSR
 *   ---- 共 64 字节(8 的倍数,AAPCS 要求)
 *
 * ## ★ 为什么必须有 svc_lr ★
 *
 * 异常入口现在是**切到 SVC 模式**再调 C 处理函数的 —— 这样才能把现场帧建在
 * 任务的栈上(M4-7 的核心要求)。
 *
 * 但 `bl c_irq_handler` 会写 **LR_svc**,而 LR_svc 里装的正是**被中断的那段
 * 内核代码的返回地址**!(异常来自 SVC 模式时,硬件把返回地址放进 LR_irq,
 * LR_svc 完全没被动过 —— 它还是活的。)
 *
 * 旧实现把处理函数跑在 IRQ 模式,`bl` 用的是 banked 的 LR_irq,所以 LR_svc
 * 安然无恙;一旦搬到 SVC 模式,`bl` 就把它**踩掉了**。等被中断的函数
 * `bx lr` 时就跳到一个垃圾地址。
 *
 * 实测后果:内核卡在"中断刚打开"那一刻,而且**整个 PS 挂住** ——
 * 连 JTAG 的 DAP 都读不到(APB AP transaction error)。串口上什么都不打,
 * 因为执行已经飞到不存在的地址上去了。
 *
 * ⇒ 所以 LR 必须在切到 SVC 模式后、任何 `bl` 之前**立刻存进帧里**。
 *   这正是 Linux 的 `pt_regs` 里带 r13/r14 的原因。
 */
#define ARM_EXC_OFF_R0     0x00u
#define ARM_EXC_OFF_SVC_LR 0x34u
#define ARM_EXC_OFF_RET    0x38u
#define ARM_EXC_OFF_SPSR   0x3Cu
#define ARM_EXC_FRAME_WORDS (ARM_EXC_OFF_SPSR / 4u + 1u)
#define ARM_EXC_FRAME_BYTES (ARM_EXC_FRAME_WORDS * 4u)

/* 第 n 个通用寄存器槽(r0..r12)的偏移 */
#define ARM_EXC_OFF_R(n) (ARM_EXC_OFF_R0 + 4u * (n))

/* 返回(RFEIA sp!)要用的偏移:从帧底加到 ret 槽 */
#define ARM_EXC_OFF_RFE_BASE ARM_EXC_OFF_RET

/* 第 n 个通用寄存器槽(r0..r12)的偏移。照写会被 4 整除的算式,汇编也认 */
#define ARM_EXC_OFF_R(n) (ARM_EXC_OFF_R0 + 4u * (n))

/* ------------------------------------------------------------------ */
/* 帧二:任务上下文(调度帧)的偏移                                      */
/* ------------------------------------------------------------------ */

#define ARM_CTX_OFF_R0   0x00u
#define ARM_CTX_OFF_SP   0x34u
#define ARM_CTX_OFF_LR   0x38u
#define ARM_CTX_OFF_PC   0x3Cu
#define ARM_CTX_OFF_CPSR 0x40u
#define ARM_CTX_WORDS    (ARM_CTX_OFF_CPSR / 4u + 1u)
#define ARM_CTX_BYTES    (ARM_CTX_WORDS * 4u)

#define ARM_CTX_OFF_R(n) (ARM_CTX_OFF_R0 + 4u * (n))

/* ------------------------------------------------------------------ */
/* SVC 立即数(系统调用号的载波)                                       */
/* ------------------------------------------------------------------ */

#define ARM_SVC_IMM_MASK 0x00FFFFFFu

/* 帧布局自检专用:立即数取这个值时,SVC 处理函数会逐个核对 r0-r12 */
#define ARM_SVC_FRAME_CHECK 0x000A5A5u

/* 自检时使用的寄存器图案(第 n 个寄存器 = ARM_FRAME_CHECK_PATTERN ^ n) */
#define ARM_FRAME_CHECK_PATTERN 0x5A5A0000u

/* 自检结论里"槽不对"的编号:1..13 = 第几个寄存器,14 = SPSR,15 = ret */
#define ARM_FRAME_CHECK_BAD_SPSR 14
#define ARM_FRAME_CHECK_BAD_RET  15

/* ------------------------------------------------------------------ */
/* VFP 上下文                                                          */
/* ------------------------------------------------------------------ */

/*
 * d0-d31 共 32 个双字寄存器,每个 4 字 = 8 字节,所以 32*2 = 64 个字 = 256 字节。
 * 对齐取 32 字节:vstmia/vldmia 按 4 字一组访问。
 */
#define ARM_VFP_D_REGS 32u
#define ARM_VFP_WORDS  (ARM_VFP_D_REGS * 2u)
#define ARM_VFP_BYTES  (ARM_VFP_WORDS * 4u)

/* ------------------------------------------------------------------ */
/* ★ 汇编**不需要**知道 TCB 里 ctx 的偏移 ★                            */
/* ------------------------------------------------------------------ */

/*
 * 这里曾经有一组 `ARM_TCB_OFF_CTX` / `_KSTACK_TOP` / `_VFP` / `_FPSCR`,
 * 想让汇编按 TCB 的偏移直接访问 ctx —— 照 x86 的 `THREAD_SYSCALL_STACK=0xb48`
 * 那个样子。**做不下去,而且不该那么做:**
 *
 *   1. TCB 里有大量指针字段(`parent_group` / `argv` / `cwd` …)。
 *      宿主的指针是 8 字节、目标是 4 字节,于是**宿主机上算出来的偏移
 *      与目标板不同** —— 那几个 `_Static_assert` 在宿主上验的是错的布局。
 *      (本项目在 heap_block_t 的 sizeof 上已经踩过一次同样的坑。)
 *
 *   2. 更要紧的是:**汇编根本不需要它**。
 *
 *      x86 之所以必须知道 TCB 的偏移,是因为它的 `syscall` 指令不切栈,
 *      `handler.S:315` 得从 TCB 里读出 `syscall_stack` 装进 %rsp;
 *      per-CPU 的 `current_task` 也一样要按 `%gs:0x4c0` 取。
 *      这些偏移**必须**与 C 结构体严丝合缝,所以 x86 才那么怕它们漂移
 *      (而 `handler.S:29-31` 那三个还真就没有任何保护)。
 *
 *      ARM 没有这个问题:SP 是按模式 banked 的,硬件自动切;
 *      而 M4-7 的切换器**由 C 侧把 `&tcb->ctx` 传进来** ——
 *      跨语言边界上传的是**指针**,不是偏移。偏移只在 C 里算,
 *      编译器自己保证一致,不存在"两边对不上"的可能。
 *
 * ⇒ 于是汇编真正要钉的偏移只剩两组,而且**两组都只含 u32 字段**,
 *   宿主与目标布局一致,下面的断言在宿主机上验的就是目标板的真实偏移:
 *
 *     - `arm_task_ctx_t` 内部的偏移(taskctx.h 里断言)
 *     - `percpu_t.current_task`(percpu.h 里断言)
 *
 * 这比 x86 少一层,而且是**架构给的**,不是省略出来的。
 */

/* ------------------------------------------------------------------ */
/* 每核结构里汇编会用到的偏移                                          */
/* ------------------------------------------------------------------ */

/*
 * ARM 用 TPIDRPRW 拿 per-CPU 结构体指针(硬件 banked,不需要 swapgs ——
 * 见计划 §4.7.8)。汇编拿到指针之后要按偏移取 `current_task`。
 *
 * ★ 这正是 x86 缺保护的那一处 ★
 *   x86 的 handler.S:29 写死 CPU_CURRENT_TASK=0x4c0,
 *   而 include/smp/smp.h 里那个字段**一条 static_assert 都没有**。
 *   ARM 侧由 percpu.h 的 _Static_assert 钉住。
 */
#define ARM_PERCPU_OFF_CURRENT_TASK 0x28u
