#pragma once

/*
 * 内核虚拟内存细粒度映射 —— M4-4
 *
 * ====================================================================
 * 为什么需要它
 * ====================================================================
 *
 * 现在 MMU 只有 **1MB 段**映射。段映射有一个绕不过去的限制:一个段里的
 * 256 个 4KB 页**共享同一套属性** —— 要访问就全访问,要不访问就全不访问。
 * 于是 M4-5 要的 **guard page 做不出来**(栈溢出防护 = 栈下方留一页不映射)。
 *
 * 所以本模块的核心能力不是"能映射 4KB 页",而是:
 *
 *   ★ 把一段 1MB 的恒等映射就地拆成 L2 表,且**保持每一页的映射与属性不变** ★
 *
 * ====================================================================
 * ★ 属性必须用参数传递,不能搬运打包好的属性值 ★
 * ====================================================================
 *
 * 这是第一版失败的原因,值得写在最显眼的地方。
 *
 * 段描述符与小页描述符的**属性位布局完全不同**:
 *
 *   | 字段    | 段(L1)     | 小页(L2) |
 *   |---------|-----------|----------|
 *   | AP[1:0] | bit 11:10 | bit 5:4  |
 *   | AP[2]   | bit 15    | bit 9    |
 *   | TEX     | bit 14:12 | bit 8:6  |
 *   | S(共享) | bit 16    | bit 10   |
 *   | XN      | bit 4     | bit 0    |
 *
 * 实测后果:`MMU_ATTR_NORMAL_WB = 0x15DE6` 里 bit 16 是段的 S 位,
 * 而在小页描述符里 **bit 16 属于物理地址字段** —— 直接搬运会让拆段之后的
 * 每一页物理地址都莫名多出 0x10000。
 *
 * **所以本模块的接口收的是参数,不是打包值**,拆段时从段描述符
 * **解码回参数**再用小页构造函数重建。`mmu.h` 同时提供两个构造函数,
 * 正是为了这件事。
 *
 * ====================================================================
 * 分层
 * ====================================================================
 *
 * 本文件与 src/vmap.c **不含 MMIO / CP15**:页表就是普通内存,
 * L1 表与 L2 表池都由调用方提供。所以宿主编译器能直接编译并跑自检。
 *
 * ⚠ 指针与物理地址是**两个字段**,不能混用 —— 描述符里存 32 位物理地址,
 *   而 C 指针在宿主上是 64 位,截断再转回会得到野地址(实测:访问违例)。
 *   目标板上两者数值相同,但代码不能依赖这一点。
 *
 * TLB 维护(CP15)由调用方在改完之后自己做 —— 见各函数说明。
 */

#include <arch/types.h>

#define VMAP_PAGE_SHIFT  12u
#define VMAP_PAGE_SIZE   4096u
#define VMAP_L2_ENTRIES  256u /* 一张 L2 表覆盖 256 x 4KB = 1MB */
#define VMAP_L2_ALIGN    1024u

/* 小页描述符的 S(共享)位。段的是 MMU_ATTR_S_BIT(bit16),两者不同 */
#define VMAP_L2_S_BIT (1u << 10)

/* 映射属性**参数**。ap3 的 bit0-1 = AP[1:0],bit2 = AP[2] */
typedef struct
{
    u32  ap3;
    u32  tex;
    u32  c;
    u32  b;
    u32  domain; /* 只有一级段用得上 */
    bool shareable;
    bool xn;
} vmap_attr_t;

vmap_attr_t vmap_attr_normal(void);
vmap_attr_t vmap_attr_ro_xn(void);

typedef enum
{
    VMAP_OK = 0,
    VMAP_ERR_NOT_INIT,
    VMAP_ERR_BAD_ARGS,
    VMAP_ERR_ALIGN,
    VMAP_ERR_OUT_OF_RANGE,
    VMAP_ERR_ALREADY,
    VMAP_ERR_NOT_MAPPED,
    VMAP_ERR_NO_L2,
    VMAP_ERR_IS_SECTION,
} vmap_err_t;

typedef struct
{
    u32 *l1;         /* 一级页表(4096 项),由调用方提供 */
    u32 *l2_pool;    /* L2 表池:用来**写**表项 */
    u32  l2_pool_pa; /* L2 表池的物理地址:用来**填**描述符 */
    u32  l2_capacity;
    u32  l2_used;

    u32 va_begin; /* 本模块可以改的地址范围 —— 越界写入会破坏内核恒等映射 */
    u32 va_end;

    u32 stat_split;
    u32 stat_map;
    u32 stat_unmap;

    bool inited;
} vmap_t;

vmap_err_t vmap_init(vmap_t *v, u32 *l1, u32 *l2_pool, u32 l2_pool_pa, u32 l2_capacity, u32 va_begin,
                     u32 va_end);

u32 vmap_l1_index(u32 va);
u32 vmap_l2_index(u32 va);

/* 0 = 未映射;VMAP_RESULT_SECTION = 该区间还是段(需要先拆) */
#define VMAP_RESULT_SECTION 1u
u32 vmap_lookup(vmap_t *v, u32 va);

/* ★ 把 1MB 段就地拆成 L2 表(解码属性参数后用小页构造函数重建)★
 * ⚠ 调用方必须在返回后做 TLB 失效 */
vmap_err_t vmap_split_section(vmap_t *v, u32 va);

/* 映射 4KB 页;该区间还是段时自动先拆。已映射则拒绝(不静默覆盖) */
vmap_err_t vmap_map(vmap_t *v, u32 va, u32 pa, const vmap_attr_t *attr);
vmap_err_t vmap_unmap(vmap_t *v, u32 va);
vmap_err_t vmap_set_attr(vmap_t *v, u32 va, const vmap_attr_t *attr);

u32 *vmap_l2_of(vmap_t *v, u32 va);

/* 自检:返回 0 = 全过;非 0 = 第几项失败 */
u32 vmap_selftest(void);
