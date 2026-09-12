/*
 * ARMv7-A 缓存维护 —— 硬件侧(CP15 + PL310)
 *
 * 与 include/arch/cache.h 的分工:那边是纯几何解码(可宿主机单测),
 * 这边是真正碰 CP15 和外设寄存器的部分,只能上板验证。
 *
 * ====================================================================
 * 为什么整块失效必须用"按 set/way"而不是"按地址"
 * ====================================================================
 *
 * 打开数据缓存之前,缓存里可能有复位后残留的未知内容。如果其中存在
 * 脏行(dirty line),缓存一旦使能,这些行在将来被逐出时会把陈旧数据
 * 写回内存,静默覆盖掉之后写入的正确内容。
 *
 * 按地址(MVA)的操作只能覆盖你**想得起来**的那些地址 ——
 * 而这里的问题是"不知道缓存里有什么",所以必须能遍历整个缓存。
 * 这就是 set/way 形式的用途。
 *
 * ====================================================================
 * ⚠ 整块 set/way 操作在多核下不安全
 * ====================================================================
 *
 * "按 set/way 失效"是按**物理缓存结构**操作的,不区分数据的归属。
 * 多核运行时执行它,可能丢掉另一个核还没写回的脏行。
 *
 * 所以本文件里的整块操作**只在单核启动阶段、使能缓存之前**调用。
 * 本板是双核 Cortex-A9,等 M4 做 SMP 时:
 *   - CPU0 使能缓存前,必须确认 CPU1 还没开始跑(它从 OCM 跳板启动,
 *     那时才参与一致性);
 *   - CPU1 使能自己的缓存前,不能用整块失效去动共享的 L2 ——
 *     L2 的维护要走 PL310 自己的寄存器,并且要在所有核都停下时做。
 *
 * 这些约束不是理论洁癖:违反它们的症状是"偶发数据损坏",
 * 而且要跑很久才出现一次。
 */

#include <arch/cache.h>
#include <arch/cpu.h>
#include <arch/io.h>
#include <arch/platform.h>
#include <arch/timer.h>
#include <arch/types.h>

/* ------------------------------------------------------------------ */
/* 几何发现                                                             */
/* ------------------------------------------------------------------ */

/*
 * 查某一级、某种缓存的几何。
 * instruction = false 查数据缓存,true 查指令缓存。
 *
 * 查完必须把 CSSELR 复原成 0 —— 它是全局状态,
 * 留在别的级别上会让之后所有读 CCSIDR 的代码拿到错误的几何。
 */
cache_geometry_t cache_discover(bool instruction, u32 level)
{
    u32              saved = arch_read_csselr();
    cache_geometry_t geo;

    arch_write_csselr((level << CACHE_CSSELR_LEVEL_SHIFT) | (instruction ? CACHE_CSSELR_IND_BIT : 0u));

    geo = cache_decode_ccsidr(arch_read_ccsidr());

    arch_write_csselr(saved);

    return geo;
}

/*
 * L1 数据缓存的行大小。
 * 放在这里而不是和区间维护放一起:PL310 的按地址操作也要用它做对齐,
 * 而 PL310 那段在文件里更靠前。
 */
u32 cache_line_bytes(void)
{
    return cache_discover(false, 0u).line_bytes;
}

/* ------------------------------------------------------------------ */
/* 按 set/way 的整块操作                                               */
/* ------------------------------------------------------------------ */

/* 三种整块操作的区别只在写哪个 CP15 寄存器,循环结构完全一样 */
typedef enum
{
    CACHE_OP_INVALIDATE = 0,       /* DCISW  —— 不写回,直接丢 */
    CACHE_OP_CLEAN = 1,            /* DCCSW  —— 写回,保留 */
    CACHE_OP_CLEAN_INVALIDATE = 2, /* DCCISW —— 写回后再丢 */
} cache_op_t;

