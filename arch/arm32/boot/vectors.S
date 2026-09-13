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
 *
 * ====================================================================
 * ★ M4-7:现场帧建在**任务的栈**上 ★
 * ====================================================================
 *
 * 在这之前,每个异常都建在自己模式的栈上(SVC 用 SVC 栈、IRQ 用 IRQ 栈、
 * ABT 用 ABT 栈)。单线程时看不出问题,一旦有了任务就是**错的**:
 *
 *   两个任务会共用同一段异常栈 —— 一个任务被中断、帧还没收走就切到另一个
 *   任务,后者再被中断就会盖掉前者的现场。症状是高负载下随机踩栈,
 *   而"共用一个全局栈"这件事本身不会报任何错。
 *
 * 所以所有异常统一落到 **SVC 模式的栈**(也就是当前任务的栈)上:
 *
 *     srsdb  sp!, #MODE_SVC     @ 在**异常模式**下执行,把 SPSR/LR 压到 SVC 栈
 *     cpsid  if,  #MODE_SVC     @ 切到 SVC 模式,此后 sp 就是任务的栈
 *     sub    sp, sp, #4         @ pc 槽
 *     push   {r0-r12}
 *
 * `srsdb` 是架构为这件事准备的指令:SRS 把**当前模式**的 SPSR 与 LR 存到
 * **指定模式**的栈上 —— 也就是说在 IRQ 模式里就能把 IRQ 的返回信息写进
 * 任务的栈,不需要先切模式。
 *
 * ★ 为什么必须先 srsdb 再 cpsid,顺序不能反 ★
 *   反过来就变成"在 SVC 栈上执行 srsdb",而那时 SPSR_svc / LR_svc 是旧值 ——
 *   返回地址会被写成上一次异常留下的东西。这个错误不会立刻崩,
 *   只是偶尔跳到一个不相关的地址。
 *
 * ⚠ 帧总共 64 字节(8 的倍数):建好之后要 `bl` 到 C,AAPCS 要求 SP 8 字节对齐。
 *   少留那个 pc 槽就是 60 字节,SP 落到 4 mod 8 —— 后果不是编译错误,
 *   而是 C 里某条 VFP 指令炸 Undefined Instruction(本项目在 M4-2 踩过)。
 *
 * ⚠ 异常模式自己的栈(IRQ/FIQ/ABT/UND)现在**只用来过渡**:srsdb 之后立刻切走。
 *   留着它们是因为嵌套异常仍然需要一段可用的栈。
 */

    .syntax unified
    .arch armv7-a
    .arm

#include <arch/taskctx_asm.h>

/* CPSR 模式位(值与 taskctx_asm.h 的 ARM_MODE_* 相同,这里给汇编直接用的别名) */
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
 * 为什么值得多一层:x86 侧的 kernel/intr/handler.S:29-31 把
 * PROCESSOR_INFO 的三个偏移硬写在汇编里,**一个 static_assert 都没有**,
 * 全靠算术偶然对上 —— 改一次结构体字段顺序就静默错位。
 * 见 docs/ZYNQ7020_PORT_PLAN.md §4.7.6。
 * ------------------------------------------------------------------
 */

