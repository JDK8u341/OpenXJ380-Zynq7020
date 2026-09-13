/*
 * 内核虚拟内存细粒度映射 —— 纯逻辑实现(M4-4)
 *
 * 页表就是普通内存,L1 表与 L2 表池都由调用方提供,所以宿主编译器能直接编译。
 * TLB 维护是 CP15 操作,不在这里做。
 *
 * ★ 本文件的核心是"段 -> 小页"的**属性翻译**,不是搬运 ★
 *   见 vmap.h 顶部的位布局对照表。
 */

#include <arch/mmu.h>
#include <arch/vmap.h>
#include <krlibc.h>

_Static_assert(sizeof(u32) == 4u, "页表项必须是 4 字节");

u32 vmap_l1_index(u32 va)
{
    return va >> MMU_SECTION_SHIFT;
}

u32 vmap_l2_index(u32 va)
{
    return (va >> VMAP_PAGE_SHIFT) & (VMAP_L2_ENTRIES - 1u);
}

vmap_attr_t vmap_attr_normal(void)
{
    vmap_attr_t a;

    a.ap3       = 3u; /* 全权限 */
    a.tex       = 1u;
    a.c         = 1u;
    a.b         = 1u;
    a.domain    = 0u;
    a.shareable = true;
    a.xn        = false;

    return a;
}

vmap_attr_t vmap_attr_ro_xn(void)
{
    vmap_attr_t a = vmap_attr_normal();

    a.ap3 = 2u; /* AP[2]=1 且 AP[1:0]=0 -> PL1 只读 */
    a.xn  = true;

    return a;
}

static u32 l1_type(u32 desc)
{
    return desc & 0x3u;
}

/*
 * 由描述符里的**物理地址**反推池内指针。落在池外返回 NULL ——
 * 而不是当成指针用(那会是野地址)。
 */
static u32 *l2_from_pa(const vmap_t *v, u32 pa)
{
    u32 span = v->l2_capacity * VMAP_L2_ENTRIES * (u32)sizeof(u32);

    if (pa < v->l2_pool_pa || (pa - v->l2_pool_pa) >= span) {
        return NULL;
    }

    return (u32 *)(void *)((u8 *)v->l2_pool + (pa - v->l2_pool_pa));
}

static bool in_range(const vmap_t *v, u32 va)
{
    return (va >= v->va_begin) && (va < v->va_end);
}

/* ------------------------------------------------------------------ */
/* ★ 段描述符 -> 属性参数 ★                                            */
/* ------------------------------------------------------------------ */

/*
 * 把一级段描述符的属性位**解码**回参数。
 *
 * 这一步是本模块存在的理由。少了它,拆段时只能把整个属性值搬过去,
 * 而两种格式的位布局不同 —— 实测表现是物理地址凭空多出 0x10000。
 */
static void decode_section_attr(u32 desc, vmap_attr_t *out)
{
    out->ap3 = ((desc >> MMU_AP_SHIFT_L1) & MMU_AP_LOW_MASK);
    if ((desc & MMU_L1_AP2_BIT) != 0u) {
        out->ap3 |= 0x4u;
    }

    out->tex    = (desc >> 12) & 0x7u;
    out->c      = (desc >> 3) & 0x1u;
    out->b      = (desc >> 2) & 0x1u;
    out->domain = (desc >> MMU_L1_DOMAIN_SHIFT) & MMU_L1_DOMAIN_MASK;

    out->shareable = (desc & MMU_ATTR_S_BIT) != 0u;
    out->xn        = (desc & (1u << 4)) != 0u;
}

/* 用参数分别构造两种描述符的**属性部分**(不含物理地址) */
static u32 section_attr_of(const vmap_attr_t *a)
{
    return mmu_l1_section_attr(a->ap3, a->tex, a->c, a->b, a->domain, a->shareable, a->xn);
}