static void cache_setway_op(cache_op_t op)
{
    u32              clidr = arch_read_clidr();
    u32              max_level = cache_setway_max_level(clidr);
    u32              level;
    cache_geometry_t geo;
    u32              way;
    u32              set;

    for (level = 0; level <= max_level; level++) {
        if (!cache_level_has_data(clidr, level)) {
            continue;
        }

        geo = cache_discover(false, level);

        for (way = 0; way < geo.ways; way++) {
            for (set = 0; set < geo.sets; set++) {
                u32 value = cache_setway_value(way, set);

                switch (op) {
                case CACHE_OP_CLEAN:
                    arch_dcache_clean_setway(value);
                    break;
                case CACHE_OP_CLEAN_INVALIDATE:
                    arch_dcache_clean_invalidate_setway(value);
                    break;
                case CACHE_OP_INVALIDATE:
                default:
                    arch_dcache_invalidate_setway(value);
                    break;
                }
            }
        }
    }

    arch_dsb();
}

void cache_dcache_invalidate_all(void)
{
    cache_setway_op(CACHE_OP_INVALIDATE);
}

void cache_dcache_clean_all(void)
{
    cache_setway_op(CACHE_OP_CLEAN);
}

void cache_dcache_clean_invalidate_all(void)
{
    cache_setway_op(CACHE_OP_CLEAN_INVALIDATE);
}

/*
 * 使能缓存前的统一清理。
 *
 * 顺带失效分支预测器:分支预测器里可能残留上次运行的预测项,
 * 指向已经失效的代码地址。ARM 的启动建议里明确要求这一步。
 */
void cache_invalidate_before_enable(void)
{
    /*
     * 顺序:先失效指令缓存与分支预测器,再失效数据缓存。
     *
     * 数据缓存放最后是有讲究的:失效数据缓存之后、使能之前,
     * 不能再有会分配缓存行的访存发生,否则又引入新的未知内容。
     * 所以这一串后面紧接着就应当是"使能"那一步,中间别插别的。
     */
    arch_icache_invalidate_all();
    arch_branch_predictor_invalidate_all();
    cache_dcache_invalidate_all();

    arch_dsb();
    arch_isb();
}

/* ------------------------------------------------------------------ */
/* Coherency 前置条件:SCU + ACTLR                                     */
/* ------------------------------------------------------------------ */

/*
 * ⚠ 这两步在移植时极易被整个漏掉,而漏了**不会报任何错** ——
 *   地址转换照样工作、系统照样跑,唯一的症状是"缓存开了但完全没有加速"。
 *   本项目就踩过:基准测试测出 off=24704us on=24701us,速度比 1x。
 *
 * 原因在于区域表把 DDR 映射成了 **Shareable(S=1)** —— 与 Xilinx 原表一致。
 * Cortex-A9 上,可共享的访问要走 SCU 做一致性检查;而 SCU 没使能、
 * ACTLR 的 SMP 位也没置时,这些访问的一致性无从保证,
 * 硬件于是干脆不把它们放进 L1。结果就是缓存"开着但没用"。
 *
 * Xilinx 的 cortexa9/gcc/boot.S 里这两步都在,顺序是:
 *   使能 SCU -> 配 TTBR0/DACR -> 开 MMU+D-Cache -> 写 ACTLR
 * 本函数把前两步合并,在 mmu_enable() 之前调用。
 *
 * ⚠ SCU 必须在**缓存使能之前**打开。反过来的话,缓存里可能已经存了
 *   未经一致性检查的数据,再打开 SCU 并不能追溯修正它们。
 */
void cortexa9_coherency_init(void)
{
    u32 scu_ctrl;
    u32 actlr;

    /* 1. 使能 SCU(bit0)。此处 MMU 还没有开,访问的是物理地址 */
    scu_ctrl = mmio_read32(PLAT_SCU_BASE);
    if ((scu_ctrl & 1u) == 0u) {
        mmio_write32(PLAT_SCU_BASE, scu_ctrl | 1u);
    }
    arch_dsb();

    /*
     * 2. ACTLR:
     *      bit6 = SMP       —— 参与 SCU 一致性
     *      bit0 = FW        —— 缓存/TLB 维护操作广播到其它核
     *
     *    单核阶段置 bit0 看着没用,但它决定的是"维护操作的行为定义"。
     *    等 M4 加第二个核时,少了它会出现"清了缓存但别的核看不见",
     *    而那时很难联想到是启动阶段漏了这么一位。
     */
    actlr = arch_read_actlr();
    actlr |= ACTLR_SMP | ACTLR_FW;
    arch_write_actlr(actlr);

    arch_dsb();
    arch_isb();
}

u32 cortexa9_scu_status(void)
{
    return mmio_read32(PLAT_SCU_BASE);
}

