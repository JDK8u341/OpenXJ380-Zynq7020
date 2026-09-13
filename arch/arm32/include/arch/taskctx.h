#pragma once

/*
 * ARMv7-A 的寄存器帧 —— M4-6
 *
 * ====================================================================
 * 为什么要有这个文件(x86 侧的对照)
 * ====================================================================
 *
 * 移植计划 §4.7 调研出的一条最要紧的结论:**x86 有两套寄存器帧,不是一个** ——
 *
 *   | 帧       | 结构体                  | 布局由谁决定            | 用在哪          |
 *   |----------|-------------------------|-------------------------|-----------------|
 *   | 调度帧   | `registers_t`           | `scheduler.cpp:46-92`   | timer -> 切换   |
 *   | 异常帧   | `struct X64_REGS`       | `handler.S:5-28`        | 异常 + syscall  |
 *
 * 两者同为 192 字节,但 `rbx..rdi` 段与 `r8..r15` 段位置**互换**,
 * 指针不可互换。ARM 侧同样要区分,所以这里定义两张帧。
 *
 * ====================================================================
 * ★ 本板实测的 LR 偏移表(不是查文档抄的)★
 * ====================================================================
 *
 * ARM 的异常入口把返回地址放在 LR_<mode> 里,而**这个值离"出错的那条指令"
 * 有多远,每个异常类型都不同**。x86 上这件事由硬件压栈的 `rip` 一并解决,
 * ARM 上必须自己算 —— 算错的后果是"打印的 pc 指向隔壁那条指令"。
 *
 * 下表是 `tmp-test/exc_frame_probe.py` 在本板(XC7Z020,Cortex-A9)
 * 上实测出来的:注入点是 ELF 里已知编码的指令,地址由 objdump 读出,
 * 再与处理函数打印的值相减。
 *
 *   | 异常            | 入口 LR                | LR-4 是            | 要出错指令用 |
 *   |-----------------|------------------------|--------------------|--------------|
 *   | SVC             | SVC 指令 + 4           | **SVC 指令本身**   | LR-4         |
 *   | Undefined       | 未定义指令 + 4         | **未定义指令本身** | LR-4         |
 *   | Prefetch Abort  | 取指失败的地址 + 4     | **失败的取指地址** | LR-4         |
 *   | Data Abort      | 出错指令 + 8           | 出错指令的下一条   | **LR-8**     |
 *   | IRQ             | 被中断指令 + 8         | 首选返回地址       | **LR-8**     |
 *
 * 实测原始数据(可直接复核):
 *   - SVC        @ 0x00101904,处理函数打印 0x00101908        → 差 +4
 *   - Undefined  @ 0x001018C8,处理函数打印 0x001018C8        → 差 0(旧代码的 -4 恰好对)
 *   - Prefetch   @ `blx r0` 0x001018E8,打印 0xE0001000 = IFAR
 *                ⇒ 取指路径上"出错的那条指令"就是**取不到的那个地址本身**
 *   - Data Abort 的出错指令是 `str r2,[r3],#-4` @ 0x00105C34,
 *     旧代码打印 0x00105C38                                   → 差 +4(要 LR-8 才对)
 *
 * ★ 两条用途必须分开 ★
 *   - **返回**用"首选返回地址":SVC = LR,IRQ = LR-4(旧代码这两处**是对的**);
 *   - **诊断**用"出错/被中断的指令":见上表(旧代码在 Data Abort 与 SVC 上差 4)。
 *   旧代码把它们当成同一个值,所以 SVC 与 Data Abort 打印的 pc 各偏 4 字节。
 *
 * ====================================================================
 * ★ ARM 比 x86 干净的一点 ★
 * ====================================================================
 *
 * x86 的 `registers_t` 里 `rip/cs/rflags/rsp/ss` 是 CPU 进中断门时压的,
 * 而**压不压 rsp/ss 取决于是否发生了特权级变化** —— 帧的形状是条件性的。
 * ARM 没有这个分支:`SPSR`/`LR` 一定被存,banked SP 一定切换。
 *
 * 所以 ARM 的异常帧**无条件**包含 `spsr`(x86 的对应物是 `rflags`)。
 *
 * ⚠ `sp` **暂时不放进取异常帧**,理由是时序而不是图省事:
 *   现在所有异常都从 SVC 模式进,而异常入口已经切到了异常模式自己的栈上 ——
 *   被中断者的 SP(SVC 模式的 SP)在 IRQ/ABT/UND 模式里**读不到**。
 *   硬塞一个"有时有效"的字段比不放更危险。
 *   M4-7 的第一件事就是按计划把中断路径改成 `srsdb sp!, #MODE_SVC` 落到
 *   **任务的栈**上,那时 SP 由构造决定,自然就有了。
 *   (x86 那边之所以能有 `rsp` 槽,是因为它的硬件替他压了。)
 *
 * ⚠ 任务上下文(`arm_task_ctx_t`)**必须**有 `sp`:任务切换是协作式的,
 *   切换代码就跑在那个任务自己的栈上,它的 SP 就是任务的 SP,存下来即可。
 *
 * ====================================================================
 * 分层:偏移宏在 taskctx_asm.h
 * ====================================================================
 *
 * 本文件会被 `include/arch/irq.h` 与 C 代码包含,**汇编不能包含它**
 * (`typedef struct` 会让 as 报 bad instruction)。汇编用的偏移在
 * `arch/taskctx_asm.h`,那个文件只含 `#define`。
 *
 * 本文件的 `_Static_assert` 把两边的表述钉在一起 —— 这正是 x86 缺的那一环:
 * `kernel/intr/handler.S:29-31` 把 PROCESSOR_INFO 的三个偏移硬写在汇编里,
 * **一个 static_assert 都没有**,全靠算术偶然对上。见计划 §4.7.6。
 */