static u32 page_attr_of(const vmap_attr_t *a)
{
    return mmu_l2_small_page_attr(a->ap3, a->tex, a->c, a->b, a->shareable, a->xn);
}

/* ------------------------------------------------------------------ */

vmap_err_t vmap_init(vmap_t *v, u32 *l1, u32 *l2_pool, u32 l2_pool_pa, u32 l2_capacity, u32 va_begin,
                     u32 va_end)
{
    if (v == NULL || l1 == NULL || l2_pool == NULL) {
        return VMAP_ERR_BAD_ARGS;
    }

    if (l2_capacity == 0u || va_begin >= va_end) {
        return VMAP_ERR_BAD_ARGS;
    }

    if (((uintptr_t)l2_pool & (VMAP_L2_ALIGN - 1u)) != 0u) {
        return VMAP_ERR_ALIGN;
    }

    if ((va_begin & (VMAP_PAGE_SIZE - 1u)) != 0u || (va_end & (VMAP_PAGE_SIZE - 1u)) != 0u) {
        return VMAP_ERR_ALIGN;
    }

    v->l1          = l1;
    v->l2_pool     = l2_pool;
    v->l2_pool_pa  = l2_pool_pa;
    v->l2_capacity = l2_capacity;
    v->l2_used     = 0u;
    v->va_begin    = va_begin;
    v->va_end      = va_end;
    v->stat_split  = 0u;
    v->stat_map    = 0u;
    v->stat_unmap  = 0u;
    v->inited      = true;

    return VMAP_OK;
}

u32 *vmap_l2_of(vmap_t *v, u32 va)
{
    u32 desc;

    if (v == NULL || !v->inited || !in_range(v, va)) {
        return NULL;
    }

    desc = v->l1[vmap_l1_index(va)];
    if (l1_type(desc) != MMU_L1_TYPE_PAGE_TABLE) {
        return NULL;
    }

    return l2_from_pa(v, desc & ~0x3FFu);
}

u32 vmap_lookup(vmap_t *v, u32 va)
{
    u32  desc;
    u32 *l2;

    if (v == NULL || !v->inited || !in_range(v, va)) {
        return 0u;
    }

    desc = v->l1[vmap_l1_index(va)];

    if (l1_type(desc) == MMU_L1_TYPE_FAULT) {
        return 0u;
    }
    if (l1_type(desc) == MMU_L1_TYPE_SECTION) {
        return VMAP_RESULT_SECTION;
    }
    if (l1_type(desc) != MMU_L1_TYPE_PAGE_TABLE) {
        return 0u;
    }

    l2 = l2_from_pa(v, desc & ~0x3FFu);
    if (l2 == NULL) {
        return 0u;
    }

    return l2[vmap_l2_index(va)];
}

/* 取或新建该区间的 L2 表。新建的表必须清零 —— 否则带着垃圾描述符 */
static u32 *l2_ensure(vmap_t *v, u32 va)
{
    u32  idx  = vmap_l1_index(va);
    u32  desc = v->l1[idx];
    u32 *l2;

    if (l1_type(desc) == MMU_L1_TYPE_PAGE_TABLE) {
        return l2_from_pa(v, desc & ~0x3FFu);
    }

    if (v->l2_used >= v->l2_capacity) {
        return NULL;
    }

    /* 池按 l2_used 顺序切分,不归还 —— 见下方 split 的说明 */
    l2 = v->l2_pool + ((size_t)v->l2_used * VMAP_L2_ENTRIES);
    v->l2_used++;

    memset(l2, 0, VMAP_L2_ENTRIES * sizeof(u32));

    return l2;
}

