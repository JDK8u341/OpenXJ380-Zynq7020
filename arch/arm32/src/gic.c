/*
 * PL390 GIC 驱动 + INTID 处理表 + 异常诊断
 *
 * 寄存器基址(见 arch/platform.h):
 *   Distributor     0xF8F01000
 *   CPU Interface   0xF8F00100
 */

#include <arch/console.h>
#include <arch/cpu.h>
#include <arch/io.h>
#include <arch/irq.h>
#include <arch/sched.h>
#include <arch/percpu.h>
#include <arch/platform.h>

/* ------------------------------------------------------------------ */
/* Distributor 寄存器偏移                                              */
/* ------------------------------------------------------------------ */

#define GICD_CTLR       0x0000u /* 控制:bit0 = 使能 */
#define GICD_TYPER      0x0004u /* 类型:读它能得到 SPI 数量 */
#define GICD_ISENABLER  0x0100u /* 置位使能,每 32 个 INTID 一个寄存器 */
#define GICD_ICENABLER  0x0180u /* 清零使能 */
#define GICD_ISPENDR    0x0200u /* 置位 pending */
#define GICD_ICPENDR    0x0280u /* 清除 pending */
#define GICD_IPRIORITYR 0x0400u /* 优先级,每个 INTID 一个字节 */
#define GICD_ITARGETSR  0x0800u /* 目标 CPU 掩码,每个 INTID 一个字节 */
#define GICD_ICFGR      0x0C00u /* 触发方式,每 16 个 INTID 一个寄存器(每 INTID 2 位) */
#define GICD_SGIR       0x0F00u /* 软件产生中断(SGI / IPI) */

/* ------------------------------------------------------------------ */
/* CPU Interface 寄存器偏移                                            */
/* ------------------------------------------------------------------ */

#define GICC_CTLR 0x0000u /* 控制:bit0 = 使能 */
#define GICC_PMR  0x0004u /* 优先级掩码:只有优先级数值 < PMR 的中断才上报 */
#define GICC_IAR  0x000Cu /* 应答:读回待处理的 INTID */
#define GICC_EOIR 0x0010u /* 结束:写回应答时读到的 INTID */

/* ------------------------------------------------------------------ */
/* 处理表                                                              */
/* ------------------------------------------------------------------ */

typedef struct
{
    irq_handler_t handler;
    void         *arg;
} irq_entry_t;

/*
 * 覆盖 GIC 支持的全部 INTID(0..95),静态分配即可。
 * x86 侧是 256 项 IDT 表;这里项数少得多,没必要动态申请。
 */
#define IRQ_TABLE_SIZE (GIC_INTID_MAX + 1)

static irq_entry_t g_irq_table[IRQ_TABLE_SIZE];
static irq_stats_t g_irq_stats;

/* ------------------------------------------------------------------ */
/* 辅助:把 INTID 换算成寄存器地址                                     */
/* ------------------------------------------------------------------ */

/* 使能/pending 类寄存器:每 32 个 INTID 共用一个 32 位寄存器 */
static inline uintptr_t gicd_bit_reg(u32 base_offset, u32 intid)
{
    return PLAT_GIC_DIST_BASE + base_offset + (intid / 32u) * 4u;
}

static inline u32 gicd_bit_mask(u32 intid)
{
    return 1u << (intid % 32u);
}

/* 优先级/目标类寄存器:每个 INTID 一个字节 */
static inline uintptr_t gicd_byte_reg(u32 base_offset, u32 intid)
{
    return PLAT_GIC_DIST_BASE + base_offset + intid;
}

/* 配置寄存器:每 16 个 INTID 一个 32 位寄存器,每 INTID 占 2 位 */
static inline uintptr_t gicd_cfg_reg(u32 intid)
{
    return PLAT_GIC_DIST_BASE + GICD_ICFGR + (intid / 16u) * 4u;
}

static inline u32 gicd_cfg_shift(u32 intid)
{
    /* 每个 INTID 占 2 位,bit0 保留,bit1 = 1 表示边沿触发 */
    return (intid % 16u) * 2u + 1u;
}

/* ------------------------------------------------------------------ */
/* 初始化                                                              */
/* ------------------------------------------------------------------ */