#include <arch/taskctx_asm.h>
#include <arch/types.h>

/* (offsetof_arm 现在定义在 taskctx_asm.h —— percpu.h 也要用它) */

/* ------------------------------------------------------------------ */
/* 帧一:异常帧                                                        */
/* ------------------------------------------------------------------ */

/*
 * 布局(全部 4 字节,共 16 字 = 64 字节),偏移见 taskctx_asm.h:
 *
 *   0x00  r[0..12]   r0..r12
 *   0x34  pc         出错/被中断的**指令地址** —— ★ 只用于诊断 ★
 *   0x38  spsr       被中断时的 CPSR
 *   0x3C  ret        异常返回地址 —— `movs pc, ret`
 *
 * ★ 顺序由 `srsdb` 的语义决定,不是随便定的 ★
 *   异常入口在异常模式下执行 `srsdb sp!, #MODE_SVC`,SRS 压出来的是
 *   [SPSR, LR](低地址是 SPSR),所以 spsr 在 ret 前面;pc 是汇编随后补上去的。
 *   详见 taskctx_asm.h。
 *
 * ⚠ `ret` 与 `pc` 是**两个不同的字段**,这是 M4-6 用板上实测改出来的:
 *   旧代码只有一个 `pc`,同时承担"返回"和"报告"两件事,而这两件事要的
 *   值不一样(见 taskctx_asm.h 的 RET_FIX / PC_FIX 表),于是至少有一件是错的 ——
 *   实测 SVC 与 Data Abort 打印的 pc 各偏 4 字节,指向了隔壁那条指令。
 */
typedef struct
{
    u32 r[13];  /* r0 .. r12 */
    u32 svc_lr; /* ★ 被中断者的 LR —— 必须在任何 bl 之前存下来 ★ */
    u32 ret;    /* 返回地址(RFEIA 要求 [sp]=PC)*/
    u32 spsr;   /* 被中断时的 CPSR([sp+4]=CPSR)*/
} arm_exc_frame_t;

/*
 * 编译期把布局钉死。汇编用 taskctx_asm.h 里的同一批宏,
 * 这里是它们的第二份表述 —— 任何一边改了而另一边没改,
 * 都在**编译期**报错,而不是等异常真的发生。
 */
