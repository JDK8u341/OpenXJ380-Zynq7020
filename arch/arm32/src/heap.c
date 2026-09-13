/*
 * 内核堆 —— 纯逻辑实现(M4-3)
 *
 * 不含 MMIO / CP15;底层内存通过 grow 回调从外部要来,
 * 所以宿主编译器能直接编译它并跑穷尽的错误路径测试。
 *
 * 结构:一条**按地址顺序**的双向链表贯穿整个堆区。
 * 空闲块也在链表里(靠 state 区分),于是合并就是"看邻居"这么简单,
 * 不需要额外维护一张空闲表 —— 那张表迟早会与链表不同步。
 */

#include <arch/heap.h>
#include <krlibc.h>

/*
 * 编译期钉住:对齐后的头必须真的装得下结构体,否则负载会压到结构体尾巴上。
 * 这一条在 host(40)与 target(24)上都会检查,而两种宽度都曾各自出过错。
 */
_Static_assert((HEAP_HEADER_RAW + (HEAP_ALIGN - 1u)) >= (u32)sizeof(heap_block_t),
               "HEAP_HEADER_SIZE 装不下 heap_block_t");

#define ALIGN_UP_8(x)   (((x) + 7u) & ~(size_t)7u)
#define ALIGN_DOWN_8(x) ((x) & ~(size_t)7u)

/* 一个块至少要能放下头 + 8 字节负载,否则切出来的碎片无法再被使用 */
#define HEAP_MIN_BLOCK (HEAP_HEADER_SIZE + HEAP_ALIGN)

static inline void *payload_of(heap_block_t *b)
{
    return (void *)((uintptr_t)b + HEAP_HEADER_SIZE);
}

static inline heap_block_t *block_of(void *payload)
{
    return (heap_block_t *)((uintptr_t)payload - HEAP_HEADER_SIZE);
}

static inline u32 block_state(const heap_block_t *b)
{
    return b->state;
}

static void init_header(heap_block_t *b, u32 state, size_t size)
{
    b->magic = HEAP_MAGIC;
    b->state = state;
    b->size  = size;
    b->next  = NULL;
    b->prev  = NULL;
    b->pad   = 0u;
}

/* ------------------------------------------------------------------ */
/* 初始化与增长                                                        */
/* ------------------------------------------------------------------ */

heap_err_t heap_init(heap_t *h, uintptr_t base, size_t size)
{
    heap_block_t *first;

    if (h == NULL) {
        return HEAP_ERR_BAD_ARGS;
    }

    if ((base & (HEAP_ALIGN - 1u)) != 0u) {
        return HEAP_ERR_BAD_ARGS;
    }

    size = ALIGN_DOWN_8(size);
    if (size < HEAP_MIN_BLOCK) {
        return HEAP_ERR_BAD_ARGS;
    }

    h->base = base;
    h->size = size;

    first = (heap_block_t *)base;
    init_header(first, HEAP_STATE_FREE, size - HEAP_HEADER_SIZE);

    h->head = first;

    h->grow        = NULL;
    h->grow_ctx    = NULL;
    h->total_bytes = size;
    h->used_bytes  = 0u;
    h->free_bytes  = size - HEAP_HEADER_SIZE;
    h->largest_free = h->free_bytes;
    h->alloc_count = 0u;
    h->free_count  = 0u;
    h->fail_count  = 0u;
    h->grow_count  = 0u;
    h->inited      = true;

    return HEAP_OK;
}

void heap_set_grow(heap_t *h, heap_grow_fn fn, void *ctx)
{
    if (h == NULL) {
        return;
    }

    h->grow     = fn;
    h->grow_ctx = ctx;
}