void gic_init(void)
{
    u32 i;
    u32 lines;

    /*
     * 全程关中断下配置。配置期间若有中断进来会读到半初始化状态。
     */
    arch_irq_disable();

    /* ---- 1. 关闭 Distributor,配置期间不接受中断 ---- */
    mmio_write32(PLAT_GIC_DIST_BASE + GICD_CTLR, 0);

    /* ---- 2. 所有 INTID 设为最高优先级掩码之外的默认值 ---- */
    /*
     * 清空处理表,并把优先级统一设成 0xA0(中等偏低),
     * 具体驱动注册时再按需调整。
     */
    for (i = 0; i < IRQ_TABLE_SIZE; i++) {
        g_irq_table[i].handler = NULL;
        g_irq_table[i].arg     = NULL;
    }

    for (i = 0; i < 32u; i++) {
        mmio_write32(PLAT_GIC_DIST_BASE + GICD_IPRIORITYR + i * 4u, 0xA0A0A0A0u);
    }

    /*
     * ---- 3. SPI 默认路由到 CPU0,并保持电平触发 ----
     *
     * 只处理 SPI 段(32..95)。SGI/PPI(0..31)是每 CPU 私有的,
     * ITARGETSR 对它们只读,写了无效 —— 这点和 SPI 不同,容易踩。
     */
    lines = ((mmio_read32(PLAT_GIC_DIST_BASE + GICD_TYPER) & 0x1Fu) + 1u) * 32u;
    if (lines > IRQ_TABLE_SIZE) {
        lines = IRQ_TABLE_SIZE;
    }

    for (i = GIC_INTID_SPI_FIRST; i < lines; i++) {
        mmio_write8(gicd_byte_reg(GICD_ITARGETSR, i), 0x01u); /* 只发给 CPU0 */
    }

    /* 全部先关掉,由各驱动通过 gic_enable_irq() 打开自己那个 */
    for (i = 0; i < 32u; i++) {
        mmio_write32(PLAT_GIC_DIST_BASE + GICD_ICENABLER + i * 4u, 0xFFFFFFFFu);
    }

    /* 清掉可能残留的 pending */
    for (i = 0; i < 32u; i++) {
        mmio_write32(PLAT_GIC_DIST_BASE + GICD_ICPENDR + i * 4u, 0xFFFFFFFFu);
    }

    /* ---- 4. 使能 Distributor ---- */
    mmio_write32(PLAT_GIC_DIST_BASE + GICD_CTLR, 1u);

    /* ---- 5. 配置 CPU Interface ---- */
    mmio_write32(PLAT_GIC_CPU_BASE + GICC_PMR, 0xF0u); /* 放行优先级 < 0xF0 的中断 */
    mmio_write32(PLAT_GIC_CPU_BASE + GICC_CTLR, 1u);   /* 使能 CPU 接口 */
    arch_dsb();
}

void irq_global_enable(void)
{
    arch_irq_enable();
}

void irq_global_disable(void)
{
    arch_irq_disable();
}

/* ------------------------------------------------------------------ */
/* INTID 管理                                                          */
/* ------------------------------------------------------------------ */

int irq_register(u32 intid, irq_handler_t handler, void *arg)
{
    if (intid > GIC_INTID_MAX || handler == NULL) {
        return -1;
    }
    if (g_irq_table[intid].handler != NULL) {
        return -1; /* 已被占用,不静默覆盖 */
    }

    g_irq_table[intid].handler = handler;
    g_irq_table[intid].arg     = arg;
    return 0;
}

int irq_unregister(u32 intid)
{
    if (intid > GIC_INTID_MAX) {
        return -1;
    }
    g_irq_table[intid].handler = NULL;
    g_irq_table[intid].arg     = NULL;
    return 0;
}

void gic_enable_irq(u32 intid)
{
    if (intid > GIC_INTID_MAX) {
        return;
    }
    mmio_write32(gicd_bit_reg(GICD_ISENABLER, intid), gicd_bit_mask(intid));
    arch_dsb();
}

void gic_disable_irq(u32 intid)
{
    if (intid > GIC_INTID_MAX) {
        return;
    }
    mmio_write32(gicd_bit_reg(GICD_ICENABLER, intid), gicd_bit_mask(intid));
    arch_dsb();
}

void gic_set_priority(u32 intid, u8 priority)
{
    if (intid > GIC_INTID_MAX) {
        return;
    }
    mmio_write8(gicd_byte_reg(GICD_IPRIORITYR, intid), priority);
}

void gic_set_target(u32 intid, u8 cpu_mask)
{
    /*
     * SGI(0-15)的目标是在 ICCSGIxR 里指定,不是 ITARGETSR;
     * PPI(16-31)的 ITARGETSR 只读。两者写了都会静默无效,所以直接挡掉。
     */
    if (intid < GIC_INTID_SPI_FIRST || intid > GIC_INTID_MAX) {
        return;
    }
    mmio_write8(gicd_byte_reg(GICD_ITARGETSR, intid), cpu_mask);
}