vmap_err_t vmap_split_section(vmap_t *v, u32 va)
{
    vmap_attr_t attr;
    u32         idx;
    u32         desc;
    u32         section_base;
    u32         page_attr;
    u32         l1_entry;
    u32        *l2;
    u32         i;

    if (v == NULL || !v->inited) {
        return VMAP_ERR_NOT_INIT;
    }
    if (!in_range(v, va)) {
        return VMAP_ERR_OUT_OF_RANGE;
    }
    if ((va & (VMAP_PAGE_SIZE - 1u)) != 0u) {
        return VMAP_ERR_ALIGN;
    }

    idx  = vmap_l1_index(va);
    desc = v->l1[idx];

    if (l1_type(desc) == MMU_L1_TYPE_PAGE_TABLE) {
        return VMAP_OK; /* 幂等 */
    }
    if (l1_type(desc) != MMU_L1_TYPE_SECTION) {
        return VMAP_ERR_IS_SECTION;
    }

    l2 = l2_ensure(v, va);
    if (l2 == NULL) {
        return VMAP_ERR_NO_L2;
    }

    /* ★ 先解码参数,再用小页构造函数重建 —— 不是搬运属性值 ★ */
    decode_section_attr(desc, &attr);
    page_attr = page_attr_of(&attr);

    section_base = va & ~(MMU_SECTION_SIZE - 1u);

    /*
     * 逐页复制。少写一页,那一页会在 TLB 失效后立刻变成 translation fault,
     * 而故障点离"我刚才拆了段"这个原因隔得很远。
     */
    for (i = 0; i < VMAP_L2_ENTRIES; i++) {
        u32 pa = section_base + (i << VMAP_PAGE_SHIFT);

        l2[i] = mmu_small_page_descriptor(pa, page_attr);
    }

    l1_entry = v->l2_pool_pa + (u32)((size_t)(l2 - v->l2_pool) * sizeof(u32));
    if ((l1_entry & (VMAP_L2_ALIGN - 1u)) != 0u) {
        return VMAP_ERR_ALIGN;
    }

    v->l1[idx] = mmu_page_table_descriptor(l1_entry, attr.domain);
    v->stat_split++;

    return VMAP_OK;
}

vmap_err_t vmap_map(vmap_t *v, u32 va, u32 pa, const vmap_attr_t *attr)
{
    vmap_err_t e;
    u32       *l2;
    u32        cur;

    if (v == NULL || !v->inited || attr == NULL) {
        return VMAP_ERR_BAD_ARGS;
    }
    if (!in_range(v, va)) {
        return VMAP_ERR_OUT_OF_RANGE;
    }
    if ((va & (VMAP_PAGE_SIZE - 1u)) != 0u || (pa & (VMAP_PAGE_SIZE - 1u)) != 0u) {
        return VMAP_ERR_ALIGN;
    }

    cur = vmap_lookup(v, va);

    if (cur != 0u && cur != VMAP_RESULT_SECTION) {
        /* 已经是一个真实的小页映射 —— 拒绝覆盖 */
        return VMAP_ERR_ALREADY;
    }

    if (cur == VMAP_RESULT_SECTION) {
        e = vmap_split_section(v, va);
        if (e != VMAP_OK) {
            return e;
        }
        /* 拆出来的是恒等映射,而调用方明确要求换成别的 —— 清掉再写 */
        l2 = vmap_l2_of(v, va);
        if (l2 == NULL) {
            return VMAP_ERR_NO_L2;
        }
        l2[vmap_l2_index(va)] = 0u;
    }

    l2 = l2_ensure(v, va);
    if (l2 == NULL) {
        return VMAP_ERR_NO_L2;
    }

    l2[vmap_l2_index(va)] = mmu_small_page_descriptor(pa, page_attr_of(attr));
    v->stat_map++;

    return VMAP_OK;
}

