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
 * 从入口 LR 到两个不同目标的偏移 —— 见 taskctx.h 顶部的实测表。
 *
 *   返回(首选返回地址)   SVC = LR,IRQ = LR-4
 *   诊断(出错/被中断指令) SVC = LR-4,Data Abort = LR-8,IRQ = LR-8
 */
#define ARM_EXC_LR_TO_INSN_SVC  4u
#define ARM_EXC_LR_TO_INSN_UND  4u
#define ARM_EXC_LR_TO_INSN_PABT 4u
#define ARM_EXC_LR_TO_INSN_DABT 8u
#define ARM_EXC_LR_TO_INSN_IRQ  8u

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

#define ARM_EXC_OFF_R0   0x00u
#define ARM_EXC_OFF_RET  0x34u
#define ARM_EXC_OFF_PC   0x38u
#define ARM_EXC_OFF_SPSR 0x3Cu
#define ARM_EXC_FRAME_WORDS (ARM_EXC_OFF_SPSR / 4u + 1u)
#define ARM_EXC_FRAME_BYTES (ARM_EXC_FRAME_WORDS * 4u)

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
