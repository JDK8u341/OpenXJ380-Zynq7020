#pragma once

/*
 * 物理页分配器 —— M4-2
 *
 * ====================================================================
 * 为什么现在就需要它
 * ====================================================================
 *
 * 后面三件事都要从它拿页:
 *   - M4-4 的二级页表(现在只有 1MB 段,做不出 guard page);
 *   - M4-5 的任务内核栈;
 *   - M4A-1 的 VFS / FATFS 缓冲区与设备 DMA 缓冲。
 *
 * x86 侧用外部依赖 liballoc-x86_64.a。ARM 侧**自己写**,理由与 M4-1 一致:
 * 切分/合并/对齐/越界这些判定是纯逻辑,能在宿主机上把最容易错的地方钉死,
 * 而且不用把 Rust 构建链拉进 ninja 图。代价是与 x86 不是同一个分配器。
 *
 * ====================================================================
 * 为什么是 2 bit/页而不是 1 bit
 * ====================================================================
 *
 * 一位只能表达"占用/空闲"两态,于是:
 *
 *   - **双重释放检测不出来** —— 释放一个已经空闲的页会静默变成 no-op;
 *   - **分不清"已分配"与"保留"** —— 内核镜像、页表、栈这些区域
 *     必须永远不可释放,而 1 bit 无法阻止有人把它们 free 掉。
 *
 * 这两类错误的共同点是**不会当场出错**,而是在很久以后以一个完全
 * 无关的症状崩掉。所以多花一位买断它们是很划算的。
 *
 *   00 = FREE(空闲,可分配)
 *   01 = USED(已分配,可释放)
 *   10 = RESERVED(保留,永远不可释放)
 *   11 = 非法(不会出现;出现即为分配器自身写坏了)
 *
 * ====================================================================
 * 分层
 * ====================================================================
 *
 * 本文件与 src/palloc.c **不含任何 MMIO / CP15 / 内联汇编**,
 * 也不自己申请 bitmap 存储 —— bitmap 由调用方提供。
 * 于是宿主编译器能直接编译它并跑穷尽的错误路径测试。
 *
 * "物理内存从哪来"是调用方的事(kmain 用链接符号 _kernel_end 算),
 * 本模块只负责"给定一段区间,把它管好"。
 */

#include <arch/types.h>

#define PALLOC_PAGE_SHIFT 12u
#define PALLOC_PAGE_SIZE  (1u << PALLOC_PAGE_SHIFT)

/* 每页 2 bit */
#define PALLOC_BITS_PER_PAGE 2u

/*
 * 上限。取 1GB / 4KB = 262144 页 —— 本板 DDR 是 0x3FF00000 约 1020MB,
 * 正好落在这个上限内。定成编译期常量是为了让 bitmap 能在 .bss 里静态开出来,
 * 不必自己给自己分配存储(那会变成先有鸡还是先有蛋)。
 */
#define PALLOC_MAX_PAGES (262144u)

/* 装下 PALLOC_MAX_PAGES 个页状态所需的 u32 字数 */
#define PALLOC_BITMAP_WORDS ((PALLOC_MAX_PAGES * PALLOC_BITS_PER_PAGE + 31u) / 32u)

typedef enum
{
    PALLOC_OK = 0,
    PALLOC_ERR_NOT_INIT,     /* 还没初始化 */
    PALLOC_ERR_ALIGN,        /* 地址未按页对齐 */
    PALLOC_ERR_RANGE,        /* 地址不在池范围内 */
    PALLOC_ERR_DOUBLE_FREE,  /* 释放了一个已经空闲的页 */
    PALLOC_ERR_RESERVED,     /* 试图释放/分配的页是保留页 */
    PALLOC_ERR_ALREADY_USED, /* 试图保留一个已经占用的页 */
    PALLOC_ERR_OOM,          /* 没有足够连续空闲页 */
    PALLOC_ERR_TOO_MANY,     /* 页数超过 PALLOC_MAX_PAGES */
    PALLOC_ERR_BAD_ARGS,     /* 传参非法(空指针、size 为 0 等) */
} palloc_err_t;

typedef struct
{
    uintptr_t base;       /* 池的物理起始地址(必须页对齐) */
    u32       page_count; /* 池内页数 */
    u32       free_pages;
    u32       used_pages;
    u32       reserved_pages;
    u32       peak_used;  /* 历史最高 used,用于观察泄漏趋势 */

    u32 *bitmap;          /* 由调用方提供,长度须 >= PALLOC_BITMAP_WORDS */
    u32  bitmap_words;

    bool inited;
} palloc_t;

/*
 * 初始化。bitmap 由调用方提供(通常是一块 .bss 数组),本函数只写它。
 *
 * 全部页初始为 FREE —— 调用方随后用 palloc_reserve() 把内核镜像、
 * 页表、栈等区域标成 RESERVED。
 *
 * ⚠ 先全 FREE 再逐个 reserve,而不是"默认保留、逐个释放":
 *   前者漏了一个 reserve 会表现为"内核内存被分配出去了"(危险但可见),
 *   后者漏了一个会让整片内存永远用不上(浪费但安全)。
 *   这里选前者,因为它的失败模式更容易被发现 —— 但前提是调用方
 *   **必须** 把该保留的都保留,所以 kmain 里那份保留清单是安全关键。
 */
palloc_err_t palloc_init(palloc_t *p, uintptr_t base, size_t size, u32 *bitmap, u32 bitmap_words);

/*
 * 把一段区间标为保留(不可释放)。
 *
 * 区间不必页对齐:会向外扩到整页 —— 宁可多保留一点,
 * 也不能因为"少了半个页"把内核数据当成空闲页发出去。
 */
palloc_err_t palloc_reserve(palloc_t *p, uintptr_t addr, size_t size);

/* 分配一页。成功时把物理地址写入 *out。 */
palloc_err_t palloc_alloc(palloc_t *p, uintptr_t *out);

/* 分配**连续** n 页(将来给 DMA 缓冲用)。成功时写入首地址。 */
palloc_err_t palloc_alloc_pages(palloc_t *p, u32 n, uintptr_t *out);

/*
 * 释放一页。
 *
 * 会拒绝:未对齐、越界、双重释放、保留页 —— 各自返回不同的错误码。
 * 这些拒绝不是"防御性编程",而是本模块存在的理由之一:
 * 静默接受它们会让 bug 推迟到很久以后才以无关症状出现。
 */
palloc_err_t palloc_free(palloc_t *p, uintptr_t addr);

/* 查某一页当前状态。越界或未对齐返回 0xFF(非法值) */
u32 palloc_state(palloc_t *p, uintptr_t addr);

/* 状态的取值 */
#define PALLOC_STATE_FREE 0u
#define PALLOC_STATE_USED 1u
#define PALLOC_STATE_RSVD 2u
#define PALLOC_STATE_BAD 0xFFu

/*
 * 自检:在**独立的合成实例**上穷尽各条错误路径。
 *
 * 为什么用合成实例而不是真实池:
 *   - 错误路径(双重释放、越界、OOM)需要精确控制状态,在真实池上做
 *     会受当前用量影响,测出来不确定;
 *   - 而且它会真的吃掉真实页,把一个只读的检查变成有副作用的操作。
 *
 * 返回 0 = 全部通过;非 0 = 第几项失败(便于定位)。
 * 与 cache_selftest() 同一套约定。
 */
u32 palloc_selftest(void);