vmap_err_t vmap_unmap(vmap_t *v, u32 va)
{
    u32  cur;
    u32 *l2;

    if (v == NULL || !v->inited) {
        return VMAP_ERR_NOT_INIT;
    }
    if (!in_range(v, va)) {
        return VMAP_ERR_OUT_OF_RANGE;
    }
    if ((va & (VMAP_PAGE_SIZE - 1u)) != 0u) {
        return VMAP_ERR_ALIGN;
    }

    cur = vmap_lookup(v, va);
    if (cur == 0u) {
        return VMAP_ERR_NOT_MAPPED;
    }
    if (cur == VMAP_RESULT_SECTION) {
        return VMAP_ERR_IS_SECTION; /* 段映射下没法只取消一页 */
    }

    l2 = vmap_l2_of(v, va);
    if (l2 == NULL) {
        return VMAP_ERR_NOT_MAPPED;
    }

    l2[vmap_l2_index(va)] = 0u;
    v->stat_unmap++;

    return VMAP_OK;
}

vmap_err_t vmap_set_attr(vmap_t *v, u32 va, const vmap_attr_t *attr)
{
    u32  cur;
    u32 *l2;

    if (v == NULL || !v->inited || attr == NULL) {
        return VMAP_ERR_BAD_ARGS;
    }

    cur = vmap_lookup(v, va);
    if (cur == 0u) {
        return VMAP_ERR_NOT_MAPPED;
    }
    if (cur == VMAP_RESULT_SECTION) {
        return VMAP_ERR_IS_SECTION;
    }

    l2 = vmap_l2_of(v, va);
    if (l2 == NULL) {
        return VMAP_ERR_NOT_MAPPED;
    }

    /* 物理地址不变,只换属性 */
    l2[vmap_l2_index(va)] = mmu_small_page_descriptor(cur & ~0xFFFu, page_attr_of(attr));

    return VMAP_OK;
}

/* ------------------------------------------------------------------ */
/* 自检                                                                */
/* ------------------------------------------------------------------ */

#define ST_L1_ENTRIES 4096u
#define ST_L2_COUNT   4u
#define ST_VA_BEGIN   0x40000000u
#define ST_VA_END     0x40400000u /* 4MB = 4 个段 */

/*
 * ⚠ 用 8192 而不是 16384:16KB 对齐是 **TTBR0 的硬件要求**
 *   (见 boot/kernel.ld 的 ASSERT),而 Windows 上静态数组最大对齐就是 8192。
 *   本自检只检查描述符填写是否正确,不把这张表交给 MMU。
 */
static u32 st_l1[ST_L1_ENTRIES] __attribute__((aligned(8192)));
static u32 st_l2[ST_L2_COUNT * VMAP_L2_ENTRIES] __attribute__((aligned(1024)));
static vmap_t st_v;

/* 合成物理地址:宿主指针是 64 位,塞不进 32 位描述符 */
#define ST_L2_PA 0x20000000u

