#pragma once

/*
 * 中断子系统:异常向量 + GIC + INTID 处理表
 *
 * 对应 x86_64 侧的 kernel/intr/apic.cpp + kernel/pctable/idt.cpp。
 *
 * 与 x86 的三个关键差异(都记在这里,免得后人按 x86 的直觉改):
 *
 * 1. **x86 没有"中断注册 API"**。原实现里驱动自己调 init_idt_desc() 往 IDT
 *    写向量,IOAPIC 重定向是 apic.cpp 里硬编码的 4 行。这里改成显式的
 *    INTID -> handler 表,注册与使能分离。
 *
 * 2. **EOI 是"写回你收到的那个 INTID"**,不是写一个固定的寄存器。
 *    x86 的 send_eoi() 往 LAPIC 的 0xB0 写 0 即可;GIC 必须把
 *    ICCIAR 读到的 INTID 原样写进 ICCEOIR,否则中断不会被清除。
 *
 * 3. **优先级方向相反**。x86 里数值越大优先级越高;GIC 里
 *    **数值越小优先级越高**,0xFF 是最低。别照着 x86 的习惯填。
 */

#include <arch/types.h>

/* ------------------------------------------------------------------ */
/* 中断号(INTID)                                                       */
/* ------------------------------------------------------------------ */

/*
 * PL390 GIC 的 INTID 空间(与 x86 的 IRQ/GSI 概念对应):
 *   0..15   SGI  (软件生成,核间中断)
 *   16..31  PPI  (每 CPU 私有外设:定时器、看门狗)
 *   32..95  SPI  (共享外设:UART/GEM/SD/GPIO...)
 *   1020.. 特殊值(1023 = 虚假中断)
 */
#define GIC_INTID_SGI_FIRST 0u
#define GIC_INTID_PPI_FIRST 16u
#define GIC_INTID_SPI_FIRST 32u
#define GIC_INTID_MAX       95u
#define GIC_INTID_SPURIOUS  1023u

/* Cortex-A9 私有定时器(PPI 29)与全局定时器(PPI 30) */
#define GIC_INTID_A9_PRIVATE_TIMER 29u
#define GIC_INTID_A9_GLOBAL_TIMER  30u

/*
 * SGI(核间中断)相关的约定(AM3-5)。
 *
 * cpu_mask 是"目标核掩码",与 SPI 的 ITARGETSR 用的是同一套位:
 * bit0 = CPU0, bit1 = CPU1。
 */
#define GIC_SGI_TARGET_CPU0 0x01u
#define GIC_SGI_TARGET_CPU1 0x02u
#define GIC_SGI_TARGET_ALL  0x03u

/*
 * 本项目用 SGI 0 做通用通知(IPI)。
 * SGI 号只有 0..15 可用,而 0..15 在 GIC 里是**每核私有**的,
 * 不存在和别的驱动抢的问题。
 */
#define GIC_INTID_SMP_IPI 0u

/* ------------------------------------------------------------------ */
/* 异常现场帧                                                          */
/* ------------------------------------------------------------------ */

/*
 * 帧的布局与偏移宏都在 arch/taskctx.h —— 汇编(vectors.S)与 C 共用同一份宏,
 * 那里的 _Static_assert 因此同时保护了两侧。见该文件顶部的实测 LR 偏移表。
 *
 * ⚠ 这里只是把类型名转出来,**不要再定义一份布局**。
 */
#include <arch/taskctx.h>

/*
 * 兼容别名。旧的 `arm_irq_frame_t` 只有 `r[13] + pc`,既没有 SPSR,
 * 也无法区分"返回地址"与"出错指令" —— M4-6 换成了完整帧。
 * 保留别名只为少改一处引用,新代码请直接用 arm_exc_frame_t。
 */
typedef arm_exc_frame_t arm_irq_frame_t;

typedef void (*irq_handler_t)(u32 intid, void *arg);

/*
 * 异常帧布局的运行时自检结果(M4-6)。
 *
 * 由 fault_test 的选择器 9 触发,c_svc_handler 里逐个核对帧槽。
 *   0  = 全部一致
 *   1..13 = 第几个寄存器槽不对
 *   14 = SPSR 槽不对(读到的不是 SVC 模式)
 *   15 = ret 与 pc 的关系不对
 *   -1 = 还没跑过
 *
 * 为什么值得单独报一项:`_Static_assert` 只能钉住 C 侧的偏移宏,
 * 钉不住"汇编真的按这些宏存了"。见 arch/taskctx.h 顶部。
 */
int irq_svc_frame_check_result(void);

/*
 * 最近一次 SVC 的现场帧地址(M4-7)。
 *
 * 判据:它必须落在链接脚本给出的 **SVC 栈区**
 * (`__stack_svc_bottom` .. `__stack_svc_top`)里。
 * 异常帧建在任务的栈上是 M4-7 的核心要求 —— 否则两个任务会共用
 * 同一段全局异常栈,在高负载下互相覆盖现场,而那种错误不报任何东西。
 */
u32 irq_last_svc_frame_addr(void);

/*
 * IRQ 现场帧的结构违例计数(M4-7)。
 *
 * 判据是那条不变量:**IRQ 的 ret 必须正好比 pc 大 4**
 * (ret = 被中断指令的下一条,pc = 被中断的那条)。
 * 异常入口漏掉"按异常类型修正 LR"那一步时,两者会取到同一个原始值,
 * 这一条立刻失败。
 *
 * 为什么值得单独设一个计数器而不是只在自检里看一眼:
 * 这个 bug 的表现是"每条中断跳过一条指令",被跳过的是哪条不确定 ——
 * 于是症状每次都不一样(实测两次 Data Abort 的 DFAR 分别是 0x00 与
 * 0x0010AA26)。用"当前这一帧对不对"来判,1000 次/秒的检查才躲得过"偶尔"。
 *
 * 正常情况必须恒为 0。
 */
