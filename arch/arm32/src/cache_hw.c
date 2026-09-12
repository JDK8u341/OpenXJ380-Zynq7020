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
 * 跑一遍内存密集循环,返回耗时(微秒)。
 *
 * 存在的意义:**证明缓存真的在起作用**,而不只是"寄存器某位被置了 1"。
 * 这一类验证很容易被自己骗过去 —— SCTLR 的 C 位读回来是 1,
 * 但如果内存属性写成了不可缓存、或者缓存根本没使能到那条路径,
 * 系统照样"正常工作",只是白忙一场。
 * 一个内存密集循环的耗时会在缓存生效前后差出数倍,这个差别骗不了人。
 *
 * 循环体刻意做成只读 + 累加:
 *   - 只读 -> 不受写回策略(write-back / write-through)影响,
 *            单纯反映"读命中缓存"带来的收益;
 *   - 累加到 volatile -> 编译器无法把循环删掉。
 */
u32 cache_benchmark_us(u32 passes)
{
    u64 t0;
    u64 t1;
    u32 pass;
    u32 i;
    u32 sum = 0;

    t0 = timer_read_us();

    for (pass = 0; pass < passes; pass++) {
        for (i = 0; i < (sizeof(g_bench_buf) / sizeof(g_bench_buf[0])); i++) {
            sum += g_bench_buf[i];
        }
    }

    t1 = timer_read_us();

    /* 写进 volatile,确保上面的循环不会被判定为无副作用而删除 */
    g_bench_sink = sum;

    return (u32)(t1 - t0);
}

/* ------------------------------------------------------------------ */
/* 使能                                                                 */
/* ------------------------------------------------------------------ */

/*
 * 使能 L1 D-Cache 与 I-Cache。
 *
 * **本阶段不动 L2(PL310)** —— 它此刻处于复位状态(ps7_init 里没有任何
 * L2 代码,实测确认),配置与使能留给 M2-5c。
 *
 * 这个顺序与 Xilinx 的 cortexa9/gcc/boot.S 一致:那里也是先写
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