/* 把一块新内存接到链表尾部(必须紧邻,因为增长是连续的) */
static heap_err_t heap_extend(heap_t *h, uintptr_t base, size_t size)
{
    heap_block_t *tail;
    heap_block_t *fresh;

    size = ALIGN_DOWN_8(size);
    if (size < HEAP_MIN_BLOCK) {
        return HEAP_ERR_BAD_ARGS;
    }

    if (base != h->base + h->size) {
        /* 增长区必须紧邻 —— 否则没法并进同一条链表 */
        return HEAP_ERR_BAD_ARGS;
    }

    tail = h->head;
    while (tail->next != NULL) {
        tail = tail->next;
    }

    fresh = (heap_block_t *)base;
    init_header(fresh, HEAP_STATE_FREE, size - HEAP_HEADER_SIZE);
    fresh->prev = tail;
    tail->next  = fresh;

    h->size += size;
    h->total_bytes += size;
    h->free_bytes += size - HEAP_HEADER_SIZE;
    h->grow_count++;

    /* 新块可能与尾部空闲块相邻,合并它 */
    if (block_state(tail) == HEAP_STATE_FREE) {
        tail->size += HEAP_HEADER_SIZE + fresh->size;
        tail->next  = NULL;
        h->free_bytes += HEAP_HEADER_SIZE;
    }

    h->largest_free = heap_largest_free(h);

    return HEAP_OK;
}

static bool heap_try_grow(heap_t *h, size_t need)
{
    uintptr_t got;

    if (h->grow == NULL) {
        return false;
    }

    /* 按页向上取整由回调负责;这里只要够用即可 */
    got = h->grow(h->grow_ctx, need + HEAP_HEADER_SIZE);
    if (got == 0u) {
        return false;
    }

    if (heap_extend(h, got, need + HEAP_HEADER_SIZE) != HEAP_OK) {
        return false;
    }

    return true;
}

/* ------------------------------------------------------------------ */
/* 分配                                                                */
/* ------------------------------------------------------------------ */

void *heap_alloc(heap_t *h, size_t size)
{
    heap_block_t *b;

    if (h == NULL || !h->inited) {
        return NULL;
    }

    size = ALIGN_UP_8(size);
    if (size == 0u) {
        size = HEAP_ALIGN; /* 分配 0 字节也返回一个可释放的块,不返回 NULL */
    }

    for (;;) {
        for (b = h->head; b != NULL; b = b->next) {
            heap_block_t *rest;
            size_t        leftover;

            if (block_state(b) != HEAP_STATE_FREE || b->size < size) {
                continue;
            }

            leftover = b->size - size;

            /*
             * 剩余空间够放下一个完整块才切分。
             * 不够就整块给出 —— 否则切出来的是一个永远无法再被使用的
             * 碎片,既浪费又会破坏"相邻空闲必须合并"的自洽性。
             */
            if (leftover >= HEAP_MIN_BLOCK) {
                rest = (heap_block_t *)((uintptr_t)b + HEAP_HEADER_SIZE + size);
                init_header(rest, HEAP_STATE_FREE, leftover - HEAP_HEADER_SIZE);

                rest->next = b->next;
                rest->prev = b;
                if (b->next != NULL) {
                    b->next->prev = rest;
                }
                b->next = rest;

                b->size = size;
                h->free_bytes -= HEAP_HEADER_SIZE;
            }

            b->state = HEAP_STATE_USED;
            h->used_bytes += b->size;
            h->free_bytes -= b->size;
            h->alloc_count++;

            /*
             * largest_free 是**派生量**,必须在这里重算。
             *
             * 第一版忘了这一步 —— 它一直是 init 时的初值,于是 heap_check()
             * 的"统计与实际一致"那条立刻报错。这是自检抓出来的 bug,而且它
             * 自己不会以任何形式表现出来(没有任何调用方读这个字段去做出决定),
             * 属于典型的"静默失真"。
             */
            h->largest_free = heap_largest_free(h);

            return payload_of(b);
        }

        /* 没找到 —— 试着增长一次,再重试一轮 */
        if (!heap_try_grow(h, size)) {
            h->fail_count++;
            return NULL;
        }
    }
}