/* 把某个 INTID 配成边沿触发(默认电平触发) */
void gic_set_edge_triggered(u32 intid, bool edge)
{
    uintptr_t reg;
    u32       shift;
    u32       value;

    if (intid > GIC_INTID_MAX) {
        return;
    }

    reg   = gicd_cfg_reg(intid);
    shift = gicd_cfg_shift(intid);
    value = mmio_read32(reg);

    if (edge) {
        value |= (1u << shift);
    } else {
        value &= ~(1u << shift);
    }
    mmio_write32(reg, value);
}

u32 gic_acknowledge(void)
{
    return mmio_read32(PLAT_GIC_CPU_BASE + GICC_IAR);
}

void gic_eoi(u32 intid)
{
    /*
     * 必须原样写回 gic_acknowledge() 读到的值。写错 INTID 会导致
     * "中断已处理完"的状态没有清除,该中断再也进不来 —— 表现为
     * 系统在一次中断后彻底安静,而寄存器看起来一切正常。
     */
    mmio_write32(PLAT_GIC_CPU_BASE + GICC_EOIR, intid);
    arch_dsb();
}

/* ------------------------------------------------------------------ */
/* 分发                                                                */
/* ------------------------------------------------------------------ */

/*
 * 前向声明:定义在本文件靠后处(和它的说明放在一起),但校验要在
 * `c_irq_handler` 的**最前面**做 —— 帧错了的话后面读到的一切都不可信。
 */
static void irq_frame_check(const arm_exc_frame_t *frame);

arm_exc_frame_t *c_irq_handler(arm_irq_frame_t *frame)
{
    u32       intid;
    u32       cpu_intid; /* 只取 INTID 字段,忽略 CPU 号(spec 里高 3 位是 CPU id) */
    percpu_t *pc;

    /*
     * ★ 帧结构自检放在最前面(在 ack 之前)★
     *
     * 它必须早于任何依赖寄存器的动作:帧错了的话,后面读到的所有东西
     * 都不可信 —— 包括 `gic_acknowledge()` 用的那些寄存器。
     * 代价是每个 tick 几次比较(1kHz),可以忽略。
     */
    irq_frame_check(frame);

    intid     = gic_acknowledge();
    cpu_intid = intid & 0x3FFu; /* ICCIAR 的 [9:0] 才是 INTID */

    /*
     * 中断统计按核分开(AM3-4)。
     *
     * 为什么必须分开:两核各自有 1kHz 私有定时器,tick 计数要能和
     * "本核处理了多少次中断"对账。若共用一份全局计数,CPU1 的 tick
     * 会把 CPU0 的对账搅乱 —— 而那种"对不上"看起来像丢了中断,
     * 排查方向会被完全带偏。
     *
     * g_irq_stats 保留为 **CPU0 的总账**:irq_get_stats() 的既有调用方
     * (启动自检里的 irq_ticks_eq_irq)语义不变。
     */
    pc = percpu_self();
    if (pc != NULL) {
        pc->irq_count++;
    }

    /* g_irq_stats 保留为 CPU0 的总账,既有调用方的语义不变 */
    if (pc == NULL || pc->cpu_id == 0u) {
        g_irq_stats.irq_count++;
    }

    /*
     * 1023 是虚假中断:可能是电平中断在 EOI 之前已被撤销。
     * 这类中断不需要也不能 EOI,直接返回。
     */
    if (cpu_intid == GIC_INTID_SPURIOUS) {
        if (pc == NULL || pc->cpu_id == 0u) {
            g_irq_stats.spurious_count++;
        }
        return;
    }

    if (pc != NULL) {
        pc->last_intid = cpu_intid;
    }
    if (pc == NULL || pc->cpu_id == 0u) {
        g_irq_stats.last_intid = cpu_intid;
    }

    if (cpu_intid <= GIC_INTID_MAX && g_irq_table[cpu_intid].handler != NULL) {
        g_irq_table[cpu_intid].handler(cpu_intid, g_irq_table[cpu_intid].arg);
    } else {
        /*
         * 没有登记处理函数。仍然必须 EOI,否则这个中断源会一直堵着,
         * 把整个 CPU 接口的后续中断都挡住。
         */
        if (pc == NULL || pc->cpu_id == 0u) {
            g_irq_stats.unhandled_count++;
        }
    }

    gic_eoi(intid);
    /*
     * ★ M4-9:在 tick 里做调度决策 ★
     *
     * 位置:在 EOI 之后 —— 中断已经应答完,这时改动现场帧不会影响
     * GIC 的状态机。返回值决定"从哪个帧离开"。
     */
    return sched_tick(frame);
}


