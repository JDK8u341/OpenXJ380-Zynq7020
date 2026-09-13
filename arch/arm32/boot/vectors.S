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

#include <arch/taskctx_asm.h>

/* CPSR 模式位(与 taskctx.h 里的 ARM_MODE_* 同值,这里是给汇编直接用的别名) */
    .equ    MODE_SVC, ARM_MODE_SVC
    .equ    MODE_IRQ, ARM_MODE_IRQ
    .equ    I_BIT,    0x80
    .equ    F_BIT,    0x40

/*
 * ------------------------------------------------------------------
 * 现场帧的偏移**全部来自 arch/taskctx_asm.h**,不在这里写裸数字。
 * 那个头文件只含 #define,所以汇编能直接包含它;结构体定义与
 * _Static_assert 在 arch/taskctx.h 里,由 C 侧把两边的表述钉在一起。
 *
 * 为什么值得多一层:c x86 侧的 kernel/intr/handler.S:29-31 把
 * PROCESSOR_INFO 的三个偏移硬写在汇编里,**一个 static_assert 都没有**,
 * 全靠算术偶然对上 —— 改一次结构体字段顺序就静默错位。
 * 这里让汇编与 C 共用同一份宏,taskctx.h 里的 _Static_assert 于是
 * 同时保护了汇编那一侧。见 docs/ZYNQ7020_PORT_PLAN.md §4.7.6。
 * ------------------------------------------------------------------
 */

/*
 * 建帧的公共宏。
 *
 * 入口时硬件已经:
 *   - 把返回地址放进 LR_<mode>
 *   - 把被中断的 CPSR 放进 SPSR_<mode>
 *   - 切到本异常模式(于是 SP 是本模式 banked 的栈)
 *
 * 本宏做三件事:算"返回地址"、算"出错/被中断的指令地址"、存下 SPSR。
 *
 * ⚠ 两者**必须分开**,这是实测的结论(见 taskctx.h 顶部的表):
 *     返回   要的是"首选返回地址"      IRQ = LR-4,SVC = LR
 *     诊断   要的是"出错的那条指令"    IRQ = LR-8,SVC = LR-4
 *   旧代码把它们当成同一个值,于是 SVC 与 Data Abort 打印的 pc 各偏 4 字节。
 *
 * @param lr_fix   从入口 LR 到**出错/被中断的指令**要减多少
 * @param ret_fix  从入口 LR 到**首选返回地址**要减多少
 */
    .macro  EXC_FRAME_ENTER lr_fix, ret_fix
    sub     sp, sp, #ARM_EXC_FRAME_BYTES
    stmia   sp, {r0-r12}                    /* r0..r12 -> ARM_EXC_OFF_R0 起 */
    .if \lr_fix
    sub     r0, lr, #\lr_fix
    .else
    mov     r0, lr
    .endif
    str     r0, [sp, #ARM_EXC_OFF_PC]
    .if \ret_fix
    sub     r0, lr, #\ret_fix
    .else
    mov     r0, lr
    .endif
    str     r0, [sp, #ARM_EXC_OFF_RET]
    mrs     r0, spsr
    str     r0, [sp, #ARM_EXC_OFF_SPSR]
    .endm

/* 拆帧并返回(r0 已被重新载入,所以不需要单独保存它) */
    .macro  EXC_FRAME_LEAVE
    ldr     lr, [sp, #ARM_EXC_OFF_RET]
    ldmia   sp, {r0-r12}
    add     sp, sp, #ARM_EXC_FRAME_BYTES
    movs    pc, lr
    .endm

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
 * 实测 LR_und = 未定义指令 + 4,所以"出错指令"是 LR-4。
 * 它不返回(返回也没有意义),所以 ret 与 pc 取同一个值即可。
 */
    .global _vec_undefined
_vec_undefined:
    EXC_FRAME_ENTER ARM_EXC_LR_TO_INSN_UND, ARM_EXC_LR_TO_INSN_UND
    mov     r0, sp
    bl      c_undef_handler
1:  wfe
    b       1b

/*
 * SVC:系统调用入口。M1 阶段还没有用户态,先落到诊断处理。
 *
 * 实测 LR_svc = SVC 指令 + 4,所以:
 *   返回    → LR 本身(不减)
 *   诊断 pc → LR-4,也就是**那条 SVC 指令**
 * 后者不是装饰:c_svc_handler 要靠它取出 svc 的立即数当系统调用号。
 */
    .global _vec_svc
_vec_svc:
    EXC_FRAME_ENTER ARM_EXC_LR_TO_INSN_SVC, ARM_EXC_LR_TO_RET_SVC
    mov     r0, sp
    bl      c_svc_handler
    EXC_FRAME_LEAVE

/*
 * Prefetch Abort:取指失败。
 *
 * 实测(选择器 5,跳进 XN 区域):入口 LR-4 = **取指失败的那个地址**,
 * 与 IFAR 相同。这正是"出错的那条指令"在取指路径上的含义 ——
 * 不是那条 `blx`,而是它想跳去的、取不到的地址。
 * 返回无意义(处理函数停机)。
 */
    .global _vec_prefetch_abort
_vec_prefetch_abort:
    EXC_FRAME_ENTER ARM_EXC_LR_TO_INSN_PABT, ARM_EXC_LR_TO_INSN_PABT
    mov     r0, sp
    bl      c_prefetch_abort_handler
1:  wfe
    b       1b

/*
 * Data Abort:访存失败。
 *
 * 实测(选择器 6,写进 guard 页):出错指令是 `str r2,[r3],#-4` @ 0x00105C34,
 * 而入口 LR-4 = 0x00105C38 = 那条指令的**下一条**。
 * 所以"出错指令"是 **LR-8** —— 这是本次实测改掉的一处(旧代码用 LR-4,
 * 打印出来的 pc 指向隔壁那条 `bne`,会把人引错方向)。
 */
    .global _vec_data_abort
_vec_data_abort:
    EXC_FRAME_ENTER ARM_EXC_LR_TO_INSN_DABT, ARM_EXC_LR_TO_INSN_DABT
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
 * 实测/推导:入口 LR_irq = 被中断指令 + 8,于是
 *   LR-4 = **首选返回地址**(被中断指令的下一条)—— 返回要用它;
 *   LR-8 = 被中断的那条指令 —— 只用于诊断。
 *
 * 旧代码用同一个 LR-4 既返回又打印,所以**返回是对的、打印是偏 4 的**。
 * 这次把两者分开:返回仍用 LR-4,p c 改成 LR-8。
 *
 * 时序要点:进入 IRQ 时硬件已经把 CPSR 存进 SPSR_irq,返回地址放在 LR_irq。
 * `movs pc, lr` 在跳转的同时把 SPSR 拷回 CPSR —— 等价于 x86 的 iretq。
 *
 * 当前在 IRQ 模式自己的栈上保存现场,不支持嵌套。
 * 中断在 GIC 里已按优先级串行处理。M4-7 会按计划改成
 * `srsdb sp!, #MODE_SVC` 落到**任务的栈**上,那时帧里才会有真正的 SP。
 */
    .global _vec_irq
_vec_irq:
    EXC_FRAME_ENTER ARM_EXC_LR_TO_INSN_IRQ, ARM_EXC_LR_TO_RET_IRQ
    mov     r0, sp                  /* 帧指针作为第一个参数传给 C */
    bl      c_irq_handler
    EXC_FRAME_LEAVE

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