_Static_assert(ARM_EXC_OFF_R0 == 0u, "异常帧必须从 r0 开始");
_Static_assert(ARM_EXC_OFF_SVC_LR == 13u * 4u, "r0-r12 之后必须紧跟 svc_lr");
_Static_assert(ARM_EXC_OFF_RET == ARM_EXC_OFF_SVC_LR + 4u, "ret 紧跟 svc_lr");
_Static_assert(ARM_EXC_OFF_SPSR == ARM_EXC_OFF_RET + 4u, "spsr 紧跟 ret(RFEIA 的 [sp+4])");
_Static_assert(ARM_EXC_FRAME_BYTES == 64u, "异常帧大小变了,vectors.S 的偏移也要改");
_Static_assert(ARM_EXC_FRAME_BYTES % 8u == 0u, "异常帧不是 8 的倍数,调用 C 时 SP 会不对齐");
_Static_assert(sizeof(arm_exc_frame_t) == ARM_EXC_FRAME_BYTES, "结构体与偏移宏不一致");
_Static_assert(offsetof_arm(arm_exc_frame_t, svc_lr) == ARM_EXC_OFF_SVC_LR, "svc_lr 偏移与汇编不一致");
_Static_assert(offsetof_arm(arm_exc_frame_t, ret) == ARM_EXC_OFF_RET, "ret 偏移与汇编不一致");
_Static_assert(offsetof_arm(arm_exc_frame_t, spsr) == ARM_EXC_OFF_SPSR, "spsr 偏移与汇编不一致");

/* ------------------------------------------------------------------ */
/* 帧二:任务上下文(调度帧)                                           */
/* ------------------------------------------------------------------ */

/*
 * x86 的对应物是 `TaskContext context0`(`include/task/pcb.h:61-82`,176 字节),
 * 它是"任务上下文"的规范存储,`change_proccess` 只搬这些字段。
 *
 * ARM 侧存 17 个字:
 *
 *   r0..r12  —— 13 个字。**全存**,而不是只存 AAPCS 的 callee-saved(r4-r11)。
 *              理由与 x86 相同:这张结构同时用于"新任务的初始上下文",
 *              而新任务的入口函数要从 r0-r3 取参数。
 *   sp       被挂起时的栈指针
 *   lr       被挂起时的返回地址
 *   pc       恢复后从这里继续
 *   cpsr     被挂起时的 CPSR(含模式位与 I/F)
 */
typedef struct
{
    u32 r[13]; /* r0 .. r12 */
    u32 sp;
    u32 lr;
    u32 pc;
    u32 cpsr;
} arm_task_ctx_t;

_Static_assert(ARM_CTX_OFF_SP == 13u * 4u, "r0-r12 之后必须紧跟 sp");
_Static_assert(ARM_CTX_OFF_LR == ARM_CTX_OFF_SP + 4u, "lr 紧跟 sp");
_Static_assert(ARM_CTX_OFF_PC == ARM_CTX_OFF_LR + 4u, "pc 紧跟 lr");
_Static_assert(ARM_CTX_OFF_CPSR == ARM_CTX_OFF_PC + 4u, "cpsr 紧跟 pc");
_Static_assert(ARM_CTX_BYTES == 68u, "任务上下文大小变了,汇编里的偏移也要改");
/*
 * boot/context.S 用 `ldr r12, [r1, r2, lsl #2]` 遍历 r[],这要求 r[] 在开头。
 * 那里的 `.error` 也钉了同一条;两边都写是有意的 ——
 * C 侧改了字段顺序时,报错的地方越靠近改动点越好。
 */