const irq_stats_t *irq_get_stats(void)
{
    return &g_irq_stats;
}

/* ------------------------------------------------------------------ */
/* 异常诊断                                                            */
/* ------------------------------------------------------------------ */

/*
 * 这几个处理函数的价值在于"把现场打出来"。
 * x86 侧对应的 kernel/wsod/wsod.cpp 有一整套异常文本,
 * 这里先做到能定位问题:类型 + 出错地址 + 关键寄存器。
 *
 * ⚠ 这里的输出**刻意只用 ASCII**。
 * panic 路径的日志可能落在任意终端、任意抓取工具上,
 * 一旦编码不匹配,中文会变成一串乱码,而这时恰恰是最需要读懂的时候。
 * 普通启动日志用中文没问题,崩溃现场不行。
 */

/*
 * ====================================================================
 * ★ DFSR / IFSR 都不能用 `fsr & 0x1F` 取状态 ★
 * ====================================================================
 *
 * 这是板上实测抓出来的,不是推演出来的。
 *
 * 权威位域(ARMv7-A ARM,TTBCR.EAE==0 即**短描述符格式**,本内核用的就是它):
 *
 *   DFSR                          IFSR
 *   ---------------------------   ---------------------------
 *   bits[3:0]  FS[3:0]            bits[3:0]  FS[3:0]
 *   bits[7:4]  **Domain**         bits[8:4]  RES0
 *   bit[9]     LPAE               bit[9]     LPAE
 *   bit[10]    FS[4]              bit[10]    FS[4]
 *   bit[11]    WnR                bit[11]    RES0
 *   bit[12]    ExT                bit[12]    ExT
 *   bit[13]    CM                 bit[13]    RES0
 *   bit[16]    FnV                bit[16]    FnV
 *
 * DFSR 的 **Domain 正好压在状态位上面**(bits[7:4]),于是
 * `dfsr & 0x1F` 会把 Domain[0] 当成 FS[4]:
 *
 *   真实 FS            本内核 domain=15 时的 `& 0x1F`      译出来
 *   0x07 translation   0x17                              保留/未知
 *   0x05 translation   0x15                              保留/未知
 *
 * ★ 为什么这个错一直没暴露 ★
 *   以前只触发过三类 Data Abort,它们的 L1 描述符**都是 fault 项**
 *   (比如 0x50000000 没有映射),而 fault 项的 Domain 位是 0 ——
 *   于是 `& 0x1F` 碰巧等于真值。**取指路径(IFSR)完全没有 Domain 字段**,
 *   所以 XN 那一路也一直是对的。
 *
 *   直到 guard page 出现:它的 L1 项是**页表描述符**,
 *   而 vmap 从 `MMU_ATTR_NORMAL_WB` 里解出来的 domain 是 **15**
 *   (见 mmu.h:bits[8:5]=0b1111),Domain[0]=1 ——
 *   实测 DFAR=0x02497FFC 时打印出 `DFSR = 0x000008F7 (reserved / unknown)`,
 *   而正确译码是 FS=0b00111 = **translation fault, level 2**。
 *
 * 所以这里把 FS[4:0] 按位域取出来,而不是切一段。
 */
#define FSR_FS_LOW_MASK  0x0000000Fu
#define FSR_FS4_BIT      0x00000400u /* FS[4] 在 DFSR 与 IFSR 里都在 bit10 */
#define DFSR_DOMAIN_SHIFT 4u
#define DFSR_DOMAIN_MASK 0x0000000Fu
#define DFSR_WNR_BIT     0x00000800u
#define DFSR_LPAE_BIT    0x00000200u

static u32 fsr_status_of(u32 fsr)
{
    u32 fs = fsr & FSR_FS_LOW_MASK;

    if ((fsr & FSR_FS4_BIT) != 0u) {
        fs |= 0x10u;
    }

    return fs;
}

/*
 * 状态码表。按短描述符格式。
 *
 * ⚠ DFSR 与 IFSR 的表**几乎但不完全相同**,两处差异:
 *     0b00001 —— 取指路径是 "PC alignment fault",数据路径是 "alignment fault";
 *     0b00100 —— 只有数据路径有("Fault on instruction cache maintenance")。
 *   本内核两条都碰不到,所以共用一张表,差异写在这里而不是分成两份。
 *
 * ⚠ "level 1 / level 2" 的含义是**在哪一级查找时失败**:
 *     level 1 = L1 描述符本身是 fault 项(短描述符下 L1 覆盖 1MB);
 *     level 2 = L1 是页表项没问题,而是 **L2 小页项**出了问题。
 *   所以 guard page(取消映射 = L2 项清零)报的是 **level 2**,
 *   而"整个段没有映射"报的是 level 1。这两个不能混。
 */