/*
 * 建帧。入口时硬件已经:
 *   - 把返回地址放进 LR_<mode>
 *   - 把被中断的 CPSR 放进 SPSR_<mode>
 *   - 切到本异常模式(于是 sp 是本模式 banked 的栈)
 *
 * 本宏把现场搬到 **SVC 模式的栈**(也就是当前任务的栈)上,
 * 并按最终布局重排。三步的顺序都是硬的,理由写在每一步旁边。
 *
 * @param ret_fix  从**入口 LR** 减到"首选返回地址"要减多少
 *                 (何时减、以及 pc 怎么来,见 taskctx_asm.h 的 RET_FIX/PC_FIX 表)
 */
    .macro  EXC_FRAME_ENTER ret_fix
    /* ---- 1. 在异常模式下,把 SPSR/LR 压到 SVC 栈 ---- */
    srsdb   sp!, #ARM_MODE_SVC              /* SP_svc -= 8;[sp]=SPSR,[sp+4]=LR */

    /*
     * ---- 2. 切到 SVC 模式并关中断 ----
     *
     * ⚠ 用 `msr cpsr_c, #imm` 而**不是** `cpsid if, #mode`:
     *   CPS 带模式操作数时,目标模式等于当前模式是 UNPREDICTABLE ——
     *   而 SVC 向量正是这种情况(异常入口本来就在 SVC 模式里)。
     *   `msr cpsr_c` 只是写 CPSR 的控制域,同模式彻底无害,
     *   而且 start.S 的 setup_mode_stacks 一直在用这个写法。
     */
    msr     cpsr_c, #(ARM_MODE_SVC | I_BIT | F_BIT)

    /*
     * ---- 3. ★★★ 立刻存下 LR ★★★ ----
     *
     * 这一刻 LR 是**被中断的那段内核代码的 LR**(LR_svc)。异常发生时硬件把
     * 返回地址放进了 LR_irq,LR_svc 完全没被动过 —— 它还是活的。
     *
     * 而下面 `bl c_xxx_handler` 会**踩掉 LR_svc**。旧实现把处理函数跑在
     * IRQ 模式(`bl` 用的是 banked 的 LR_irq),所以没有这个问题;
     * 一旦搬到 SVC 模式而不存它,就等于毁掉被中断函数的返回地址 ——
     * 它一 `bx lr` 就跳到垃圾地址。
     *
     * 实测代价:内核卡在"中断刚打开"那一刻,而且**整个 PS 挂住** ——
     * 连 JTAG 的 DAP 都读不到(APB AP transaction error),只能断电重启。
     * 串口上一个字都不打,因为执行已经飞到不存在的地址上。
     */
    push    {lr}
    push    {r0-r12}

    /*
     * ---- 4. 把 [SPSR, LR_<mode>] 重排成 RFEIA 要的顺序 ----
     *
     * 此时帧里(从低到高):r0..r12 | svc_lr | SPSR_<mode> | LR_<mode>
     * 目标布局:           r0..r12 | svc_lr | ret        | spsr
     * 也就是把最后两个字**对调**,顺便把 LR 修正成"首选返回地址"。
     *
     * `srsdb` 存进去的是**原始** LR,它不是返回地址 ——
     * 返回地址是"LR 减去一个取决于异常类型的偏移"。漏掉这一步的后果是
     * **每条中断跳过一条指令**(症状是随机的 Data Abort,DFAR 每次都不一样)。
     */
    ldr     r0, [sp, #ARM_EXC_OFF_SPSR]     /* r0 = SPSR_<mode> */
    ldr     r1, [sp, #ARM_EXC_OFF_RET]      /* r1 = 原始 LR_<mode> */
    .if \ret_fix
    sub     r1, r1, #\ret_fix
    .endif
    str     r1, [sp, #ARM_EXC_OFF_RET]      /* [0x38] = ret  */
    str     r0, [sp, #ARM_EXC_OFF_SPSR]     /* [0x3C] = spsr */
    .endm

/*
 * 返回。会返回的只有 SVC 与 IRQ(其余三个处理完就停机)。
 *
 * 用 **`rfeia sp!`**:PC ← [sp]、CPSR ← [sp+4]、SP += 8,一条指令里
 * 同时完成"跳转"和"恢复 CPSR"。
 *
 * ⚠ 为什么不用 `msr spsr_cxsf` + `movs pc, lr`(第一版就是这么写的):
 *   那样写要么在跳转前就把中断重新打开(中间那几条指令可以被中断打断,
 *   而帧还没收完),要么就得把返回地址塞进 LR —— 而 LR 里必须装着
 *   **被中断函数的 LR**,两者抢同一个寄存器。
 *   RFE 把 CPSR 的恢复和跳转做成原子的,这两个问题一起消失。
 */
    .macro  EXC_FRAME_LEAVE
    ldmia   sp, {r0-r12}                    /* 不带写回:sp 仍是帧基址 */
    ldr     lr, [sp, #ARM_EXC_OFF_SVC_LR]   /* ★ 还回被中断者的 LR ★ */
    add     sp, sp, #ARM_EXC_OFF_RFE_BASE   /* sp 指向 ret 槽 */
    rfeia   sp!                             /* PC<-ret, CPSR<-spsr, SP += 8 */
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
 * 它不返回(返回也没有意义),ret 与 pc 取同一个值。
 */
    .global _vec_undefined
_vec_undefined:
    EXC_FRAME_ENTER ARM_EXC_RET_FIX_UND
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
 * 后者不是装饰:c_svc_handler 要靠它取出 svc 的立即数当系统调用号 ——
 * 而取它的前提正是 pc 指对了地方。
 */
    .global _vec_svc
_vec_svc:
    EXC_FRAME_ENTER ARM_EXC_RET_FIX_SVC
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
    EXC_FRAME_ENTER ARM_EXC_RET_FIX_PABT
    mov     r0, sp
    bl      c_prefetch_abort_handler
1:  wfe
    b       1b

/*
 * Data Abort:访存失败。
 *
 * 实测(选择器 6,写进 guard 页):出错指令是 `str r2,[r3],#-4` @ 0x00105C34,
 * 而入口 LR-4 = 0x00105C38 = 那条指令的**下一条**。
 * 所以"出错指令"是 **LR-8** —— M4-6 改掉的那处。
 */
    .global _vec_data_abort
_vec_data_abort:
    EXC_FRAME_ENTER ARM_EXC_RET_FIX_DABT
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
 * 所以 RET_FIX = 4、PC_FIX = 4:先减 4 得到返回地址,再减 4 得到被中断的指令。
 *
 * ★ 帧建在**任务的栈**上(见文件顶部),这是 M4-7 的硬要求 ★
 *   帧落在任务自己的栈上,切换任务时它随任务一起被保存/恢复;
 *   在此之前它落在全局的 IRQ 栈上,两个任务会互相覆盖现场。
 */
    .global _vec_irq
_vec_irq:
    EXC_FRAME_ENTER ARM_EXC_RET_FIX_IRQ
    mov     r0, sp                  /* 帧指针作为第一个参数传给 C */
    bl      c_irq_handler
    /*
     * ★ 协议:返回值就是"要从哪个帧离开" ★
     *
     * 照抄 x86 `scheduler.cpp:68-69` 的 `call timer_handle; mov rsp, rax` ——
     * C 侧通过**返回一个帧指针**决定异常返回到哪里:
     *   - 不切换:原样返回传进来的那个帧(下面的 `mov sp, r0` 是空操作)
     *   - 切换:返回在**目标任务栈上新搭的帧** —— `rfeia` 于是落到新任务的
     *     PC/CPSR 上,SP 也变成它的
     *
     * 这正是 M4-7 把现场帧建在**任务的栈**上的用处:帧在谁的栈上,谁就被恢复。
     */
    mov     sp, r0
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