void *heap_calloc(heap_t *h, size_t count, size_t size)
{
    size_t total;
    void  *p;

    if (h == NULL || count == 0u || size == 0u) {
        /* 语义上有歧义时选择返回可释放的块,与 heap_alloc(0) 保持一致 */
        if (h == NULL) {
            return NULL;
        }
        total = HEAP_ALIGN;
    } else {
        total = count * size;
        if (total / size != count) {
            return NULL; /* 乘法溢出 */
        }
    }

    p = heap_alloc(h, total);
    if (p != NULL) {
        memset(p, 0, total);
    }

    return p;
}

void *heap_realloc(heap_t *h, void *ptr, size_t size)
{
    heap_block_t *b;
    void         *fresh;
    size_t        copy;

    if (h == NULL || !h->inited) {
        return NULL;
    }

    if (ptr == NULL) {
        return heap_alloc(h, size);
    }

    b = block_of(ptr);
    if (b->magic != HEAP_MAGIC || b->state != HEAP_STATE_USED) {
        return NULL; /* 头被写坏或指针非法 —— 不猜,直接失败 */
    }

    size = ALIGN_UP_8(size);

    /* 已经够大:原块返回,不做就地收缩(收缩会制造碎片,收益极小) */
    if (b->size >= size) {
        return ptr;
    }

    fresh = heap_alloc(h, size);
    if (fresh == NULL) {
        return NULL;
    }

    copy = b->size;
    if (copy > size) {
        copy = size;
    }
    memcpy(fresh, ptr, copy);

    (void)heap_free(h, ptr);
    return fresh;
}

/* ------------------------------------------------------------------ */
/* 释放与合并                                                          */
/* ------------------------------------------------------------------ */

heap_err_t heap_free(heap_t *h, void *ptr)
{
    heap_block_t *b;

    if (h == NULL || !h->inited) {
        return HEAP_ERR_NOT_INIT;
    }

    if (ptr == NULL) {
        return HEAP_ERR_BAD_POINTER;
    }

    /*
     * 边界检查分两层:
     *   1. 地址必须落在堆区内 —— 否则 block_of() 会读到一个完全无关的地址;
     *   2. 块头魔数必须对 —— 这一层抓的是"越界写把块头覆盖了"。
     *
     * ⚠ 第 2 层不能省。少了它,一块被写坏的头会导致链表操作按垃圾 size
     *   去改 next/prev,后果是整条链表被拆散 —— 而现场离真正的错误
     *   已经隔了很远。
     */
    if ((uintptr_t)ptr < h->base + HEAP_HEADER_SIZE ||
        (uintptr_t)ptr >= h->base + h->size) {
        return HEAP_ERR_BAD_POINTER;
    }

    b = block_of(ptr);

    if (b->magic != HEAP_MAGIC) {
        return HEAP_ERR_BAD_MAGIC;
    }

    if (b->state == HEAP_STATE_FREE) {
        return HEAP_ERR_DOUBLE_FREE;
    }

    if (b->state != HEAP_STATE_USED) {
        return HEAP_ERR_BAD_MAGIC;
    }

    b->state = HEAP_STATE_FREE;
    h->used_bytes -= b->size;
    h->free_bytes += b->size;
    h->free_count++;

    /*
     * 向后合并(与 next),再向前合并(与 prev)。
     * 顺序无所谓,但两步都要做 —— 只做一步的话,反复分配/释放同一位置
     * 会逐渐积累相邻空闲块,堆"看起来还能用"但最大可分配块越来越小。
     */
    if (b->next != NULL && block_state(b->next) == HEAP_STATE_FREE) {
        heap_block_t *victim = b->next;

        b->size += HEAP_HEADER_SIZE + victim->size;
        b->next = victim->next;
        if (victim->next != NULL) {
            victim->next->prev = b;
        }
        h->free_bytes += HEAP_HEADER_SIZE;
    }

    if (b->prev != NULL && block_state(b->prev) == HEAP_STATE_FREE) {
        heap_block_t *keep = b->prev;

        keep->size += HEAP_HEADER_SIZE + b->size;
        keep->next = b->next;
        if (b->next != NULL) {
            b->next->prev = keep;
        }
        h->free_bytes += HEAP_HEADER_SIZE;
    }

    h->largest_free = heap_largest_free(h);

    return HEAP_OK;
}