static const char *fsr_status_text(u32 fsr)
{
    switch (fsr_status_of(fsr)) {
    case 0x01: return "alignment fault (PC alignment fault for IFSR)";
    case 0x02: return "debug event";
    case 0x03: return "access flag fault, level 1";
    case 0x04: return "fault on instruction cache maintenance (DFSR only)";
    case 0x05: return "translation fault, level 1";
    case 0x06: return "access flag fault, level 2";
    case 0x07: return "translation fault, level 2";
    case 0x08: return "synchronous external abort, not on translation table walk";
    case 0x09: return "domain fault, level 1";
    case 0x0A: return "reserved";
    case 0x0B: return "domain fault, level 2";
    case 0x0C: return "synchronous external abort on translation table walk, level 1";
    case 0x0D: return "permission fault, level 1";
    case 0x0E: return "synchronous external abort on translation table walk, level 2";
    case 0x0F: return "permission fault, level 2";
    case 0x10: return "TLB conflict abort";
    case 0x14: return "implementation defined fault (lockdown)";
    case 0x15: return "implementation defined fault (unsupported exclusive access)";
    case 0x16: return "SError exception";
    case 0x18: return "SError exception from parity or ECC error";
    case 0x19: return "synchronous parity or ECC error on memory access";
    case 0x1C: return "synchronous parity or ECC error on translation table walk, level 1";
    case 0x1E: return "synchronous parity or ECC error on translation table walk, level 2";
    default:   return "reserved / unknown";
    }
}


/*
 * 只打寄存器,不打标题 —— 标题由调用方打。
 *
 * 拆成两半是有原因的:早先的做法是处理函数先打"标题 + 故障地址",
 * 再调用一个"自带标题"的 dump_frame,于是同一个异常在串口上看起来像
 * 连续发生了两次:
 *     !!! Data Abort !!!
 *       DFAR = 0x50000000 ...
 *     !!! Data Abort !!!
 *       pc = ...
 * 真正严重的故障(嵌套异常、双重故障)也长这样,这种输出会直接误导排障。
 * 一个异常,一段输出。
 */
/*
 * 最近一次异常的 PC_FIX 修正量 —— dump_regs 用它把 `ret` 换算成
 * "出错/被中断的那条指令"。由各异常处理函数在入口处设置(纯 C,不依赖硬件)。
 */
static u32 g_last_pc_fix;

static void dump_regs(arm_irq_frame_t *frame)
{
    if (frame == NULL) {
        return;
    }

    /*
     * `pc` 与 `ret` 是两个不同的东西,所以要分别打 ——
     * 这是本板实测出来的(见 arch/taskctx.h 顶部的 LR 偏移表):
     *   pc  = 出错/被中断的那条指令(只用于报告)
     *   ret = 首选返回地址(只用于 `movs pc, lr`)
     * 旧代码只有一个字段,于是 SVC 与 Data Abort 打印的 pc 各偏 4 字节 ——
     * 一个"看起来很正常、实际指向隔壁那条指令"的值。
     */
    /*
     * `pc` 不再单独存字段:帧里只有 `ret`,而"出错/被中断的那条指令"是
     * `ret - PC_FIX`(修正量随异常类型不同,见 taskctx_asm.h 的表)。
     * 这里按"上一次异常是哪一个"取——dump_regs 是诊断用的,
     * 取不到精确值也比多存一个字段、多一处同步要好。
     */
    console_printf("  ret      = 0x%08X   (preferred return address)\n", frame->ret);
    console_printf("  pc       = 0x%08X   (faulting/interrupted instruction)\n",
                   arm_exc_pc(frame, g_last_pc_fix));
    console_printf("  svc_lr   = 0x%08X   (interrupted LR —— bl 会踩掉它,所以存进帧里)\n",
                   frame->svc_lr);
    console_printf("  spsr     = 0x%08X   (mode %u, %s)\n", frame->spsr, frame->spsr & ARM_CPSR_MODE_MASK,
                   arm_mode_text(frame->spsr));
    console_printf("  r0-r3    = 0x%08X 0x%08X 0x%08X 0x%08X\n",
                   frame->r[0], frame->r[1], frame->r[2], frame->r[3]);
    console_printf("  r4-r7    = 0x%08X 0x%08X 0x%08X 0x%08X\n",
                   frame->r[4], frame->r[5], frame->r[6], frame->r[7]);
    console_printf("  r8-r11   = 0x%08X 0x%08X 0x%08X 0x%08X\n",
                   frame->r[8], frame->r[9], frame->r[10], frame->r[11]);
    console_printf("  r12      = 0x%08X\n", frame->r[12]);
}

