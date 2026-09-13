/*
 * 物理页分配器 —— 纯逻辑实现(M4-2)
 *
 * 不含 MMIO / CP15 / 内联汇编,bitmap 由调用方提供,
 * 所以宿主编译器能直接编译它并跑穷尽的错误路径测试。
 */

#include <arch/palloc.h>

/* ------------------------------------------------------------------ */
/* bitmap 位操作                                                       */
/* ------------------------------------------------------------------ */

static u32 state_shift(u32 index)
{
    /* 每页占 2 bit,所以页号 i 落在字 i/16 的 (i%16)*2 位上 */
    return (index & 15u) * PALLOC_BITS_PER_PAGE;
}

static u32 state_mask(void)
{
    return (1u << PALLOC_BITS_PER_PAGE) - 1u;
}

static u32 bitmap_get(const palloc_t *p, u32 index)
{
    u32 word  = p->bitmap[index / 16u];
    u32 shift = state_shift(index);

    return (word >> shift) & state_mask();
}

static void bitmap_set(palloc_t *p, u32 index, u32 state)
{
    u32 word  = index / 16u;
    u32 shift = state_shift(index);
    u32 mask  = state_mask() << shift;

    p->bitmap[word] = (p->bitmap[word] & ~mask) | ((state & state_mask()) << shift);
}

/* 页号 -> 物理地址 */
static uintptr_t page_addr(const palloc_t *p, u32 index)
{
    return p->base + ((uintptr_t)index << PALLOC_PAGE_SHIFT);
}

/*
 * 把一个物理地址换算成页号,顺便做完全部合法性检查。
 *
 * 集中在一处是刻意的:检查散落在各个入口时,迟早会漏掉一个。
 */
static palloc_err_t addr_to_index(const palloc_t *p, uintptr_t addr, u32 *out_index)
{
    uintptr_t offset;

    if (p == NULL || !p->inited) {
        return PALLOC_ERR_NOT_INIT;
    }

    if ((addr & (PALLOC_PAGE_SIZE - 1u)) != 0u) {
        return PALLOC_ERR_ALIGN;
    }

    if (addr < p->base) {
        return PALLOC_ERR_RANGE;
    }

    offset = addr - p->base;
    if (offset >= ((uintptr_t)p->page_count << PALLOC_PAGE_SHIFT)) {
        return PALLOC_ERR_RANGE;
    }

    *out_index = (u32)(offset >> PALLOC_PAGE_SHIFT);
    return PALLOC_OK;
}

/* ------------------------------------------------------------------ */
/* 初始化与保留                                                        */
/* ------------------------------------------------------------------ */

palloc_err_t palloc_init(palloc_t *p, uintptr_t base, size_t size, u32 *bitmap, u32 bitmap_words)
{
    u32 i;

    if (p == NULL || bitmap == NULL) {
        return PALLOC_ERR_BAD_ARGS;
    }

    if ((base & (PALLOC_PAGE_SIZE - 1u)) != 0u) {
        return PALLOC_ERR_ALIGN;
    }

    if (size == 0u || (size & (PALLOC_PAGE_SIZE - 1u)) != 0u) {
        /* size 也必须页对齐 —— 否则最后一页是"半页",语义不清 */
        return PALLOC_ERR_ALIGN;
    }

    if ((size >> PALLOC_PAGE_SHIFT) > (size_t)PALLOC_MAX_PAGES) {
        return PALLOC_ERR_TOO_MANY;
    }

    if (bitmap_words < PALLOC_BITMAP_WORDS) {
        return PALLOC_ERR_BAD_ARGS;
    }

    p->base           = base;
    p->page_count     = (u32)(size >> PALLOC_PAGE_SHIFT);
    p->free_pages     = p->page_count;
    p->used_pages     = 0u;
    p->reserved_pages = 0u;
    p->peak_used      = 0u;
    p->bitmap         = bitmap;
    p->bitmap_words   = bitmap_words;

    /* 先整片清零 = 全部 FREE,再按需标保留 */
    for (i = 0; i < bitmap_words; i++) {
        bitmap[i] = 0u;
    }

    p->inited = true;
    return PALLOC_OK;
}