u32 irq_frame_violations(void);

/*
 * 现场帧**不是** 8 字节对齐的次数(只统计,不判失败)。
 *
 * 帧基址 = 异常入口那一刻的 SP - 64,而 AAPCS 只要求**调用点** 8 字节对齐,
 * 函数体内部允许 4 字节对齐 —— 所以 SP 合法地可以是 4 mod 8,
 * 于是帧合法地可以不是 8 对齐。实测来源:libgcc 的 Thumb 函数
 * `__udivmoddi4`(它的 `push {r4,r5,lr}` 是 12 字节)。
 *
 * 用途是**量规模**:如果这个数很大,说明"异常入口对齐帧 + 把原始 SP 存进帧"
 * 这件事值得做(要把帧从 16 字加宽到 18 字)。
 *
 * ⚠ 硬要求是 **4** 字节对齐(帧里全是 u32),那一条算在
 *   `irq_frame_violations()` 里。
 */
u32 irq_frame_unaligned8(void);

/* ------------------------------------------------------------------ */
/* 初始化                                                              */
/* ------------------------------------------------------------------ */

/* 安装异常向量表(汇编实现,写 VBAR)。必须在使能中断之前调用 */
void vectors_install(void);

/*
 * 初始化 GIC:关中断状态下配置 Distributor 与 CPU Interface。
 * 完成后中断尚未全局使能 —— 由 irq_global_enable() 打开。
 */
void gic_init(void);

/*
 * 本核的 GIC CPU 接口初始化(AM3-4)。
 * GICC_CTLR/GICC_PMR 是每核银行化的,CPU0 写过对 CPU1 无效 ——
 * 每个核都必须自己调一次。
 */
void gic_cpu_init(void);

/* 发 SGI(核间中断)。intid 取 0..15,cpu_mask 见 GIC_SGI_TARGET_* */
void gic_send_sgi(u32 intid, u8 cpu_mask);

/* 打开 CPU 的中断响应(CPSR 的 I 位清零) */
void irq_global_enable(void);

/* 关闭 CPU 的中断响应 */
void irq_global_disable(void);

/* ------------------------------------------------------------------ */
/* INTID 管理                                                          */
/* ------------------------------------------------------------------ */

/*
 * 注册某个 INTID 的处理函数。
 * @return 0 成功;-1 参数非法或该 INTID 已被占用
 */
int irq_register(u32 intid, irq_handler_t handler, void *arg);

/* 注销 */
int irq_unregister(u32 intid);

/* 在 GIC 中使能/关闭该 INTID */
void gic_enable_irq(u32 intid);
void gic_disable_irq(u32 intid);

/* 优先级:数值越小优先级越高(0 = 最高,0xFF = 最低) */
void gic_set_priority(u32 intid, u8 priority);

/* 目标 CPU 掩码:bit0 = CPU0,bit1 = CPU1 */
void gic_set_target(u32 intid, u8 cpu_mask);

/* 读取当前待处理的 INTID(读 ICCIAR) */
u32 gic_acknowledge(void);

/* 结束中断(写 ICCEOIR),必须传 gic_acknowledge() 返回的那个值 */
void gic_eoi(u32 intid);

/* 供汇编入口调用的 C 分发函数 */
/*
 * 异常处理函数。**返回值是"要从哪个帧离开"** ——
 * 不切换就原样返回 `frame`;切换则返回在目标任务栈上新搭的帧。
 * 见 boot/vectors.S 里 `mov sp, r0` 那一段(照抄 x86 的
 * "返回值即新栈指针"协议,`scheduler.cpp:68-69`)。
 */
arm_exc_frame_t *c_irq_handler(arm_irq_frame_t *frame);

/* 诊断:统计信息 */
typedef struct
{
    u32 irq_count;       /* 收到的中断总数 */
    u32 spurious_count;  /* 虚假中断数 */
    u32 unhandled_count; /* 没有登记处理函数的中断数 */
    u32 last_intid;      /* 最近一次处理的 INTID */
} irq_stats_t;

const irq_stats_t *irq_get_stats(void);

/* ------------------------------------------------------------------ */
/* 异常处理入口(由 vectors.S 调用)                                     */
/* ------------------------------------------------------------------ */

void c_undef_handler(arm_irq_frame_t *frame);
void c_prefetch_abort_handler(arm_irq_frame_t *frame);
void c_data_abort_handler(arm_irq_frame_t *frame);

/*
 * SVC 处理函数。与 c_irq_handler 同一套协议:**返回值是"要从哪个帧离开"**。
 *
 * 为什么要返回值(而 M4-8 时是 void):
 *   `svc` 不只是"系统调用" —— M4-9 之后它同时是**陷阱式让出**的入口
 *   (`arch_svc_yield()`),而让出必须真的切走。切走 = 在目标任务栈上
 *   搭一个帧并返回它。见 boot/vectors.S 的 `_vec_svc`。
 *
 * 立即数 = ARM_SVC_YIELD 时交给 sched_tick;= ARM_SVC_FRAME_CHECK 时做帧自检;
 * 其余落到"还没有系统调用层"的诊断分支。
 */
arm_exc_frame_t *c_svc_handler(arm_irq_frame_t *frame);