/*
 * 停机提示。实际的自旋在 vectors.S 的 `1: wfe / b 1b` 里 ——
 * 处理函数返回后就停在那里,核心寄存器仍可被 JTAG 读回。
 */
static void dump_halt(void)
{
    console_puts("  System halted. Registers remain readable via JTAG (rrd).\n");
}

/*
 * (CPSR 模式位的文本现在是 arch/taskctx.h 里的 `arm_mode_text()` ——
 *  纯函数,宿主机上就能单测。这里不再留一份副本:两份实现早晚会分叉。)
 */

/* ------------------------------------------------------------------ */
/* 异常帧布局的运行时自检(M4-6)                                       */
/* ------------------------------------------------------------------ */

/*
 * 为什么需要它:帧布局是**汇编与 C 之间的 ABI**。
 * `_Static_assert` 能钉住 C 侧的偏移宏,但钉不住"汇编真的按这些宏存了" ——
 * 比如 `stmia sp, {r0-r12}` 的寄存器顺序、`sub sp` 的字节数、`mrs` 存到哪个槽。
 * 这些只有**真的跑一遍**才知道。
 *
 * 做法:fault_test 的选择器 4 会把 r0-r12 设成已知图案再执行 `svc #0xA5A5`。
 * 这里逐个核对帧里的 13 个槽是否等于那些图案。
 *
 * ★ 这个检查还顺带证明了 `pc` 的偏移是对的 ★
 *   因为"这是不是一个布局自检的 SVC"这件事,是靠**读 `pc` 处那条指令的立即数**
 *   判断出来的。旧代码的 pc 指向 SVC 的下一条,按它取立即数会取到随机的
 *   下一条指令 —— 于是这个检查根本不会被触发(静默地什么都不做)。
 *   现在 pc 指向 SVC 本身,立即数才取得到。
 *
 * 返回 0 = 全部一致;非 0 = 第几个寄存器不对(1..13),-1 = 不是布局自检。
 */
static int svc_frame_check(const arm_exc_frame_t *frame)
{
    u32 insn;
    u32 i;

    if (frame == NULL) {
        return -1;
    }

    /*
     * pc 指向那条 SVC 指令 —— 直接读它的编码。
     * 这是内核自己的代码段,读取是安全的;真读不到也只会在返回前停机。
     */
    /*
     * `pc` 由 `ret - PC_FIX` 算出来 —— svc 的 PC_FIX 是 4,
     * 所以取指地址就是那条 `svc` 指令本身。
     */
    insn = *(const volatile u32 *)(uintptr_t)arm_exc_pc(frame, ARM_EXC_PC_FIX_SVC);

    if (arm_svc_immediate(insn) != ARM_SVC_FRAME_CHECK) {
        return -1;
    }

    for (i = 0; i < 13u; i++) {
        u32 expect = ARM_FRAME_CHECK_PATTERN ^ i;

        if (frame->r[i] != expect) {
            return (int)(i + 1u);
        }
    }

    /*
     * SPSR 也要是"从 SVC 模式来的" —— 这一条顺手验证了
     * `mrs r0, spsr` 确实被存进了 ARM_EXC_OFF_SPSR 那个槽。
     * 存错槽的话这里会读到 r12 或别的什么,模式位几乎不可能正好是 SVC。
     */
    if ((frame->spsr & ARM_CPSR_MODE_MASK) != ARM_MODE_SVC) {
        return 14;
    }

    /* ret 应当是 SVC 的下一条(实测 LR_svc = SVC + 4,不减) */
    if (frame->ret != (arm_exc_pc(frame, ARM_EXC_PC_FIX_SVC) + 4u)) {
        return 15;
    }

    return 0;
}

/* 自检结果:kmain 读它进自检报告 */
static int g_svc_frame_check = -1;

int irq_svc_frame_check_result(void)
{
    return g_svc_frame_check;
}

/*
 * 最近一次**会返回的**异常(SVC)的现场帧地址(M4-7)。
 *
 * 为什么要记它:异常帧现在必须建在 **SVC 栈**上(也就是任务的栈),
 * 而不是异常模式自己的栈。这是可以板上核对的硬判据 ——
 * 两个栈区在链接脚本里是**不相交**的两段,所以"落错地方"一眼可辨。
 *
 * ⚠ 只记 SVC(它会返回,不会打断后续自检)。IRQ 每秒来一千次,
 *   记它会把这里刷成噪声,核对时看不出是哪一次。
 */
static u32 g_last_svc_frame;



