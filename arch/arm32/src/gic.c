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

void c_irq_handler(arm_irq_frame_t *frame)
{
    u32 intid;
    u32 cpu_intid; /* 只取 INTID 字段,忽略 CPU 号(spec 里高 3 位是 CPU id) */

    (void)frame;

    intid     = gic_acknowledge();
    cpu_intid = intid & 0x3FFu; /* ICCIAR 的 [9:0] 才是 INTID */

    g_irq_stats.irq_count++;

    /*
     * 1023 是虚假中断:可能是电平中断在 EOI 之前已被撤销。
     * 这类中断不需要也不能 EOI,直接返回。
     */
    if (cpu_intid == GIC_INTID_SPURIOUS) {
        g_irq_stats.spurious_count++;
        return;
    }

    g_irq_stats.last_intid = cpu_intid;

    if (cpu_intid <= GIC_INTID_MAX && g_irq_table[cpu_intid].handler != NULL) {
        g_irq_table[cpu_intid].handler(cpu_intid, g_irq_table[cpu_intid].arg);
    } else {
        /*
         * 没有登记处理函数。仍然必须 EOI,否则这个中断源会一直堵着,
         * 把整个 CPU 接口的后续中断都挡住。
         */
        g_irq_stats.unhandled_count++;
    }

    gic_eoi(intid);
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
 * 译码 FSR(DFSR/IFSR)的故障状态。
 *
 * 用 [4:0] 而不是 [3:0] —— 这一点很容易搞错:
 * 例如 0x08 在 [3:0] 下看着像"域故障",而 [4:0]=0b01000 实际是
 * "Synchronous external abort"(访问了没有从设备的地址,
 * AXI 事务失败,正是本板故障注入触发的那种)。
 *
 * 上一版就写错了这个提示,反而误导排障,故此处写成显式查表。
 */
static const char *fsr_status_text(u32 fsr)
{
    switch (fsr & 0x1Fu) {
    case 0x01: return "alignment fault";
    case 0x02: return "debug event";
    case 0x04: return "instruction cache maintenance fault";
    case 0x05: return "translation fault, section";
    case 0x06: return "translation fault, page";
    case 0x07: return "translation fault, level 3 (LPAE)";
    case 0x08: return "synchronous external abort";
    case 0x09: return "domain fault, section";
    case 0x0A: return "domain fault, page";
    case 0x0B: return "domain fault, level 2 (LPAE)";
    case 0x0C: return "synchronous external abort on translation table walk";
    case 0x0D: return "permission fault, section";
    case 0x0E: return "permission fault, page";
    case 0x0F: return "permission fault, level 3 (LPAE)";
    case 0x10: return "synchronous parity or ECC error";
    case 0x16: return "asynchronous external abort";
    case 0x19: return "synchronous parity or ECC error on translation table walk";
    case 0x1C: return "synchronous external abort on translation table walk (LPAE)";
    case 0x1E: return "asynchronous external abort on translation table walk";
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
static void dump_regs(arm_irq_frame_t *frame)
{
    if (frame == NULL) {
        return;
    }

    console_printf("  pc       = 0x%08X\n", frame->pc);
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

void c_undef_handler(arm_irq_frame_t *frame)
{
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
    /*
     * M1 阶段还没有用户态;走到这里说明有人主动发了 SVC。
     *
     * ⚠ 这个处理函数**会返回**,不能打 "System halted":
     *   vectors.S 的 _vec_svc 没有 wfe 自旋,它是 pop 之后 movs pc, lr
     *   返回到 SVC 的下一条指令。照抄另外三个致命异常的说法会让人
     *   以为系统停了,而实际上它继续在跑 —— 这类误导在排障时代价很高。
     */
    console_puts("\n!!! SVC (no syscall layer yet) - diagnostic only, returning !!!\n");
    dump_regs(frame);
    console_puts("  Returning to the instruction after SVC.\n");
}

void c_prefetch_abort_handler(arm_irq_frame_t *frame)
{
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
    u32 dfar = arch_read_dfar(); /* 类似 x86 的 CR2 */
    u32 dfsr = arch_read_dfsr();

    console_puts("\n!!! Data Abort !!!\n");
    console_printf("  DFAR     = 0x%08X   (address that faulted; x86 equivalent is CR2)\n", dfar);
    console_printf("  DFSR     = 0x%08X   (%s)\n", dfsr, fsr_status_text(dfsr));
    dump_regs(frame);
    dump_halt();
}