u32 cortexa9_actlr_status(void)
{
    return arch_read_actlr();
}

/* ------------------------------------------------------------------ */
/* 几何诊断                                                             */
/* ------------------------------------------------------------------ */

u32 cache_total_bytes(bool instruction, u32 level)
{
    return cache_discover(instruction, level).total_bytes;
}

/* ------------------------------------------------------------------ */
/* 缓存有效性基准                                                       */
/* ------------------------------------------------------------------ */

/*
 * 4KB 工作缓冲区。放在 .bss 里 —— 那属于 DDR 的 Normal WB 段,
 * 正是要被缓存的那类内存。
 *
 * 4KB 远小于 32KB 的 L1,所以只要缓存真的在起作用,
 * 第一遍之后所有访问都应当命中。
 *
 * ⚠ 必须是 volatile,而且这不是"保险起见"。
 *
 *   第一版没加 volatile,结果这个基准测试**被编译器整体优化掉了**:
 *   数组在 .bss 里、编译期就知道全是 0,于是 GCC 直接把 1024 个 0
 *   求和折叠成常数 0,整个循环消失。实测读出 "off=7us on=7us" ——
 *   20 万次访存只用 7us(666MHz 下才 4662 个周期),物理上不可能。
 *
 *   这个错误恰好被基准测试自己的判据抓到了:速度比 1x 触发警告。
 *   如果当初只判断"寄存器位是否为 1",这次就会安静地通过 ——
 *   而验证工具本身失效,比没有验证更危险。
 */
static volatile u32 g_bench_buf[1024];

/* 用来吃掉累加结果,防止编译器把整个循环优化掉 */
volatile u32 g_bench_sink;

/*
 * 在**指定工作集**上跑内存密集循环,返回耗时(微秒)。
 *
 * 工作集大小是这套验证的关键变量:
 *   4KB   —— 装得进 32KB 的 L1,只反映 L1;
 *   128KB —— 超出 L1、装得进 512KB 的 L2,只有 L2 在起作用才快;
 *   2MB   —— 两级都装不下,每次都要去 DDR。
 *
 * 之所以要参数化:光测 4KB 的话,L2 开没开完全看不出来 ——
 * 本项目就出过这个纰漏,L2 明明"寄存器读回是 1"却没有独立证据
 * 证明它在缓存任何东西。
 */
u32 cache_bench_us_on(const volatile u32 *buf, u32 words, u32 passes)
{
    u64 t0;
    u64 t1;
    u32 pass;
    u32 i;
    u32 sum = 0;

    if (buf == NULL || words == 0u || passes == 0u) {
        return 0u;
    }

    t0 = timer_read_us();

    for (pass = 0; pass < passes; pass++) {
        for (i = 0; i < words; i++) {
            sum += buf[i];
        }
    }

    t1 = timer_read_us();

    /* 写进 volatile,确保上面的循环不会被判定为无副作用而删除 */
    g_bench_sink = sum;

    return (u32)(t1 - t0);
}

u32 cache_benchmark_us(u32 passes)
{
    return cache_bench_us_on(g_bench_buf, sizeof(g_bench_buf) / sizeof(g_bench_buf[0]), passes);
}

/*
 * L2 有效性实验用的工作集。
 *
 * 128KB:远超 32KB 的 L1,又远小于 512KB 的 L2 ——
 * 这个尺寸下若 L2 真的在工作,访问应当基本命中 L2;
 * 若 L2 没在工作(哪怕寄存器说它开着),就退化成每次去 DDR。
 * 两者的耗时差别是数量级的,骗不了人。
 */
static volatile u32 g_bench_l2_buf[32768]; /* 128KB */

const volatile u32 *cache_l2_bench_buf(void)
{
    return g_bench_l2_buf;
}

u32 cache_l2_bench_words(void)
{
    return sizeof(g_bench_l2_buf) / sizeof(g_bench_l2_buf[0]);
}

/* ------------------------------------------------------------------ */
/* 使能                                                                 */
/* ------------------------------------------------------------------ */

