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
/* 按地址(MVA)维护的区间对齐                                          */
/* ------------------------------------------------------------------ */

/*
 * 一段已经按缓存行对齐的区间([start, end),end 不含)。
 */
typedef struct
{
    uintptr_t start;
    uintptr_t end;
} cache_range_t;

static inline bool cache_range_is_empty(const cache_range_t *range)
{
    return range->start >= range->end;
}

/*
 * 把 [addr, addr+size) 扩展成整行覆盖的范围。
 *
 * 为什么要扩展而不是直接用给定范围:
 *   缓存维护操作是**按行**生效的,而一行的所有权与请求范围无关。
 *   如果只对范围内的行做操作,那么范围两端若各有一行只有一部分落在里面,
 *   那一行的另一半数据就得不到处理 ——
 *   对 DMA 来说这意味着缓冲区首尾各有一小段没被写回(或没被失效),
 *   表现为"大部分时候对,偶尔错几个字节"。
 *
 *   所以必须**向外**取整:起点向下对齐到行边界,终点向上对齐。
 *   多处理一点是安全的(最坏是多刷几行相邻数据),
 *   少处理一点则一定出错。
 *
 * 边界情况:
 *   size == 0        -> 空区间(start == end),调用方据此跳过
 *   line_bytes == 0  -> 视为 1 字节粒度(几何未知时的保守退化)
 *   addr + size 溢出 -> 夹到地址空间上界,不绕回
 */
static inline cache_range_t cache_align_range(uintptr_t addr, size_t size, u32 line_bytes)
{
    cache_range_t range;
    u32           align = (line_bytes != 0u) ? line_bytes : 1u;
    uintptr_t     mask  = (uintptr_t)align - 1u;

    if (size == 0u) {
        range.start = addr;
        range.end   = addr;
        return range;
    }

    range.start = addr & ~mask;

    /*
     * 上界单独算并检查回绕。addr + size 在 32 位上完全可能绕回 0,
     * 那时 end < start,调用方会当成空区间直接跳过 ——
     * 于是整段缓冲区**一行都没被维护**,而且不会有任何报错。
     */
    {
        uintptr_t end = addr + size;

        if (end < addr) {
            end = ~(uintptr_t)0; /* 绕回:夹到上界 */
        }

        end = (end + mask) & ~mask;

        if (end < range.start) {
            end = ~(uintptr_t)0; /* 向上对齐本身绕回 */
        }

        range.end = end;
    }

    return range;
}

