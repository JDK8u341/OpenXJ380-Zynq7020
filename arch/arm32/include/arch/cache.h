#pragma once

/*
 * ARMv7-A 缓存几何与维护 —— 纯逻辑部分
 *
 * 与 src/cache_hw.c 的分工:
 *   本文件     几何解码与 CP15 参数编码。**不含任何内联汇编**,
 *              所以宿主编译器能直接编译,单测见 tests/test_arm32_cache.py。
 *   cache_hw.c CP15 访问、按 set/way 的整块操作、PL310 L2 配置。
 *
 * 为什么几何解码值得单独抽出来测:
 *
 *   ARM 的 CCSIDR 三个字段全部是"减一"或"减四"存储的:
 *     LineSize      = log2(行字节数) - 4
 *     Associativity = 路数   - 1
 *     NumSets       = 组数   - 1
 *
 *   少加那个 1 不会报错 —— 只会让失效循环少覆盖一行/一路,
 *   于是缓存里残留的脏行在之后被写回,静默覆盖正确数据。
 *   这类错误在板上表现为偶发数据损坏,是本项目最不想遇到的一类 bug。
 *
 * 本板的实测值(Cortex-A9)用作单测基准:
 *   L1 D-Cache 32KB 4 路 32 字节行 -> sets=256, ways=4, CCSIDR=0x701FE019
 *   L1 I-Cache 32KB 4 路 32 字节行 -> 同上
 */

#include <arch/types.h>

/* ------------------------------------------------------------------ */
/* CLIDR(Cache Level ID Register)                                     */
/* ------------------------------------------------------------------ */

/*
 * 每 3 位描述一级缓存的类型。
 *
 * ⚠ Cortex-A9 的 CLIDR **只报告 L1**。本板实测:
 *     CLIDR = 0x09200003   (LoC=1, LoUIS=1, 第 0 级 = 分离缓存)
 *   只有低 3 位描述了缓存,第 1 级起全是"无缓存" ——
 *   而 L2(PL310)根本不是 CP15 视野里的东西,它有自己的一套寄存器
 *   (0xF8F02000)。
 *
 *   所以"用 CP15 按 set/way 循环失效所有级别"这个通用写法,
 *   在本芯片上只会失效 L1;L2 必须单独处理。
 *   这个区别值得记住:通用写法看起来更"完备",实际覆盖不到真正的大头。
 */
#define CACHE_CLIDR_TYPE_SHIFT(level) ((level) * 3u)
#define CACHE_CLIDR_TYPE_MASK         0x7u

#define CACHE_TYPE_NONE      0u /* 该级无缓存 */
#define CACHE_TYPE_ICACHE    1u /* 只有指令缓存 */
#define CACHE_TYPE_DCACHE    2u /* 只有数据缓存 */
#define CACHE_TYPE_SEPARATE  3u /* 指令与数据分离 */
#define CACHE_TYPE_UNIFIED   4u /* 统一缓存(指令与数据共用) */

/*
 * LoC(Level of Coherency)与 LoUIS(Level of Unification Inner Shareable)。
 *
 * 本板实测两者都是 1(即最低一级就是一致性点)。
 * 这两个字段是 1 基的:0 表示"没有缓存满足该条件"。
 */
#define CACHE_CLIDR_LOC_SHIFT   24u
#define CACHE_CLIDR_LOUIS_SHIFT 27u

/* ------------------------------------------------------------------ */
/* CCSIDR(Cache Size ID Register)                                     */
/* ------------------------------------------------------------------ */

#define CACHE_CCSIDR_LINE_SHIFT  0u
#define CACHE_CCSIDR_LINE_MASK   0x7u
#define CACHE_CCSIDR_WAYS_SHIFT  3u
#define CACHE_CCSIDR_WAYS_MASK   0x3FFu
#define CACHE_CCSIDR_SETS_SHIFT  13u
#define CACHE_CCSIDR_SETS_MASK   0x7FFFu

/* CSSELR:选择要查询哪一级、哪种缓存 */
#define CACHE_CSSELR_LEVEL_SHIFT 1u
#define CACHE_CSSELR_IND_BIT     (1u << 0) /* 0 = 数据缓存,1 = 指令缓存 */

typedef struct
{
    u32 line_bytes;  /* 缓存行字节数 */
    u32 ways;        /* 相联度(路数) */
    u32 sets;        /* 组数 */
    u32 total_bytes; /* 总容量 = ways × sets × line_bytes */
} cache_geometry_t;

/* 从 CLIDR 取出某级(0 基)的缓存类型 */
static inline u32 cache_level_type(u32 clidr, u32 level)
{
    return (clidr >> CACHE_CLIDR_TYPE_SHIFT(level)) & CACHE_CLIDR_TYPE_MASK;
}