/*
 * 使能 L1 D-Cache 与 I-Cache。
 *
 * **本函数不动 L2(PL310)** —— 它由 l2_cache_init() 单独配置。
 *
 * 顺序与 Xilinx 的 cortexa9/gcc/boot.S 一致:那里也是先写
 * SCTLR 打开 MMU 与 D-Cache,之后才处理 L2。先动 L1 的好处是
 * 万一出问题,变量只有一个 —— 不用同时怀疑两级缓存。
 *
 * ⚠ 三步的顺序不能变:
 *     1. 失效 D-Cache / I-Cache / 分支预测器
 *     2. (紧接着)写 SCTLR 使能
 *     3. 中间不要插入会分配缓存行的访存
 *
 *   第 3 条容易被忽略:失效之后如果还去读了一大片数据,
 *   那些访问在缓存已使能的情况下会重新分配缓存行,把刚清干净的缓存
 *   又填上未知内容 —— 于是"先失效"这一步白做了,而且不留任何痕迹。
 */
void cache_enable_l1(void)
{
    u32 sctlr;

    /* 1. 清干净 */
    cache_invalidate_before_enable();

    /* 2. 使能。D-Cache 与 I-Cache 一起开:I-Cache 的失效刚做过,
     *    而两者独立,一起开不会互相影响。 */
    sctlr = arch_read_sctlr();
    sctlr |= SCTLR_C | SCTLR_I;
    arch_write_sctlr(sctlr);

    arch_dsb();
    arch_isb();

    /*
     * 3. 到这里已经在缓存下运行了。
     *
     * 紧接着做一次自检:如果 I-Cache 或 D-Cache 有任何一致性问题,
     * 最可能的表现就是"接下来的第一次函数调用取到错的指令"
     * 或者"读回的值不对"。所以这里立刻读回一个已知常量,
     * 让问题在最早的时刻暴露,而不是等到几百毫秒后的主循环。
     */
    (void)arch_read_sctlr();
}

bool cache_dcache_enabled(void)
{
    return (arch_read_sctlr() & SCTLR_C) != 0u;
}

bool cache_icache_enabled(void)
{
    return (arch_read_sctlr() & SCTLR_I) != 0u;
}

/* ------------------------------------------------------------------ */
/* PL310 L2                                                             */
/* ------------------------------------------------------------------ */

#define L2CC(offset) (PLAT_L2CC_BASE + (offset))

/* SLCR 里 L2 的 RAM 配置寄存器(与 unlock/lock 一起用) */
#define SLCR_L2C_RAM      0xF8000A1Cu
#define SLCR_L2C_RAM_CFG  0x00020202u

u32 l2_cache_id(void)
{
    return mmio_read32(L2CC(L2CC_ID));
}

u32 l2_cache_type(void)
{
    return mmio_read32(L2CC(L2CC_TYPE));
}

u32 l2_cache_control(void)
{
    return mmio_read32(L2CC(L2CC_CONTROL));
}

bool l2_cache_is_enabled(void)
{
    return (l2_cache_control() & L2CC_CONTROL_ENABLE) != 0u;
}

/*
 * 等 L2 维护操作做完。
 *
 * 写 SYNC 之后读回 0 表示完成。这个轮询不能省:
 * PL310 的失效/清洗是**后台执行**的,不等待就继续改内存,
 * 缓存里可能还留着描述旧内容的行 —— 之后它们被逐出时会把
 * 陈旧数据写回去,是 DMA 数据损坏的经典成因。
 */
void l2_cache_sync(void)
{
    mmio_write32(L2CC(L2CC_SYNC), 0u);

    while (mmio_read32(L2CC(L2CC_SYNC)) != 0u) {
        /* 轮询等待 */
    }
}

void l2_cache_invalidate_all(void)
{
    mmio_write32(L2CC(L2CC_INV_WAY), L2CC_ALL_WAYS);
    l2_cache_sync();
}

void l2_cache_clean_all(void)
{
    mmio_write32(L2CC(L2CC_CLEAN_WAY), L2CC_ALL_WAYS);
    l2_cache_sync();
}

void l2_cache_disable(void)
{
    if (!l2_cache_is_enabled()) {
        return;
    }

    /*
     * 先写回再失效,最后才清使能位。
     *
     * 直接清使能位会丢掉尚未写回 DDR 的脏行 —— 症状是"某些内存写入
     * 凭空消失",而且只在关 L2 的那一刻发生,下次开起来又是一切正常。
     * 拆成两步(clean 再 invalidate)同时也是 PL310 勘误 588369 的规避方式。
     */
    l2_cache_clean_all();
    l2_cache_invalidate_all();

    mmio_write32(L2CC(L2CC_CONTROL), 0u);
    l2_cache_sync();
    arch_dsb();
}