/* ------------------------------------------------------------------ */
/* 自洽性检查                                                          */
/* ------------------------------------------------------------------ */

heap_err_t heap_check(heap_t *h)
{
    heap_block_t *b;
    heap_block_t *prev = NULL;
    size_t        free_sum = 0u;
    size_t        used_sum = 0u;
    size_t        largest  = 0u;
    uintptr_t     expect;

    if (h == NULL || !h->inited) {
        return HEAP_ERR_NOT_INIT;
    }

    expect = h->base;

    for (b = h->head; b != NULL; b = b->next) {
        if (b->magic != HEAP_MAGIC) {
            return HEAP_ERR_BAD_MAGIC;
        }

        /* 地址必须严格按顺序、且紧邻 —— 中间有空洞说明链表与内存脱节 */
        if ((uintptr_t)b != expect) {
            return HEAP_ERR_CORRUPT;
        }

        if (b->prev != prev) {
            return HEAP_ERR_CORRUPT;
        }

        if (b->size == 0u || (b->size & (HEAP_ALIGN - 1u)) != 0u) {
            return HEAP_ERR_CORRUPT;
        }

        if (block_state(b) == HEAP_STATE_FREE) {
            /* 相邻双空闲 = 合并漏了 */
            if (prev != NULL && block_state(prev) == HEAP_STATE_FREE) {
                return HEAP_ERR_CORRUPT;
            }
            free_sum += b->size;
            if (b->size > largest) {
                largest = b->size;
            }
        } else if (block_state(b) == HEAP_STATE_USED) {
            used_sum += b->size;
        } else {
            return HEAP_ERR_CORRUPT;
        }

        expect = (uintptr_t)b + HEAP_HEADER_SIZE + b->size;
        prev   = b;
    }

    /* 链表必须恰好铺满整个堆区 */
    if (expect != h->base + h->size) {
        return HEAP_ERR_CORRUPT;
    }

    /* 统计值必须与实际一致 —— 不一致说明某处漏更新了 */
    if (free_sum != h->free_bytes || used_sum != h->used_bytes) {
        return HEAP_ERR_CORRUPT;
    }

    if (largest != h->largest_free) {
        return HEAP_ERR_CORRUPT;
    }

    return HEAP_OK;
}

size_t heap_largest_free(heap_t *h)
{
    heap_block_t *b;
    size_t        largest = 0u;

    if (h == NULL || !h->inited) {
        return 0u;
    }

    for (b = h->head; b != NULL; b = b->next) {
        if (block_state(b) == HEAP_STATE_FREE && b->size > largest) {
            largest = b->size;
        }
    }

    return largest;
}

/* ------------------------------------------------------------------ */
/* 自检                                                                */
/* ------------------------------------------------------------------ */

#define ST_HEAP_BYTES 4096u

/*
 * 初始堆区与增长区必须是**同一块内存里相邻的两段** —— 见 heap.h 的说明。
 * 所以这里用一个数组切两半,而不是两个独立数组(那样增长必然失败,
 * 第一版就是这么写的,自检第 45 项抓到了)。
 */
#define ST_TOTAL      8192u
#define ST_INIT_BYTES 4096u /* 主测试用的堆 */
#define ST_GROW_BYTES (ST_TOTAL - ST_GROW_INIT) /* 增长测试可用的尾部 */
#define ST_GROW_INIT  512u  /* 增长测试的初始堆:小到一定会触发增长 */

static u8   st_area[ST_TOTAL] __attribute__((aligned(8)));
static bool st_grow_used;