_Static_assert(ARM_CTX_OFF_R0 == 0u, "r[] 必须在任务上下文开头(context.S 的遍历假设)");
_Static_assert(sizeof(arm_task_ctx_t) == ARM_CTX_BYTES, "结构体与偏移宏不一致");
_Static_assert(offsetof_arm(arm_task_ctx_t, sp) == ARM_CTX_OFF_SP, "sp 偏移与汇编不一致");
_Static_assert(offsetof_arm(arm_task_ctx_t, pc) == ARM_CTX_OFF_PC, "pc 偏移与汇编不一致");
_Static_assert(offsetof_arm(arm_task_ctx_t, cpsr) == ARM_CTX_OFF_CPSR, "cpsr 偏移与汇编不一致");

/* ------------------------------------------------------------------ */
/* 纯函数                                                              */
/* ------------------------------------------------------------------ */

/*
 * 从一条 SVC 指令的编码里取出系统调用号。
 *
 * ARM 的 `svc #imm24` 把号放在**指令的低 24 位**里(x86 那边放在 `rax`)。
 *
 * ⚠ 这条要成立,前提是异常帧的 `pc` **真的指向那条 SVC 指令** ——
 *   实测表明旧代码打印的是 SVC 的下一条,按它去取指会取到随机的下一条指令
 *   当"系统调用号",而这个错误**不会报错**,只会调用到别的号上。
 *   宿主单测直接钉这一点。见 tests/test_arm32_taskctx.py。
 */
static inline u32 arm_svc_immediate(u32 insn)
{
    return insn & ARM_SVC_IMM_MASK;
}

/*
 * 出错/被中断的**指令地址** —— 只用于诊断,由 `ret` 算出来。
 *
 * 帧里不再单独存一个 pc 字段:64 字节的布局已经被 svc_lr 占满了,
 * 而"出错指令"本来就是 `ret` 减去一个**取决于异常类型**的常数
 * (见 taskctx_asm.h 的 PC_FIX 表)。把它做成一个有名字的纯函数,
 * 比多存一个字段再让两边同步更不容易错。
 */
static inline u32 arm_exc_pc(const arm_exc_frame_t *f, u32 pc_fix)
{
    return f->ret - pc_fix;
}

/* SPSR/CPSR 的模式位文本。纯函数,宿主可测 */
static inline const char *arm_mode_text(u32 cpsr)
{
    switch (cpsr & ARM_CPSR_MODE_MASK) {
    case ARM_MODE_USR: return "User";
    case ARM_MODE_FIQ: return "FIQ";
    case ARM_MODE_IRQ: return "IRQ";
    case ARM_MODE_SVC: return "Supervisor";
    case ARM_MODE_ABT: return "Abort";
    case ARM_MODE_UND: return "Undefined";
    case ARM_MODE_SYS: return "System";
    default:           return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/* CPU 相关的位(实现在 src/taskctx_hw.c,CP15/汇编)                    */
/* ------------------------------------------------------------------ */

/*
 * 读当前 SP —— 用于给 idle 线程造初始上下文。
 * x86 的对应物是 `get_rsp()`(`include/cpu/regio.h:19`)。
 * (CPSR 的读取在 arch/cpu.h 的 arch_read_cpsr(),不在这里重复声明。)
 */
u32 arch_read_sp(void);

/* ------------------------------------------------------------------ */
/* 汇编实现的布局自检(实现在 boot/context.S)                          */
/* ------------------------------------------------------------------ */

/*
 * 对一个 `arm_task_ctx_t` 做一次"按宏读出来核对"的往返:
 * C 侧先把 17 个字段写成 ARM_FRAME_CHECK_PATTERN + 下标 的图案。
 *
 * 存在的理由:`_Static_assert` 钉得住"C 结构体 == 宏",钉不住
 * "**汇编里用的是那个宏**" —— 手滑写错一个宏名,编译器不会有任何意见。
 * 见 boot/context.S 顶部。
 *
 * 返回:0 = 全对;1..13 = 第几个 r 槽不对;14 = sp;15 = lr;16 = pc;17 = cpsr。
 * **不会停机** —— 布局错了应该报 FAIL 后继续跑,而不是把内核弄停。
 */
u32 arch_ctx_layout_check(const arm_task_ctx_t *ctx);
