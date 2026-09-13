#pragma once

/*
 * 内核堆 —— M4-3
 *
 * ====================================================================
 * 为什么自己写,以及为什么做成这个形状
 * ====================================================================
 *
 * 源 OS 用外部依赖 liballoc-x86_64.a。ARM 侧自己写(与 M4-1/M4-2 同一决策):
 * 切分/合并/对齐/越界这些判定是纯逻辑,能在宿主机上把最容易错的地方钉死,
 * 而且不用把 Rust 构建链拉进 ninja 图。
 *
 * ⚠ **代价:这与源 OS 不是同一个分配器。** 已登记在 README 的退化实现清单,
 *   将来若要统一需另做一轮。
 *
 * 形状取自 M4-2 的两个结论:
 *   - **判定逻辑纯化** —— 本文件与 src/heap.c 不含 MMIO/CP15,
 *     底层内存通过 grow 回调从外部要来,于是宿主能直接跑;
 *   - **多花几位买断静默错误** —— 每个块头带魔数,于是"越界写"与
 *     "用野指针 free"能被**当场**抓住,而不是等到很久以后崩在别处。
 *
 * ====================================================================
 * 与 palloc 的分工
 * ====================================================================
 *
 *   palloc  按页发,最小单位 4KB;调用方是内核自己
 *   heap    按字节发,堆不够时向 palloc 要页
 *
 * 堆**不**自己调 palloc —— 它通过 grow 回调(见 heap_set_grow)。
 * 这样宿主测试可以喂一块合成内存,不需要真的有一个页分配器。
 */

#include <arch/types.h>

#define HEAP_ALIGN       8u

typedef enum
{
    HEAP_OK = 0,
    HEAP_ERR_NOT_INIT,
    HEAP_ERR_BAD_ARGS,
    HEAP_ERR_OOM,
    HEAP_ERR_BAD_POINTER,   /* 指针不在堆范围内 / 未对齐到负载起点 */
    HEAP_ERR_DOUBLE_FREE,   /* 该块已经是空闲的 */
    HEAP_ERR_BAD_MAGIC,     /* 块头魔数不对 —— 越界写或野指针 */
    HEAP_ERR_CORRUPT,       /* 链表自洽性被破坏(顺序/重叠/相邻双空闲) */
} heap_err_t;

typedef struct heap_block
{
    u32                 magic;
    u32                 state;
    size_t              size; /* 负载可用字节数(不含块头) */
    struct heap_block  *next; /* 按**地址顺序**的下一个块 */
    struct heap_block  *prev;
    u32                 pad;  /* 补齐到 HEAP_HEADER_SIZE */
} heap_block_t;

/*
 * 块头大小由 sizeof 推导,**不能硬编码 24**。
 *
 * 实测踩到:硬编码 24 时,宿主机(64 位,指针 8 字节)上 sizeof 是 40,
 * 于是负载指针压到结构体尾巴上 —— 板子上(32 位)恰好是 24 所以能跑,
 * 而**宿主测试测的是一套完全不同的布局**,等于白测。
 *
 * 现在两边各自自洽(host 40 / target 24,都向 8 对齐),
 * 并有用静态断言钉住"对齐后的头不小于结构体本身"。
 *
 * ⚠ 这两套布局**不同但没有 ABI 含义** —— 堆的内部布局不需要跨架构一致。
 *   需要一致的是 device_t / errno 那类对外契约,不是这里。
 */
#define HEAP_HEADER_RAW  ((u32)sizeof(heap_block_t))
#define HEAP_HEADER_SIZE ((HEAP_HEADER_RAW + (HEAP_ALIGN - 1u)) & ~(HEAP_ALIGN - 1u))

#define HEAP_MAGIC       0x4B524C48u /* "KRLH" */
#define HEAP_STATE_FREE  0xF4EEF4EEu
#define HEAP_STATE_USED  0x055ED055u

/*
 * 增长回调:堆不够时调用,期望它返回一段 >= need 字节的连续可写内存。
 * 返回 0 表示失败。
 *
 * ⚠⚠ 返回的内存**必须紧邻当前堆区的末尾**(即 base == 堆基址 + 当前 size)。
 *    因为本堆是一条贯穿整个区域的链表,非相邻的内存接不进同一条链表。
 *
 *    这个约束不是实现偷懒,而是设计选择:
 *    实测发现 palloc_alloc_pages() 返回的页**不保证与堆相邻**,
 *    所以"堆不够就向 palloc 要一页接上"这条路**根本走不通**。
 *
 * 因此实际用法是:**启动时一次性拿一大块连续内存**(kmain 里从 palloc
 * 预留),增长回调只作为可选路径。将来若真需要动态扩张,应当改成
 * **分段堆**(每段各自一条链表)—— 那是另一轮的工作,不在 M4-3。
 *
 * 回调的存在仍有价值:宿主测试可以喂一块相邻的合成内存来验证增长路径。
 */
typedef uintptr_t (*heap_grow_fn)(void *ctx, size_t need);

typedef struct
{
    uintptr_t    base;
    size_t       size;
    heap_block_t *head;

    heap_grow_fn grow;
    void        *grow_ctx;

    /* 统计 */
    size_t total_bytes;
    size_t used_bytes;
    size_t free_bytes;
    size_t largest_free;
    u32    alloc_count;
    u32    free_count;
    u32    fail_count;
    u32    grow_count;

    bool inited;
} heap_t;

/*
 * 用一段已经可写的内存初始化堆。
 *
 * base 必须 8 字节对齐;size 会被向下取整到 8 的倍数。
 * 最小的堆要能放下一个块头 + 最小负载,否则返回 BAD_ARGS。
 */
heap_err_t heap_init(heap_t *h, uintptr_t base, size_t size);

/* 设置增长回调(可选)。不设时堆用完即 OOM */
void heap_set_grow(heap_t *h, heap_grow_fn fn, void *ctx);

void   *heap_alloc(heap_t *h, size_t size);
void   *heap_calloc(heap_t *h, size_t count, size_t size);
void   *heap_realloc(heap_t *h, void *ptr, size_t size);

/*
 * 释放。会拒绝:空指针以外的非法指针、双重释放、块头被写坏。
 *
 * 不返回 void 是刻意的 —— 释放失败**必须**能被调用方看见。
 * 一个静默失败的 free 会表现为"内存越用越少"或"同一块被发两次",
 * 而两者都要很久以后才暴露。
 */
heap_err_t heap_free(heap_t *h, void *ptr);

/*
 * 遍历整个链表做自洽性检查:
 *   - 每个块头魔数正确;
 *   - 地址严格递增且不重叠;
 *   - **不存在相邻的两个空闲块**(相邻空闲说明合并漏了 —— 那会让堆
 *     逐渐碎片化,而"还能用"的表象会掩盖它);
 *   - 统计值与实际链表一致。
 *
 * 返回 HEAP_OK 或第一个发现的问题。
 */
heap_err_t heap_check(heap_t *h);

/* 当前最大可分配字节数(取最大空闲块) */
size_t heap_largest_free(heap_t *h);

/*
 * 自检:在**独立的合成实例**上穷尽各条错误路径。
 * 返回 0 = 全过;非 0 = 第几项失败。与 cache_selftest() / palloc_selftest() 同约定。
 */
u32 heap_selftest(void);
