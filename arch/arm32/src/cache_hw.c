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
/* 几何诊断                                                             */
/* ------------------------------------------------------------------ */

u32 cache_total_bytes(bool instruction, u32 level)
{
    return cache_discover(instruction, level).total_bytes;
}