u32 irq_last_svc_frame_addr(void)
{
    return g_last_svc_frame;
}

/*
 * ★ IRQ 现场帧的结构自检(M4-7)★
 *
 * 为什么需要它:异常入口要把 `srsdb` 压进来的**原始 LR** 按异常类型修正
 * 才能得到返回地址。漏掉那一步的后果极其隐蔽 ——
 *
 *   LR_irq = 被中断指令 + 8,而首选返回地址是 +4。
 *   于是**每一条中断返回时都跳过一条指令**。内核照常启动、串口照常输出,
 *   直到定时器起来;被跳过的那条指令是随机的,后面某处 `ldr r0,[r0]`
 *   就可能拿着 0 去访存 → Data Abort,而且**每次的 DFAR 都不一样**。
 *   (实测就是这样:0x00 和 0x0010AA26 两个完全无关的地址。)
 *
 * 判据用的是那条不变量本身,与具体地址无关:
 *
 *   对 IRQ 而言,`ret` 是"被中断指令的下一条",而 `pc` 是"被中断的那条"
 *   ⇒ **ret 必须正好比 pc 大 4**。
 *
 * 修正漏掉时 `ret` 与 `pc` 会取到同一个原始值(ret == pc),这一条立刻失败。
 * 它每秒被检查上千次(每个 tick 一次),所以"偶尔跳错"也躲不过去。
 */
static u32 g_irq_frame_violations;

u32 irq_frame_violations(void)
{
    return g_irq_frame_violations;
}

/* ret 必须落在内核 .text 里(链接脚本给的界)*/
extern char __text_start[];
extern char __text_end[];

static void irq_frame_check(const arm_exc_frame_t *frame)
{
    u32 lo = (u32)(uintptr_t)__text_start;
    u32 hi = (u32)(uintptr_t)__text_end;

    if (frame == NULL) {
        g_irq_frame_violations++;
        return;
    }

    /* ★ 不变量:IRQ 的 ret 比 pc 大 4(PC_FIX_IRQ)== 被中断指令的下一条 ★ */
    if (frame->ret != (arm_exc_pc(frame, ARM_EXC_PC_FIX_IRQ) + 4u)) {
        g_irq_frame_violations++;
        return;
    }

    /* 不变量在下面这行:ret 比 pc 大 4,等价于"帧里两个值不是同一个原始值" */
    /* 返回地址必须在内核代码段内 —— 挡住"返回到野地址"*/
    if (frame->ret < lo || frame->ret >= hi) {
        g_irq_frame_violations++;
        return;
    }

    /* 被中断时应当仍在 SVC(内核)模式;用户态支持是 M7 的事 */
    if ((frame->spsr & ARM_CPSR_MODE_MASK) != ARM_MODE_SVC) {
        g_irq_frame_violations++;
    }
}

void c_undef_handler(arm_irq_frame_t *frame)
{
    g_last_pc_fix = ARM_EXC_PC_FIX_UND;
    console_puts("\n!!! Undefined Instruction !!!\n");
    /*
     * 触发指令地址在 frame->pc。
     * fault_test 会把选择器留在 r4,所以这里额外提示一下:
     * 若是故障注入进来的,r4 就是选择器,可直接对照 fault_test.h。
     */
    console_puts("  (pc points at the offending instruction;\n");
    console_puts("   if injected via fault_test, r4 holds the selector)\n");
    dump_regs(frame);
    dump_halt();
}

void c_svc_handler(arm_irq_frame_t *frame)
{
    g_last_pc_fix = ARM_EXC_PC_FIX_SVC;
    if (frame != NULL) {
        g_last_svc_frame = (u32)(uintptr_t)frame;
    }

    /*
     * M1 阶段还没有用户态;走到这里说明有人主动发了 SVC。
     *
     * ⚠ 这个处理函数**会返回**,不能打 "System halted":
     *   vectors.S 的 _vec_svc 没有 wfe 自旋,它是 ldmia 之后 movs pc, lr
     *   返回到 SVC 的下一条指令。照抄另外三个致命异常的说法会让人
     *   以为系统停了,而实际上它继续在跑 —— 这类误导在排障时代价很高。
     *
     * 先看这是不是帧布局自检(M4-6):是的话不打印长篇现场,
     * 只记录结论,免得把自检报告淹掉。
     */
    int check = svc_frame_check(frame);

    if (check >= 0) {
        g_svc_frame_check = check;
        return;
    }

    console_puts("\n!!! SVC (no syscall layer yet) - diagnostic only, returning !!!\n");
    dump_regs(frame);
    console_puts("  Returning to the instruction after SVC.\n");
}