palloc_err_t palloc_reserve(palloc_t *p, uintptr_t addr, size_t size)
{
    uintptr_t start;
    uintptr_t end;
    u32       first;
    u32       last;
    u32       i;

    if (p == NULL || !p->inited) {
        return PALLOC_ERR_NOT_INIT;
    }

    if (size == 0u) {
        return PALLOC_ERR_BAD_ARGS;
    }

    /*
     * 向外扩到整页。宁可多保留一点,也不能因为"少了半个页"
     * 把内核数据当成空闲页发出去。
     */
    start = addr & ~(uintptr_t)(PALLOC_PAGE_SIZE - 1u);
    end   = (addr + size + PALLOC_PAGE_SIZE - 1u) & ~(uintptr_t)(PALLOC_PAGE_SIZE - 1u);

    if (start < p->base) {
        start = p->base;
    }

    if (end <= start) {
        return PALLOC_ERR_BAD_ARGS;
    }

    /* 整段落在池外就当作无事发生 —— 调用方常常会把整个地址空间的范围都报一遍 */
    if (start >= p->base + ((uintptr_t)p->page_count << PALLOC_PAGE_SHIFT)) {
        return PALLOC_OK;
    }

    if (end > p->base + ((uintptr_t)p->page_count << PALLOC_PAGE_SHIFT)) {
        end = p->base + ((uintptr_t)p->page_count << PALLOC_PAGE_SHIFT);
    }

    first = (u32)((start - p->base) >> PALLOC_PAGE_SHIFT);
    last  = (u32)((end - p->base) >> PALLOC_PAGE_SHIFT);

    for (i = first; i < last; i++) {
        u32 state = bitmap_get(p, i);

        if (state == PALLOC_STATE_RSVD) {
            continue; /* 已经保留过,重复保留是无害的 */
        }

        if (state == PALLOC_STATE_USED) {
            /*
             * 已经分配出去的页被要求保留 —— 这是真错误:
             * 说明保留清单写得比实际占用晚,或者两处配置互相矛盾。
             * 静默改成 RSVD 会让那个已分配的页永远无法释放(泄漏),
             * 所以直接报错。
             */
            return PALLOC_ERR_ALREADY_USED;
        }

        bitmap_set(p, i, PALLOC_STATE_RSVD);
        p->free_pages--;
        p->reserved_pages++;
    }

    return PALLOC_OK;
}

/* ------------------------------------------------------------------ */
/* 分配与释放                                                          */
/* ------------------------------------------------------------------ */

palloc_err_t palloc_alloc(palloc_t *p, uintptr_t *out)
{
    return palloc_alloc_pages(p, 1u, out);
}

palloc_err_t palloc_alloc_pages(palloc_t *p, u32 n, uintptr_t *out)
{
    u32 run   = 0;
    u32 i;
    u32 start = 0;

    if (p == NULL || !p->inited) {
        return PALLOC_ERR_NOT_INIT;
    }

    if (out == NULL || n == 0u) {
        return PALLOC_ERR_BAD_ARGS;
    }

    if (n > p->free_pages) {
        return PALLOC_ERR_OOM;
    }

    /*
     * 首次适应(first fit)。不做"切分/合并"是因为位图分配器天然没有
     * 外部碎片的概念 —— 一页就是最小单位,不存在半个页。
     * 代价是**连续多页**会随使用而变难找,这是 DMA 缓冲将来可能遇到的
     * 问题,那时再考虑伙伴系统。现在不预先优化。
     */
    for (i = 0; i < p->page_count; i++) {
        if (bitmap_get(p, i) == PALLOC_STATE_FREE) {
            if (run == 0u) {
                start = i;
            }
            run++;

            if (run == n) {
                u32 k;
                for (k = start; k < start + n; k++) {
                    bitmap_set(p, k, PALLOC_STATE_USED);
                }

                p->free_pages -= n;
                p->used_pages += n;
                if (p->used_pages > p->peak_used) {
                    p->peak_used = p->used_pages;
                }

                *out = page_addr(p, start);
                return PALLOC_OK;
            }
        } else {
            run = 0u;
        }
    }

    return PALLOC_ERR_OOM;
}

palloc_err_t palloc_free(palloc_t *p, uintptr_t addr)
{
    u32           index;
    u32           state;
    palloc_err_t  err;

    err = addr_to_index(p, addr, &index);
    if (err != PALLOC_OK) {
        return err;
    }

    state = bitmap_get(p, index);

    /*
     * 三种"看起来像成功、实际是 bug"的情形,必须各自报出来。
     * 这正是 2 bit/页 换来的东西 —— 1 bit 时它们全都无法区分。
     */
    if (state == PALLOC_STATE_FREE) {
        return PALLOC_ERR_DOUBLE_FREE;
    }

    if (state == PALLOC_STATE_RSVD) {
        return PALLOC_ERR_RESERVED;
    }

    bitmap_set(p, index, PALLOC_STATE_FREE);
    p->used_pages--;
    p->free_pages++;

    return PALLOC_OK;
}