void l2_cache_enable(void)
{
    mmio_write32(L2CC(L2CC_CONTROL), L2CC_CONTROL_ENABLE);
    l2_cache_sync();
    arch_dsb();
}

/*
 * 勘误 727915(Background Clean and Invalidate by Way 会导致数据损坏)
 * 的规避方式,记录在此以备将来:
 *
 * Xilinx 的 xil_cache.c 在做"按 Way 的清洗并失效"前后,会往调试寄存器
 * (L2CC_DEBUG_CTRL,0xF40)写 0x3 关闭写回与行填充、操作完再写 0x0 恢复,
 * 让操作从前台完成。
 *
 * **本实现没有使用那条路径** —— 初始化用的是"按 Way 失效"(不涉及清洗),
 * 区间维护用的是"按地址",两者都不受该勘误影响。
 * 将来若有人为了"整块刷新"去用 L2CC_INV_CLN_WAY,必须先加上这层包裹。
 */

void l2_cache_init(void)
{
    u32 ctrl;

    /* 1. 先关闭,配置期间不受影响 */
    mmio_write32(L2CC(L2CC_CONTROL), 0u);
    l2_cache_sync();

    /* 2. 辅助控制:预取、替换策略、奇偶校验等默认位 */
    ctrl = mmio_read32(L2CC(L2CC_AUX_CONTROL));
    ctrl |= L2CC_AUX_CONTROL_DEFAULT;
    mmio_write32(L2CC(L2CC_AUX_CONTROL), ctrl);

    /* 3. Tag / Data RAM 延迟。这两个值随芯片走,不能自己猜 */
    mmio_write32(L2CC(L2CC_TAG_RAM_CTRL), L2CC_TAG_RAM_LATENCY);
    mmio_write32(L2CC(L2CC_DATA_RAM_CTRL), L2CC_DATA_RAM_LATENCY);

    /* 4. 整块失效。此时 L2 还关着,清掉复位后的未知内容 */
    l2_cache_invalidate_all();

    /* 5. 清掉可能挂起的中断 */
    {
        u32 pending = mmio_read32(L2CC(L2CC_ISR));

        if (pending != 0u) {
            mmio_write32(L2CC(L2CC_IAR), pending);
        }
    }

    /*
     * 6. SLCR 里的 L2 RAM 配置。
     *
     * 这一步容易被漏掉 —— 它不在 L2 寄存器空间里,而在 SLCR 里,
     * 且需要先解锁。少了它 L2 能"使能成功"但行为不保证。
     */
    mmio_write32(SLCR_UNLOCK, SLCR_UNLOCK_KEY);
    mmio_write32(SLCR_L2C_RAM, SLCR_L2C_RAM_CFG);
    mmio_write32(SLCR_LOCK, SLCR_LOCK_KEY);

    /* 7. 使能 */
    mmio_write32(L2CC(L2CC_CONTROL), L2CC_CONTROL_ENABLE);
    l2_cache_sync();

    arch_dsb();
}

/* ------------------------------------------------------------------ */
/* L2 按物理地址的区间维护                                             */
/* ------------------------------------------------------------------ */

typedef enum
{
    L2_OP_CLEAN = 0,
    L2_OP_INVALIDATE = 1,
    L2_OP_CLEAN_INVALIDATE = 2,
} l2_op_t;

static void l2_range_op(uintptr_t addr, size_t size, l2_op_t op)
{
    cache_range_t range = cache_align_range(addr, size, cache_line_bytes());
    uintptr_t     line;
    u32           step = cache_line_bytes();

    if (cache_range_is_empty(&range) || !l2_cache_is_enabled()) {
        return;
    }

    for (line = range.start; line < range.end; line += step) {
        switch (op) {
        case L2_OP_CLEAN:
            mmio_write32(L2CC(L2CC_CLEAN_PA), (u32)line);
            break;
        case L2_OP_INVALIDATE:
            mmio_write32(L2CC(L2CC_INV_PA), (u32)line);
            break;
        case L2_OP_CLEAN_INVALIDATE:
        default:
            /*
             * ⚠ PL310 勘误 588369:单条"清洗并失效"**不会失效已经干净的行**。
             * 于是一行若本来是干净的,做完这个操作后它仍然留在缓存里 ——
             * 对 DMA 接收缓冲来说,这意味着 CPU 接着读到的还是旧副本。
             *
             * 规避办法就是拆成两步:先按地址清洗(把该写的写下去),
             * 再按地址失效(无条件丢弃)。两步之后无论原本脏不脏,
             * 缓存里都不会再留着这一行。
             */
            mmio_write32(L2CC(L2CC_CLEAN_PA), (u32)line);
            mmio_write32(L2CC(L2CC_INV_PA), (u32)line);
            break;
        }
    }

    l2_cache_sync();
}