/* 该级是否含数据缓存(供按 set/way 失效使用) */
static inline bool cache_level_has_data(u32 clidr, u32 level)
{
    u32 type = cache_level_type(clidr, level);

    /* 数据缓存(2)、分离(3)、统一(4)都含数据 */
    return type == CACHE_TYPE_DCACHE || type == CACHE_TYPE_SEPARATE || type == CACHE_TYPE_UNIFIED;
}

static inline u32 cache_loc(u32 clidr)
{
    return (clidr >> CACHE_CLIDR_LOC_SHIFT) & CACHE_CLIDR_TYPE_MASK;
}

static inline u32 cache_louis(u32 clidr)
{
    return (clidr >> CACHE_CLIDR_LOUIS_SHIFT) & CACHE_CLIDR_TYPE_MASK;
}

/*
 * 需要按 set/way 维护的最高级别。
 *
 * 取 LoC 与 LoUIS 的较小者:超过一致性点的级别由硬件或别的机制负责,
 * 软件再去动它反而会破坏一致性。这是 ARM ARM 给出的标准做法
 * (B2.2.4 "Cache maintenance operations")。
 */
static inline u32 cache_setway_max_level(u32 clidr)
{
    u32 loc   = cache_loc(clidr);
    u32 louis = cache_louis(clidr);

    return (loc < louis) ? loc : louis;
}

/*
 * 解码 CCSIDR。
 *
 * ⚠ 三个字段都要 +1 / +4,见文件头的说明。
 */
static inline cache_geometry_t cache_decode_ccsidr(u32 ccsidr)
{
    cache_geometry_t geo;

    /* LineSize 存的是 log2(行字节数) - 4 */
    geo.line_bytes = 1u << (((ccsidr >> CACHE_CCSIDR_LINE_SHIFT) & CACHE_CCSIDR_LINE_MASK) + 4u);

    /* 路数与组数都是"减一"存储 */
    geo.ways = ((ccsidr >> CACHE_CCSIDR_WAYS_SHIFT) & CACHE_CCSIDR_WAYS_MASK) + 1u;
    geo.sets = ((ccsidr >> CACHE_CCSIDR_SETS_SHIFT) & CACHE_CCSIDR_SETS_MASK) + 1u;

    geo.total_bytes = geo.ways * geo.sets * geo.line_bytes;

    return geo;
}

/*
 * 构造按 set/way 操作时传给 CP15 的值。
 *
 * 布局:bits[31:30] = 路号,bits[13] 起 = 组号。
 * 这两个字段的位置没有任何"看起来显然"的地方,写错就会失效到错误的
 * 组/路 —— 而循环照样跑完、不报任何错,只是缓存里留下一片没被清理的脏行。
 */
static inline u32 cache_setway_value(u32 way, u32 set)
{
    return (way << 30) | (set << 13);
}

/* 遍历整个缓存需要的迭代次数,便于调用方做上界检查 */
static inline u32 cache_setway_iterations(const cache_geometry_t *geo)
{
    return geo->ways * geo->sets;
}

/* ------------------------------------------------------------------ */
/* 硬件侧(实现在 src/cache_hw.c,只能上板验证)                        */
/* ------------------------------------------------------------------ */

/*
 * 这几个只存在于 ARM 侧,不能放进本文件的内联实现 ——
 * 本文件要被宿主机编译器直接编译来做单测,
 * 一旦引入 mcr/mrc 内联汇编就编不过了。
 */

/* 查某一级、某种缓存的几何。会把 CSSELR 复原 */
cache_geometry_t cache_discover(bool instruction, u32 level);

/* 总容量(字节),失败时返回 0 */
u32 cache_total_bytes(bool instruction, u32 level);

/*
 * 按 set/way 的整块数据缓存操作。
 *
 * ⚠ 只在**单核启动阶段、使能缓存之前**使用。按 set/way 的操作不区分
 *   数据归属,多核运行时执行会丢掉别的核尚未写回的脏行。
 *   详见 src/cache_hw.c 文件头的说明。
 */
void cache_dcache_invalidate_all(void);
void cache_dcache_clean_all(void);
void cache_dcache_clean_invalidate_all(void);

/*
 * 使能缓存前的统一清理:失效 I-Cache、分支预测器、D-Cache。
 *
 * 调用之后应当**紧接着**使能缓存,中间不要再插会分配缓存行的访存 ——
 * 否则又引入了新的未知内容,这一步就白做了。
 */
void cache_invalidate_before_enable(void);