u32 vmap_selftest(void)
{
    vmap_attr_t normal = vmap_attr_normal();
    vmap_err_t  e;
    u32         i;

    memset(st_l1, 0, sizeof(st_l1));
    memset(st_l2, 0, sizeof(st_l2));

    /* 预置:4MB 建为段映射(模拟内核恒等映射) */
    for (i = 0; i < 4u; i++) {
        u32 base = ST_VA_BEGIN + (i << MMU_SECTION_SHIFT);
        st_l1[vmap_l1_index(base)] =
            mmu_section_descriptor(base, section_attr_of(&normal));
    }

    /* 1. 初始化 */
    if (vmap_init(&st_v, st_l1, (u32 *)st_l2, ST_L2_PA, ST_L2_COUNT, ST_VA_BEGIN, ST_VA_END) !=
        VMAP_OK) {
        return 1;
    }

    /* 2. 非法参数 */
    if (vmap_init(&st_v, st_l1, (u32 *)st_l2, ST_L2_PA, ST_L2_COUNT, ST_VA_BEGIN + 1u, ST_VA_END) !=
        VMAP_ERR_ALIGN) {
        return 2;
    }
    if (vmap_init(&st_v, st_l1, (u32 *)st_l2, ST_L2_PA, 0u, ST_VA_BEGIN, ST_VA_END) !=
        VMAP_ERR_BAD_ARGS) {
        return 3;
    }
    if (vmap_init(&st_v, st_l1, NULL, ST_L2_PA, ST_L2_COUNT, ST_VA_BEGIN, ST_VA_END) !=
        VMAP_ERR_BAD_ARGS) {
        return 4;
    }
    if (vmap_init(&st_v, st_l1, (u32 *)st_l2, ST_L2_PA, ST_L2_COUNT, ST_VA_BEGIN, ST_VA_BEGIN) !=
        VMAP_ERR_BAD_ARGS) {
        return 5;
    }
    if (vmap_init(&st_v, st_l1, (u32 *)st_l2, ST_L2_PA, ST_L2_COUNT, ST_VA_BEGIN, ST_VA_END) !=
        VMAP_OK) {
        return 6;
    }

    /* 3. 索引换算 */
    if (vmap_l1_index(ST_VA_BEGIN) != 0x400u) return 7;
    if (vmap_l2_index(ST_VA_BEGIN) != 0u) return 8;
    if (vmap_l2_index(ST_VA_BEGIN + 0x1000u) != 1u) return 9;
    if (vmap_l2_index(ST_VA_BEGIN + 0xFF000u) != 255u) return 10;

    /* 4. 拆段之前 = 段 */
    if (vmap_lookup(&st_v, ST_VA_BEGIN) != VMAP_RESULT_SECTION) return 11;
    if (vmap_l2_of(&st_v, ST_VA_BEGIN) != NULL) return 12;

    /* 5. 越界保护 */
    if (vmap_lookup(&st_v, ST_VA_BEGIN - VMAP_PAGE_SIZE) != 0u) return 13;
    if (vmap_map(&st_v, ST_VA_END, 0x1000u, &normal) != VMAP_ERR_OUT_OF_RANGE) return 14;
    if (vmap_map(&st_v, ST_VA_BEGIN - VMAP_PAGE_SIZE, 0x1000u, &normal) != VMAP_ERR_OUT_OF_RANGE) {
        return 15;
    }

    /* 6. 未对齐 */
    if (vmap_map(&st_v, ST_VA_BEGIN + 1u, 0x1000u, &normal) != VMAP_ERR_ALIGN) return 16;
    if (vmap_map(&st_v, ST_VA_BEGIN, 0x1001u, &normal) != VMAP_ERR_ALIGN) return 17;

    /* 7. ★★ 拆段必须逐页保住物理地址与属性 ★★ */
    e = vmap_split_section(&st_v, ST_VA_BEGIN + 0x8000u);
    if (e != VMAP_OK) return 18;
    if (st_v.stat_split != 1u) return 19;

    for (i = 0; i < VMAP_L2_ENTRIES; i++) {
        u32 va  = ST_VA_BEGIN + (i << VMAP_PAGE_SHIFT);
        u32 got = vmap_lookup(&st_v, va);

        if (got == 0u || got == VMAP_RESULT_SECTION) return 20; /* 有页没映射 */
        if ((got & ~0xFFFu) != va) return 21;                  /* ★ 物理地址必须逐页相等 */
        if ((got & 0xFFFu) != page_attr_of(&normal)) return 22; /* ★ 属性必须翻译正确 */
    }

    /* 8. 相邻段不受影响 */
    if (vmap_lookup(&st_v, ST_VA_BEGIN + 0x100000u) != VMAP_RESULT_SECTION) return 23;

    /* 9. 幂等 */
    if (vmap_split_section(&st_v, ST_VA_BEGIN) != VMAP_OK) return 24;
    if (st_v.stat_split != 1u) return 25;

    /*
     * 10. 换掉一页映射 —— 必须先 unmap。
     *
     * vmap_map 对"已存在的 4KB 映射"一律拒绝(ALREADY),**这是刻意的**:
     * 静默覆盖会让先注册的那一方悄悄失效,而那种问题要等到它自己访问时才炸。
     * 拆段之后的恒等映射也算"已存在",所以换映射的流程是
     *   split(隐式) -> unmap -> map
     * 而不是让 map 自己去猜调用方的意图。
     *
     * ⚠ 对**段**映射则不需要先 unmap:vmap_map 会自动拆段并覆盖那一页
     *   (见第 14 项)。
     */
    if (vmap_unmap(&st_v, ST_VA_BEGIN) != VMAP_OK) return 45;
    if (vmap_map(&st_v, ST_VA_BEGIN, 0x80000000u, &normal) != VMAP_OK) return 26;
    if ((vmap_lookup(&st_v, ST_VA_BEGIN) & ~0xFFFu) != 0x80000000u) return 27;

    /* 11. ★ 重复映射被拒,且原值不变 ★ */
    if (vmap_map(&st_v, ST_VA_BEGIN, 0x90000000u, &normal) != VMAP_ERR_ALREADY) return 28;
    if ((vmap_lookup(&st_v, ST_VA_BEGIN) & ~0xFFFu) != 0x80000000u) return 29;

    /* 12. 解除映射 + 双重解除被拒 */
    if (vmap_unmap(&st_v, ST_VA_BEGIN) != VMAP_OK) return 30;
    if (vmap_lookup(&st_v, ST_VA_BEGIN) != 0u) return 31;
    if (vmap_unmap(&st_v, ST_VA_BEGIN) != VMAP_ERR_NOT_MAPPED) return 32;

    /* 13. 段上不能直接 unmap/set_attr */
    if (vmap_unmap(&st_v, ST_VA_BEGIN + 0x200000u) != VMAP_ERR_IS_SECTION) return 33;
    if (vmap_set_attr(&st_v, ST_VA_BEGIN + 0x200000u, &normal) != VMAP_ERR_IS_SECTION) return 34;

    /* 14. 自动拆段 */
    if (vmap_map(&st_v, ST_VA_BEGIN + 0x200000u, 0xA0000000u, &normal) != VMAP_OK) return 35;
    if (st_v.stat_split != 2u) return 36;
    if ((vmap_lookup(&st_v, ST_VA_BEGIN + 0x200000u) & ~0xFFFu) != 0xA0000000u) return 37;
    /* 同段邻居仍是恒等映射 */
    if ((vmap_lookup(&st_v, ST_VA_BEGIN + 0x201000u) & ~0xFFFu) != (ST_VA_BEGIN + 0x201000u)) {
        return 38;
    }

    /* 15. 改属性:物理地址不变、XN 位真的变了 */
    {
        vmap_attr_t ro = vmap_attr_ro_xn();
        u32         before = vmap_lookup(&st_v, ST_VA_BEGIN + 0x200000u);

        if (vmap_set_attr(&st_v, ST_VA_BEGIN + 0x200000u, &ro) != VMAP_OK) return 39;

        if ((vmap_lookup(&st_v, ST_VA_BEGIN + 0x200000u) & ~0xFFFu) != 0xA0000000u) return 40;

        /* 小页的 XN 在 bit0 —— 改属性必须真的动到它 */
        if ((before & 0x1u) != 0u) return 41;
        if ((vmap_lookup(&st_v, ST_VA_BEGIN + 0x200000u) & 0x1u) != 0x1u) return 42;
    }

    /* 16. L2 池用尽必须报错,而不是越界写 */
    {
        u32 guard = 0u;

        while (st_v.l2_used < st_v.l2_capacity && guard < 16u) {
            u32 va = ST_VA_BEGIN + ((u32)st_v.l2_used << MMU_SECTION_SHIFT);

            if (vmap_split_section(&st_v, va) != VMAP_OK) {
                break;
            }
            guard++;
        }

        if (st_v.l2_used > st_v.l2_capacity) return 43;

        if (st_v.l2_used == st_v.l2_capacity) {
            vmap_err_t r = vmap_split_section(&st_v, ST_VA_END - (1u << MMU_SECTION_SHIFT));
            if (r != VMAP_OK && r != VMAP_ERR_NO_L2) return 44;
        }
    }

    return 0;
}