static uintptr_t st_grow(void *ctx, size_t need)
{
    (void)ctx;

    if (st_grow_used || need > ST_GROW_BYTES) {
        return 0u;
    }

    st_grow_used = true;
    return (uintptr_t)st_area + ST_GROW_INIT; /* 紧邻增长测试的初始堆区 */
}

u32 heap_selftest(void)
{
    heap_t  h;
    void   *a;
    void   *b;
    void   *c;

    st_grow_used = false;

    /* 1. 初始化 */
    memset(st_area, 0, sizeof(st_area));
    if (heap_init(&h, (uintptr_t)st_area, ST_INIT_BYTES) != HEAP_OK) return 1;
    if (heap_check(&h) != HEAP_OK) return 2;
    if (h.used_bytes != 0u) return 3;
    if (h.free_bytes != ST_INIT_BYTES - HEAP_HEADER_SIZE) return 4;

    /* 2. 未对齐的 base 必须被拒 */
    if (heap_init(&h, (uintptr_t)st_area + 1u, 1024u) != HEAP_ERR_BAD_ARGS) return 5;

    /* 3. 太小的堆必须被拒 */
    if (heap_init(&h, (uintptr_t)st_area, 8u) != HEAP_ERR_BAD_ARGS) return 6;

    if (heap_init(&h, (uintptr_t)st_area, ST_INIT_BYTES) != HEAP_OK) return 7;

    /* 4. 基本分配 */
    a = heap_alloc(&h, 100u);
    if (a == NULL) return 8;
    if (((uintptr_t)a & 7u) != 0u) return 9; /* 必须 8 字节对齐 */
    if (heap_check(&h) != HEAP_OK) return 10;
    if (h.used_bytes < 100u) return 11;

    b = heap_alloc(&h, 200u);
    if (b == NULL || b == a) return 12;
    if (heap_check(&h) != HEAP_OK) return 13;

    /* 5. 写入负载不能破坏块头 —— 写满再检查 */
    memset(a, 0xAA, 100u);
    memset(b, 0xBB, 200u);
    if (heap_check(&h) != HEAP_OK) return 14;

    /* 6. 双重释放必须被拒,且不改变状态 */
    if (heap_free(&h, a) != HEAP_OK) return 15;
    {
        size_t free_before = h.free_bytes;
        size_t used_before = h.used_bytes;

        if (heap_free(&h, a) != HEAP_ERR_DOUBLE_FREE) return 16;
        if (h.free_bytes != free_before || h.used_bytes != used_before) return 17;
    }
    if (heap_check(&h) != HEAP_OK) return 18;

    /* 7. ★ 相邻空闲必须被合并 ★ —— 释放 b 之后应当只剩一个大空闲块 */
    if (heap_free(&h, b) != HEAP_OK) return 19;
    if (heap_check(&h) != HEAP_OK) return 20;
    if (h.free_bytes != ST_INIT_BYTES - HEAP_HEADER_SIZE) return 21; /* 完全回收 */
    if (heap_largest_free(&h) != h.free_bytes) return 22;            /* 只有一块 */

    /* 8. 释放非法指针 */
    if (heap_free(&h, NULL) != HEAP_ERR_BAD_POINTER) return 23;
    if (heap_free(&h, (void *)((uintptr_t)st_area - 64u)) != HEAP_ERR_BAD_POINTER) return 24;
    if (heap_free(&h, (void *)((uintptr_t)st_area + ST_HEAP_BYTES + 64u)) != HEAP_ERR_BAD_POINTER) {
        return 25;
    }
    /* 指向块头而不是负载的指针 —— 必须被拒(否则会读到垃圾 size) */
    if (heap_free(&h, (void *)((uintptr_t)st_area)) == HEAP_OK) return 26;

    /* 9. ★ 块头被写坏必须被发现 ★ */
    a = heap_alloc(&h, 64u);
    if (a == NULL) return 27;
    {
        heap_block_t *hdr = block_of(a);
        u32           saved = hdr->magic;

        hdr->magic = 0xDEADBEEFu; /* 模拟越界写覆盖了块头 */
        if (heap_free(&h, a) != HEAP_ERR_BAD_MAGIC) return 28;
        hdr->magic = saved; /* 恢复,便于后续检查 */
    }
    if (heap_free(&h, a) != HEAP_OK) return 29;
    if (heap_check(&h) != HEAP_OK) return 30;

    /* 10. calloc 必须清零 */
    c = heap_calloc(&h, 16u, 8u);
    if (c == NULL) return 31;
    {
        u32 i;
        for (i = 0; i < 128u; i++) {
            if (((u8 *)c)[i] != 0u) return 32;
        }
    }
    if (heap_free(&h, c) != HEAP_OK) return 33;

    /* 11. realloc:扩容要保留内容,且旧块被回收 */
    a = heap_alloc(&h, 32u);
    if (a == NULL) return 34;
    memset(a, 0x5A, 32u);
    c = heap_realloc(&h, a, 256u);
    if (c == NULL) return 35;
    {
        u32 i;
        for (i = 0; i < 32u; i++) {
            if (((u8 *)c)[i] != 0x5A) return 36; /* 内容必须保留 */
        }
    }
    /* 缩小应当原地返回同一个指针(不做就地收缩) */
    if (heap_realloc(&h, c, 16u) != c) return 37;
    if (heap_free(&h, c) != HEAP_OK) return 38;

    /* 12. 分配 0 字节:必须返回可释放的块,而不是 NULL */
    a = heap_alloc(&h, 0u);
    if (a == NULL) return 39;
    if (heap_free(&h, a) != HEAP_OK) return 40;

    /* 13. 超出堆容量必须返回 NULL,且 fail_count 增加 */
    {
        u32 before = h.fail_count;
        if (heap_alloc(&h, ST_INIT_BYTES * 2u) != NULL) return 41;
        if (h.fail_count == before) return 42;
    }

    /* 14. 增长:小堆用完时会去要内存,并接在链表尾部 */
    memset(st_area, 0, sizeof(st_area));
    if (heap_init(&h, (uintptr_t)st_area, ST_GROW_INIT) != HEAP_OK) return 43;
    heap_set_grow(&h, st_grow, NULL);

    a = heap_alloc(&h, 200u);
    if (a == NULL) return 44;
    b = heap_alloc(&h, 1000u); /* 这一个必须触发增长 */
    if (b == NULL) return 45;
    if (h.grow_count == 0u) return 46;
    if (heap_check(&h) != HEAP_OK) return 47;
    if (heap_free(&h, a) != HEAP_OK) return 48;
    if (heap_free(&h, b) != HEAP_OK) return 49;
    if (heap_check(&h) != HEAP_OK) return 50;

    /* 15. 碎片压力:交替分配不同大小再全部释放,最终必须完全回收 */
    {
        void *ptrs[16];
        u32   i;
        u32   k;

        memset(st_area, 0, sizeof(st_area));
        if (heap_init(&h, (uintptr_t)st_area, ST_HEAP_BYTES) != HEAP_OK) return 51;

        for (k = 0; k < 4u; k++) {
            for (i = 0; i < 16u; i++) {
                ptrs[i] = heap_alloc(&h, 16u + i * 8u);
                if (ptrs[i] == NULL) return 52;
            }
            /* 隔一个释放,制造碎片 */
            for (i = 0; i < 16u; i += 2u) {
                if (heap_free(&h, ptrs[i]) != HEAP_OK) return 53;
            }
            for (i = 1; i < 16u; i += 2u) {
                if (heap_free(&h, ptrs[i]) != HEAP_OK) return 54;
            }
            if (heap_check(&h) != HEAP_OK) return 55;
        }

        /* 全部释放之后堆必须恢复原样 —— 否则说明合并有漏 */
        if (h.used_bytes != 0u) return 56;
        if (h.free_bytes != ST_HEAP_BYTES - HEAP_HEADER_SIZE) return 57;
        if (heap_largest_free(&h) != h.free_bytes) return 58;
    }

    return 0;
}