/* 区间跨越的缓存行数,便于诊断与上界检查 */
static inline u32 cache_range_lines(const cache_range_t *range, u32 line_bytes)
{
    u32 align = (line_bytes != 0u) ? line_bytes : 1u;

    if (cache_range_is_empty(range)) {
        return 0u;
    }

    return (u32)((range->end - range->start) / align);
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

/* 使能 L1 D-Cache 与 I-Cache(本阶段不动 L2,见 src/cache_hw.c) */
void cache_enable_l1(void);

/*
 * Coherency 前置条件:使能 SCU 并设置 ACTLR(SMP + 维护广播)。
 *
 * **必须在任何缓存使能之前调用。**
 *
 * ⚠ 漏掉它的症状极具迷惑性:地址转换照常、系统照常跑、SCTLR 的 C/I 位
 *   读回来都是 1,唯独缓存完全没有加速效果 —— 因为 DDR 是 Shareable 映射,
 *   而 Cortex-A9 在 SCU 未使能、ACTLR.SMP 未置位时不会把可共享访问放进 L1。
 *   见 src/cache_hw.c 的说明。本项目踩过一次,靠耗时基准才发现。
 */
void cortexa9_coherency_init(void);

/* 读回 SCU 控制寄存器与 ACTLR,用于诊断输出 */
u32 cortexa9_scu_status(void);
u32 cortexa9_actlr_status(void);

bool cache_dcache_enabled(void);
bool cache_icache_enabled(void);

/*
 * 内存密集循环的耗时(微秒),用于**证明缓存真的在起作用**。
 *
 * 存在的意义:光看 SCTLR 的 C 位读回 1 不能说明缓存有效 ——
 * 内存属性写错、或者缓存没覆盖到那条路径,系统照样"正常工作",
 * 只是白忙一场。跑一段内存密集循环,缓存生效前后耗时会差出数倍,
 * 这个差别骗不了人。
 */
u32 cache_benchmark_us(u32 passes);

/* ------------------------------------------------------------------ */
/* 按地址(MVA)的区间维护 —— DMA 真正要用的东西                        */
/* ------------------------------------------------------------------ */

/*
 * 这三个是驱动做 DMA 时唯一需要关心的缓存接口。用法约定:
 *
 *   设备 -> 内存(DMA 写入后,CPU 要读)
 *       cache_invalidate_range(...)    丢弃 CPU 缓存里的旧副本,
 *                                       强制从内存重新读
 *   内存 -> 设备(CPU 写完后,DMA 要读)
 *       cache_clean_range(...)         把脏行写回到一致性点
 *   双向(缓冲区会被双方交替使用)
 *       cache_clean_invalidate_range(...)
 *
 * ⚠ 顺序不能反:
 *   - "先 invalidate 再写"会丢掉还没写回的数据;
 *   - "只 clean 不 invalidate"用于 RX 缓冲会让 CPU 继续读到缓存里的旧内容。
 *
 * ⚠ 区间会**向外对齐到整行**(见 cache_align_range 的说明),
 *   所以相邻数据也可能被一并写回/丢弃 —— 调用方不应把
 *   不该被丢弃的数据紧挨着 DMA 缓冲区放。
 *
 * 当前实现只覆盖 L1。L2(PL310)尚未使能,等 M2-5d 处理它之后,
 * 这里的语义需要重新审视:L2 在 PL310 上由它自己的寄存器维护。
 */
void cache_clean_range(uintptr_t addr, size_t size);
void cache_invalidate_range(uintptr_t addr, size_t size);
void cache_clean_invalidate_range(uintptr_t addr, size_t size);

/* 当前 L1 D-Cache 的行大小(字节)。缓存未使能时也返回硬件的真实值 */
u32 cache_line_bytes(void);

/*
 * 板上自检:验证 clean / invalidate 真的按语义工作。
 *
 * 不用 DMA 也能验证,靠的是"缓存与内存是两份副本"这个事实:
 *   - 写入数据后 **invalidate**,若之后读回的是**旧值**,
 *     说明缓存里确实存着未写回的新值(缓存有效),且 invalidate 真的丢弃了它;
 *   - 写入数据后 **clean**,再 invalidate,读回**新值**,
 *     说明 clean 真的把数据写回了内存。
 *
 * 返回 0 表示全部通过,非 0 是失败项编号。
 * 详见 src/cache_hw.c 的实现注释。
 */
u32 cache_selftest(void);

/* ------------------------------------------------------------------ */
/* PL310 L2 寄存器映射                                                 */
/* ------------------------------------------------------------------ */

/*
 * ⚠ Zynq 上这颗 PL310 的寄存器偏移与**通用 PL310 手册不一致**,
 *   而且不一致的恰好是两个"按 Way"与"按地址"的操作 ——
 *   用错不会报错,只会静默地做错事(比如把"失效整条 way"当成
 *   "清洗一行地址")。这类错误在 DMA 场景表现为随机数据损坏。
 *
 *   对照(通用 PL310 TRM vs 本芯片):
 *     偏移     通用 PL310              本芯片(Zynq)
 *     0x0770   Invalidate Line by PA   Invalidate by PA      一致
 *     0x077C   Clean&Invalidate by PA  **Invalidate by Way**
 *     0x07B0   Clean Line by PA        Clean by PA           一致
 *     0x07F0   Invalidate Way          **Clean&Invalidate by PA**
 *     0x07FC   Clean&Invalidate Way    Clean&Invalidate Way  一致
 *
 *   也就是说 0x77C 与 0x7F0 的功能在两者之间是对调的。
 *   下表的取值来自 AMD/Xilinx standalone BSP 的
 *   arm/cortexa9/xl2cc.h —— 那是随这颗芯片一起出货的定义,以它为准。
 */
#define L2CC_ID            0x0000u /* Cache ID */
#define L2CC_TYPE          0x0004u /* Cache Type */
#define L2CC_CONTROL       0x0100u /* 使能位在 bit0 */
#define L2CC_AUX_CONTROL   0x0104u
#define L2CC_TAG_RAM_CTRL  0x0108u
#define L2CC_DATA_RAM_CTRL 0x010Cu
#define L2CC_ISR           0x021Cu /* 原始中断状态 */
#define L2CC_IAR           0x0220u /* 中断清除 */
#define L2CC_SYNC          0x0730u /* 写入任意值即发起同步;读回 0 表示完成 */
#define L2CC_INV_PA        0x0770u /* 按物理地址失效一行 */
#define L2CC_INV_WAY       0x077Cu /* 按 Way 失效(写 16 位 way 位图) */
#define L2CC_CLEAN_PA      0x07B0u /* 按物理地址清洗一行 */
#define L2CC_CLEAN_WAY     0x07BCu
#define L2CC_INV_CLN_PA    0x07F0u /* 按物理地址清洗并失效一行 */
#define L2CC_INV_CLN_WAY   0x07FCu
#define L2CC_DEBUG_CTRL    0x0F40u /* 勘误规避用:关闭写回与行填充 */

#define L2CC_CONTROL_ENABLE 0x00000001u

/* 写入 DEBUG_CTRL 的值:0x3 = 关闭写回与行填充,0x0 = 恢复 */
#define L2CC_DEBUG_DISABLE_WB_AND_LINEFILL 0x3u
#define L2CC_DEBUG_ENABLE_WB_AND_LINEFILL  0x0u

/* 按 Way 操作时用 16 位位图表示哪些 way 参与 */
#define L2CC_ALL_WAYS 0xFFFFu

/* PL310 在 Zynq 上是 512KB / 8 路 / 32 字节行 */
#define L2CC_ZYNQ7020_BYTES 0x00080000u

/*
 * 下列三个值来自 Xilinx boot.S,是随这块芯片验证过的配置,**不要自己推算**:
 *
 *   AUX_CONTROL    0x72360000  预取全开、替换策略、奇偶校验等默认位
 *   TAG_RAM_LAT   0x0111       Tag RAM 延迟
 *   DATA_RAM_LAT  0x0121       Data RAM 延迟
 *
 * 两个延迟值尤其不能猜:它们取决于芯片的时序实现,写错不会报错,
 * 只会在高负载下偶发缓存错误。
 */
#define L2CC_AUX_CONTROL_DEFAULT 0x72360000u
#define L2CC_TAG_RAM_LATENCY     0x0111u
#define L2CC_DATA_RAM_LATENCY    0x0121u

/* ------------------------------------------------------------------ */
/* L2 硬件接口(实现在 src/cache_hw.c)                                 */
/* ------------------------------------------------------------------ */

/*
 * 配置并使能 PL310。
 *
 * ⚠ 必须在 L1 缓存使能**之后**调用,顺序与 Xilinx boot.S 一致:
 *   它也是先写 SCTLR 打开 MMU 与 D-Cache,再初始化 L2。
 *
 * 流程:关闭 -> 写辅助控制/延迟 -> 整块失效 -> 同步
 *       -> 配 SLCR 的 L2 RAM -> 使能
 */
void l2_cache_init(void);

/* 等待 L2 维护操作完成(写 SYNC 后轮询读回 0) */
void l2_cache_sync(void);

/* 整块失效(按 Way)。只在单核启动阶段、使能之前用 */
void l2_cache_invalidate_all(void);

/*
 * 整块写回(按 Way)。
 *
 * ⚠ 这里是"只清洗"而不是"清洗并失效":后者(0x7FC)正是 PL310 勘误 727915
 *   涉及的那条路径。要"刷新整个 L2"时,应当像 588369 的规避一样
 *   拆成两步 —— 先 clean_all() 再 invalidate_all()。
 */
void l2_cache_clean_all(void);

/*
 * 关闭 / 使能 L2 的使能位。
 *
 * disable() 会**先写回再失效**再清使能位:直接清使能位会丢掉尚未写回的
 * 脏行,表现为"某些内存写入凭空消失",而且只在关 L2 的那一刻发生。
 *
 * 这两个接口的用途是让上层能做"开/关 L2 的对照实验" ——
 * 光看控制寄存器读回 1,并不能说明 L2 真的在缓存任何东西,
 * 必须用超出 L1 容量、又能装进 L2 的工作集去测出差别。
 */
void l2_cache_disable(void);
void l2_cache_enable(void);

/* 在指定工作集上跑内存密集循环,返回耗时(微秒) */
u32 cache_bench_us_on(const volatile u32 *buf, u32 words, u32 passes);

/* L2 有效性实验用的 128KB 工作集(超出 L1、装得进 L2) */
const volatile u32 *cache_l2_bench_buf(void);
u32 cache_l2_bench_words(void);

bool l2_cache_is_enabled(void);

/* 诊断:Cache ID / Type / 控制寄存器 */
u32 l2_cache_id(void);
u32 l2_cache_type(void);
u32 l2_cache_control(void);

/*
 * 按物理地址的 L2 区间维护。
 *
 * 这三个是在 L1 的对应操作之后**追加**调用的(见 cache_clean_range 等),
 * 调用方一般不需要直接用它。
 *
 * ⚠ 内含 PL310 勘误 588369 的规避:该勘误使得"清洗并失效"不会失效
 *   已经干净的行,所以这里把它拆成"先清洗、再失效"两步做 ——
 *   单条 INV_CLN 在受影响的行上会留下本该被丢弃的干净数据。
 */
void l2_cache_clean_range(uintptr_t addr, size_t size);
void l2_cache_invalidate_range(uintptr_t addr, size_t size);
void l2_cache_clean_invalidate_range(uintptr_t addr, size_t size);