u32 palloc_state(palloc_t *p, uintptr_t addr)
{
    u32 index;

    if (addr_to_index(p, addr, &index) != PALLOC_OK) {
        return PALLOC_STATE_BAD;
    }

    return bitmap_get(p, index);
}

/* ------------------------------------------------------------------ */
/* 自检                                                                */
/* ------------------------------------------------------------------ */

/*
 * 合成实例:池 = 0x1000_0000 起 64 页,bitmap 用一个静态数组。
 *
 * 用合成实例而不是真实池:错误路径需要精确控制状态,在真实池上做会受
 * 当前用量影响;而且它会真的吃掉页,把一个只读的检查变成有副作用的操作。
 */
#define ST_PAGES 64u
#define ST_BASE  0x10000000u

static u32      st_bitmap[PALLOC_BITMAP_WORDS];
static palloc_t st_pool;

u32 palloc_selftest(void)
{
    palloc_err_t e;
    uintptr_t    a1;
    uintptr_t    a2;
    uintptr_t    run;
    u32          i;

    /* 1. 初始化 */
    e = palloc_init(&st_pool, ST_BASE, ST_PAGES * PALLOC_PAGE_SIZE, st_bitmap, PALLOC_BITMAP_WORDS);
    if (e != PALLOC_OK) return 1;
    if (st_pool.page_count != ST_PAGES) return 2;
    if (st_pool.free_pages != ST_PAGES || st_pool.used_pages != 0u) return 3;

    /* 2. 未对齐的 base 必须被拒 */
    if (palloc_init(&st_pool, ST_BASE + 1u, ST_PAGES * PALLOC_PAGE_SIZE, st_bitmap,
                    PALLOC_BITMAP_WORDS) != PALLOC_ERR_ALIGN) return 4;

    /* 3. 非页对齐的 size 必须被拒 */
    if (palloc_init(&st_pool, ST_BASE, ST_PAGES * PALLOC_PAGE_SIZE - 1u, st_bitmap,
                    PALLOC_BITMAP_WORDS) != PALLOC_ERR_ALIGN) return 5;

    /* 4. bitmap 太小必须被拒 */
    if (palloc_init(&st_pool, ST_BASE, ST_PAGES * PALLOC_PAGE_SIZE, st_bitmap,
                    PALLOC_BITMAP_WORDS - 1u) != PALLOC_ERR_BAD_ARGS) return 6;

    /* 重新正常初始化 */
    if (palloc_init(&st_pool, ST_BASE, ST_PAGES * PALLOC_PAGE_SIZE, st_bitmap,
                    PALLOC_BITMAP_WORDS) != PALLOC_OK) return 7;

    /* 5. 页数超过上限必须被拒(用超大的 size 触发) */
    if (palloc_init(&st_pool, ST_BASE, ((size_t)PALLOC_MAX_PAGES + 1u) << PALLOC_PAGE_SHIFT, st_bitmap,
                    PALLOC_BITMAP_WORDS) != PALLOC_ERR_TOO_MANY) return 8;

    if (palloc_init(&st_pool, ST_BASE, ST_PAGES * PALLOC_PAGE_SIZE, st_bitmap,
                    PALLOC_BITMAP_WORDS) != PALLOC_OK) return 9;

    /*
     * 6. 保留一段**故意跨页**且两端都不对齐的区间 —— 必须向外扩到整页。
     *
     * 区间 = [BASE+0x1010, BASE+0x2020):
     *   起点的页内偏移 0x10,终点的页内偏移 0x20 —— 都不对齐,
     *   而且它横跨第 1、2 两页。所以正确结果是**两页**都被保留。
     *
     * 这个形状是刻意选的:只测"单页内的不对齐区间"无法区分
     * "向外扩"与"只扩起点",而那两种写法在跨页时才分道扬镳。
     */
    if (palloc_reserve(&st_pool, ST_BASE + PALLOC_PAGE_SIZE + 16u, PALLOC_PAGE_SIZE + 16u) != PALLOC_OK) {
        return 10;
    }
    if (palloc_state(&st_pool, ST_BASE + PALLOC_PAGE_SIZE) != PALLOC_STATE_RSVD) return 11;
    if (palloc_state(&st_pool, ST_BASE + 2u * PALLOC_PAGE_SIZE) != PALLOC_STATE_RSVD) return 12;
    if (palloc_state(&st_pool, ST_BASE + 3u * PALLOC_PAGE_SIZE) != PALLOC_STATE_FREE) return 13;

    /* 7. 保留页不可释放 */
    if (palloc_free(&st_pool, ST_BASE + PALLOC_PAGE_SIZE) != PALLOC_ERR_RESERVED) return 14;

    /* 8. 分配一页 */
    if (palloc_alloc(&st_pool, &a1) != PALLOC_OK) return 15;
    if (a1 != ST_BASE) return 16; /* 首适应:应当拿到第 0 页 */
    if (palloc_state(&st_pool, a1) != PALLOC_STATE_USED) return 17;
    if (st_pool.used_pages != 1u || st_pool.free_pages != ST_PAGES - 3u) return 18;

    /* 9. 分配不应碰到保留页 */
    if (palloc_alloc(&st_pool, &a2) != PALLOC_OK) return 19;
    if (a2 != ST_BASE + 3u * PALLOC_PAGE_SIZE) return 20; /* 跳过保留的两页 */

    /* 10. 释放并重新分配 —— 应当拿回同一页(首次适应) */
    if (palloc_free(&st_pool, a1) != PALLOC_OK) return 21;
    if (palloc_state(&st_pool, a1) != PALLOC_STATE_FREE) return 22;
    if (palloc_alloc(&st_pool, &run) != PALLOC_OK || run != a1) return 23;

    /* 11. ★ 双重释放必须被拒 ★ —— 这是 2 bit/页 的主要理由 */
    if (palloc_free(&st_pool, a1) != PALLOC_OK) return 24;
    if (palloc_free(&st_pool, a1) != PALLOC_ERR_DOUBLE_FREE) return 25;

    /* 12. 未对齐地址必须被拒 */
    if (palloc_free(&st_pool, a1 + 1u) != PALLOC_ERR_ALIGN) return 26;

    /* 13. 越界地址必须被拒(两头都要测) */
    if (palloc_free(&st_pool, ST_BASE - PALLOC_PAGE_SIZE) != PALLOC_ERR_RANGE) return 27;
    if (palloc_free(&st_pool, ST_BASE + ST_PAGES * PALLOC_PAGE_SIZE) != PALLOC_ERR_RANGE) return 28;

    /* 14. 池内的地址必须能查到状态;池外必须是 BAD */
    if (palloc_state(&st_pool, ST_BASE + ST_PAGES * PALLOC_PAGE_SIZE) != PALLOC_STATE_BAD) return 29;

    /* 15. 连续多页分配 */
    if (palloc_alloc_pages(&st_pool, 8u, &run) != PALLOC_OK) return 30;
    for (i = 0; i < 8u; i++) {
        if (palloc_state(&st_pool, run + (i << PALLOC_PAGE_SHIFT)) != PALLOC_STATE_USED) return 31;
    }

    /* 16. 请求超过空闲数的连续页必须 OOM,而不是"给一部分" */
    if (palloc_alloc_pages(&st_pool, ST_PAGES, &run) != PALLOC_ERR_OOM) return 32;

    /* 17. 0 页请求必须被拒(而不是返回一个"合法"地址) */
    if (palloc_alloc_pages(&st_pool, 0u, &run) != PALLOC_ERR_BAD_ARGS) return 33;

    /* 18. 空指针必须被拒 */
    if (palloc_alloc(&st_pool, NULL) != PALLOC_ERR_BAD_ARGS) return 34;
    if (palloc_free(&st_pool, 0u) == PALLOC_OK) return 35; /* 0 不在池内 */

    /* 19. 释放全部已分配页之后,空闲数应当回到"总数 - 保留数" */
    for (i = 0; i < ST_PAGES; i++) {
        uintptr_t a = ST_BASE + ((uintptr_t)i << PALLOC_PAGE_SHIFT);
        u32       s = palloc_state(&st_pool, a);

        if (s == PALLOC_STATE_USED) {
            if (palloc_free(&st_pool, a) != PALLOC_OK) return 36;
        }
    }
    if (st_pool.used_pages != 0u) return 37;
    if (st_pool.free_pages != ST_PAGES - 2u) return 38; /* 保留的那两页仍不可用 */
    if (st_pool.reserved_pages != 2u) return 39;

    /* 20. 保留页仍在 —— 释放风暴之后它不该被误释放 */
    if (palloc_state(&st_pool, ST_BASE + PALLOC_PAGE_SIZE) != PALLOC_STATE_RSVD) return 40;

    /* 21. peak_used 应当记录到过历史上的最高值 */
    if (st_pool.peak_used == 0u) return 41;

    return 0;
}