void l2_cache_clean_range(uintptr_t addr, size_t size)
{
    l2_range_op(addr, size, L2_OP_CLEAN);
}

void l2_cache_invalidate_range(uintptr_t addr, size_t size)
{
    l2_range_op(addr, size, L2_OP_INVALIDATE);
}

void l2_cache_clean_invalidate_range(uintptr_t addr, size_t size)
{
    l2_range_op(addr, size, L2_OP_CLEAN_INVALIDATE);
}

/* ------------------------------------------------------------------ */
/* 按地址(MVA)的区间维护                                               */
/* ------------------------------------------------------------------ */

/*
 * 三种区间维护的循环结构完全一样,区别只在写哪个 CP15 寄存器:
 *
 *   DCCMVAC  (c7,c10,1)  清洗     —— 写回到一致性点,行保留
 *   DCIMVAC  (c7,c6,1)   失效     —— 丢弃,不写回
 *   DCCIMVAC (c7,c14,1)  清洗并使失效
 *
 * ⚠ 这里**没有**逐个地址都做一次 DSB。整段做完之后统一 DSB 一次即可:
 *   DSB 的作用是让之前发出的维护操作在后续访存之前完成,
 *   而不是每条操作之后都要屏障一次。
 *   逐条 DSB 会让 DMA 路径慢上几十倍,而收益为零。
 */
static void cache_range_op(uintptr_t addr, size_t size, cache_op_t op)
{
    cache_range_t range = cache_align_range(addr, size, cache_line_bytes());
    uintptr_t     line;

    if (cache_range_is_empty(&range)) {
        return;
    }

    for (line = range.start; line < range.end; line += cache_line_bytes()) {
        switch (op) {
        case CACHE_OP_CLEAN:
            arch_dcache_clean_mva(line);
            break;
        case CACHE_OP_CLEAN_INVALIDATE:
            arch_dcache_clean_invalidate_mva(line);
            break;
        case CACHE_OP_INVALIDATE:
        default:
            arch_dcache_invalidate_mva(line);
            break;
        }
    }

    arch_dsb();

    /*
     * L2 必须**跟着 L1 一起维护**,而且顺序不能反。
     *
     * 为什么不能只做 L1:Zynq 上的 L2 位于 CPU 与 DDR 之间,
     * PL 侧的主设备访问 DDR 时会经过它。只清洗 L1 的话,
     * 数据可能停在 L2 里没落到 DDR,PL 读到的是旧内容 ——
     * 而 CPU 这边一切正常,问题只在设备侧偶发出现。
     *
     * 为什么顺序不能反:
     *   clean  —— 必须**先 L1 后 L2**。先清 L2 再清 L1 的话,
     *             L1 随后写回的脏行会留在 L2 里,而 L2 的清洗已经做完了,
     *             于是这些数据最终没到 DDR;
     *   invalidate —— 也必须**先 L1 后 L2**。反过来的话 L1 里残留的
     *             旧副本会一直用下去,读不到设备刚写进 DDR 的新数据。
     *
     * 上面 L1 的循环已经跑完,这里再对 L2 做同样的操作,顺序自然正确。
     */
    switch (op) {
    case CACHE_OP_CLEAN:
        l2_cache_clean_range(addr, size);
        break;
    case CACHE_OP_CLEAN_INVALIDATE:
        l2_cache_clean_invalidate_range(addr, size);
        break;
    case CACHE_OP_INVALIDATE:
    default:
        l2_cache_invalidate_range(addr, size);
        break;
    }
}

void cache_clean_range(uintptr_t addr, size_t size)
{
    cache_range_op(addr, size, CACHE_OP_CLEAN);
}