void c_prefetch_abort_handler(arm_irq_frame_t *frame)
{
    g_last_pc_fix = ARM_EXC_PC_FIX_PABT;
    u32 ifar = 0;
    u32 ifsr = 0;

    __asm__ volatile("mrc p15, 0, %0, c6, c0, 2" : "=r"(ifar)); /* IFAR */
    __asm__ volatile("mrc p15, 0, %0, c5, c0, 1" : "=r"(ifsr)); /* IFSR */

    console_puts("\n!!! Prefetch Abort !!!\n");
    console_printf("  IFAR     = 0x%08X   (address the fetch failed at)\n", ifar);
    console_printf("  IFSR     = 0x%08X   (%s)\n", ifsr, fsr_status_text(ifsr));
    dump_regs(frame);
    dump_halt();
}

void c_data_abort_handler(arm_irq_frame_t *frame)
{
    g_last_pc_fix = ARM_EXC_PC_FIX_DABT;
    u32 dfar = arch_read_dfar(); /* 类似 x86 的 CR2 */
    u32 dfsr = arch_read_dfsr();

    console_puts("\n!!! Data Abort !!!\n");
    console_printf("  DFAR     = 0x%08X   (address that faulted; x86 equivalent is CR2)\n", dfar);
    console_printf("  DFSR     = 0x%08X   (%s)\n", dfsr, fsr_status_text(dfsr));
    /*
     * Domain 与 WnR 单独打出来。
     *
     * 不是为了好看:Domain 在 DFSR 里占 bits[7:4],**紧压在状态位上面** ——
     * 只打状态文本的话,一个"域号不对"的故障和"状态码不对"的故障
     * 在串口上长得一模一样。本项目的 FSR 译码就因为这个布局错了很久,
     * 直到 guard page 的 Data Abort 报出 0x8F7 才露出来(见上面的说明)。
     *
     * WnR(bit11)同样值得单列:"读崩了"和"写崩了"对应的越界方向相反,
     * 而栈溢出只可能是写。
     */
    console_printf("  domain   = %u   WnR = %u (%s)   LPAE = %u\n",
                   (dfsr >> DFSR_DOMAIN_SHIFT) & DFSR_DOMAIN_MASK,
                   (dfsr & DFSR_WNR_BIT) != 0u ? 1u : 0u,
                   (dfsr & DFSR_WNR_BIT) != 0u ? "write" : "read",
                   (dfsr & DFSR_LPAE_BIT) != 0u ? 1u : 0u);
    dump_regs(frame);
    dump_halt();
}

/* ------------------------------------------------------------------ */
/* 每 CPU 的 GIC 配置(AM3-4)                                          */
/* ------------------------------------------------------------------ */

/*
 * 本核的 CPU 接口初始化。
 *
 * ⚠ GICC_CTLR / GICC_PMR 是**每核银行化**的:CPU0 在 gic_init() 里写过,
 *   对 CPU1 没有任何影响。CPU1 必须自己再写一遍,否则它的 CPU 接口
 *   一直是关的 —— 表现为"CPU1 的中断永远不来,而 distributor 那边
 *   看上去什么都配好了"。
 *
 * Distributor 不用重配(它是全局的,gic_init() 已经弄好)。但要注意:
 * **PPI 与 SGI 段(INTID 0..31)的使能位在 distributor 里同样是按核
 * 银行化的** —— 所以 CPU1 也得自己 gic_enable_irq(自己的 PPI)。
 */
void gic_cpu_init(void)
{
    mmio_write32(PLAT_GIC_CPU_BASE + GICC_PMR, 0xF0u);
    mmio_write32(PLAT_GIC_CPU_BASE + GICC_CTLR, 1u);
    arch_dsb();
}

/*
 * 发一个 SGI(软件产生中断,即 IPI)。
 *
 * GICD_SGIR 的字段:
 *   [15:0]  INTID(0..15 才是 SGI)
 *   [23:16] 目标核掩码(当 [25:24] = 0b01 时有效)
 *   [25:24] 目标过滤:0b01 = 只发给掩码里的核
 *
 * 用 SGI 而不是"写一个共享变量然后 SEV":SEV 只是唤醒 WFE,
 * 如果目标核正在跑而不是在 WFE,那个"通知"就丢了。SGI 走的是
 * 中断控制器,一定能进中断向量。
 */
void gic_send_sgi(u32 intid, u8 cpu_mask)
{
    mmio_write32(PLAT_GIC_DIST_BASE + GICD_SGIR,
                 ((u32)(cpu_mask & 0xFFu) << 16) | (1u << 24) | (intid & 0xFu));
    arch_dsb();
}