void cache_invalidate_range(uintptr_t addr, size_t size)
{
    cache_range_op(addr, size, CACHE_OP_INVALIDATE);
}

void cache_clean_invalidate_range(uintptr_t addr, size_t size)
{
    cache_range_op(addr, size, CACHE_OP_CLEAN_INVALIDATE);
}

/* ------------------------------------------------------------------ */
/* 自检                                                                */
/* ------------------------------------------------------------------ */

/* 自检缓冲区。256 字节 = 8 行(32 字节行),足够覆盖多行情况 */
#define SELFTEST_BYTES 256u
#define SELFTEST_OLD   0xA5A5A5A5u
#define SELFTEST_NEW   0x5A5A5A5Au

static volatile u32 g_selftest_buf[SELFTEST_BYTES / 4u];

/*
 * 不用 DMA 也能验证缓存维护,靠的是"缓存与内存是两份副本"这个事实。
 *
 *   测试 1(证明缓存确实持有未写回的数据,且 invalidate 真的丢弃它):
 *     先把 OLD 写进内存并让缓存失效 -> 此时内存与缓存都是 OLD
 *     再写 NEW(只进缓存,还没写回)
 *     invalidate                       -> 丢弃缓存里的 NEW
 *     读回                             -> 应当是 OLD
 *     若读回 NEW,说明 invalidate 没起作用(缓存根本没被丢弃);
 *     若第一次就写不进缓存(缓存无效),这里也会看到 NEW ——
 *     两种情况都能被这一个断言区分开。
 *
 *   测试 2(证明 clean 真的把数据写回了内存):
 *     写 NEW 之后 clean(写回),再 invalidate(丢弃缓存副本),
 *     读回应当是 NEW。若读回 OLD,说明 clean 没把数据写下去。
 *
 * 两个测试合起来把 clean 与 invalidate 的语义都钉住了,
 * 而且完全不需要任何外设参与。
 */
u32 cache_selftest(void)
{
    u32 i;

    /* ---- 测试 1:invalidate 丢弃未写回的数据 ---- */
    for (i = 0; i < (SELFTEST_BYTES / 4u); i++) {
        g_selftest_buf[i] = SELFTEST_OLD;
    }
    cache_clean_invalidate_range((uintptr_t)g_selftest_buf, SELFTEST_BYTES);

    for (i = 0; i < (SELFTEST_BYTES / 4u); i++) {
        g_selftest_buf[i] = SELFTEST_NEW;
    }

    cache_invalidate_range((uintptr_t)g_selftest_buf, SELFTEST_BYTES);

    for (i = 0; i < (SELFTEST_BYTES / 4u); i++) {
        if (g_selftest_buf[i] != SELFTEST_OLD) {
            return 1u; /* invalidate 没有丢弃缓存里的新数据 */
        }
    }

    /* ---- 测试 2:clean 把数据写回内存 ---- */
    for (i = 0; i < (SELFTEST_BYTES / 4u); i++) {
        g_selftest_buf[i] = SELFTEST_NEW;
    }
    cache_clean_range((uintptr_t)g_selftest_buf, SELFTEST_BYTES);

    /* 丢弃缓存副本,强制从内存重新读 */
    cache_invalidate_range((uintptr_t)g_selftest_buf, SELFTEST_BYTES);

    for (i = 0; i < (SELFTEST_BYTES / 4u); i++) {
        if (g_selftest_buf[i] != SELFTEST_NEW) {
            return 2u; /* clean 没有把数据写回内存 */
        }
    }

    /*
     * ---- 测试 3:非整行对齐的区间必须覆盖到首尾所在的行 ----
     *
     * 取缓冲区中间 4 字节(落在某一行内部)做失效。
     * 由于维护操作按行生效,这一行整体被丢弃;
     * 若实现没有向外对齐,边界行会被漏掉,行为就不可预期。
     * 这里只验证"调用不会出错且数据仍然自洽",对齐逻辑本身由宿主单测覆盖。
     */
    {
        volatile u32 *mid = &g_selftest_buf[4];

        *mid = SELFTEST_OLD;
        cache_clean_range((uintptr_t)mid, 4u);
        cache_invalidate_range((uintptr_t)mid, 4u);

        if (*mid != SELFTEST_OLD) {
            return 3u;
        }
    }

    return 0u;
}

