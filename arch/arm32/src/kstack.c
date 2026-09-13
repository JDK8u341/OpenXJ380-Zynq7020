/*
 * 内核栈池 + guard page —— 纯逻辑实现(M4-5)
 *
 * 页表通过 vmap(M4-4)操作,TLB 维护通过调用方给的钩子外置,
 * 所以宿主编译器能直接编译本文件并跑自检。
 *
 * 设计理由(为什么 guard 在下面、为什么用"不映射"、为什么 TLB 由本模块拥有)
 * 全部写在 include/arch/kstack.h 顶部。
 */

#include <arch/kstack.h>
#include <arch/mmu.h>
#include <krlibc.h>

/* ------------------------------------------------------------------ */
/* 槽位几何                                                            */
/* ------------------------------------------------------------------ */

/*
 * 一个槽从低到高的布局:
 *
 *   slot_base(i) == guard(i)  <- 不可访问
 *   base(i)      == guard(i) + 1 页  <- 栈区底
 *   top(i)       == slot_base(i) + slot_pages * 页  <- 初始 SP
 *
 * 两条不变量(自检里逐条断言):
 *   guard(i) == base(i) - KSTACK_PAGE_SIZE
 *   guard(i+1) == top(i)          <- 槽与槽无缝相邻:不浪费、不重叠
 */
static u32 slot_base_of(const kstack_pool_t *p, u32 index)
{
    return p->pool_base + (index * p->slot_pages * KSTACK_PAGE_SIZE);
}

static u32 slot_guard_of(const kstack_pool_t *p, u32 index)
{
    return slot_base_of(p, index);
}

static u32 slot_stack_base_of(const kstack_pool_t *p, u32 index)
{
    return slot_base_of(p, index) + KSTACK_PAGE_SIZE;
}

static u32 slot_top_of(const kstack_pool_t *p, u32 index)
{
    return slot_base_of(p, index) + (p->slot_pages * KSTACK_PAGE_SIZE);
}

u32 kstack_slot_base(const kstack_pool_t *p, u32 index)
{
    if (p == NULL || !p->inited || index >= p->slot_count) {
        return 0u;
    }

    return slot_base_of(p, index);
}

bool kstack_slot_in_use(const kstack_pool_t *p, u32 index)
{
    if (p == NULL || !p->inited || index >= p->slot_count) {
        /* 越界当"占用"处理:调用方据此会拒绝释放,比放行安全 */
        return true;
    }

    return (p->bitmap[index >> 5] & (1u << (index & 31u))) != 0u;
}

/* ------------------------------------------------------------------ */
/* 记账                                                                */
/* ------------------------------------------------------------------ */

static void slot_mark(kstack_pool_t *p, u32 index, bool used)
{
    u32 mask = 1u << (index & 31u);

    if (used) {
        p->bitmap[index >> 5] |= mask;
        p->used_slots++;
        if (p->used_slots > p->peak_used) {
            p->peak_used = p->used_slots;
        }
    } else {
        p->bitmap[index >> 5] &= ~mask;
        if (p->used_slots > 0u) {
            p->used_slots--;
        }
    }
}

/* 首个空闲槽。找不到返回 slot_count */
static u32 slot_find_free(const kstack_pool_t *p)
{
    u32 i;

    for (i = 0; i < p->slot_count; i++) {
        if ((p->bitmap[i >> 5] & (1u << (i & 31u))) == 0u) {
            return i;
        }
    }

    return p->slot_count;
}

/* ------------------------------------------------------------------ */
/* 底层 vmap 的薄封装                                                  */
/* ------------------------------------------------------------------ */

static kstack_err_t vmap_result(kstack_pool_t *p, vmap_err_t e)
{
    if (e == VMAP_OK) {
        return KSTACK_OK;
    }

    p->last_vmap_err = e;

    return KSTACK_ERR_MAP;
}

static void flush_range(kstack_pool_t *p, u32 va_begin, u32 va_end)
{
    if (p->flush != NULL && va_begin < va_end) {
        p->flush(va_begin, va_end);
        p->stat_flush++;
    }
}

/*
 * ★ 让 guard 页变成"不可访问" ★
 *
 * 拆段会把**整段 256 页**都填成恒等映射,guard 页也在其中 ——
 * 所以"guard 不可访问"这件事不是默认成立的,必须显式做一次。
 *
 * ⚠ 这里有一个很容易漏掉的分支:guard 页可能落在**上一个 1MB 段**里
 *   (栈区起点正好贴着段边界时)。那一段还没被拆过,
 *   `vmap_unmap()` 会返回 VMAP_ERR_IS_SECTION,必须先拆段。
 *
 *   漏掉它的后果**极其隐蔽**:vmap_unmap 报错,而如果调用方不看返回值,
 *   guard 页就仍然是可访问的,溢出再也不会被抓到,页表里看上去却完全正常。
 *   自检里的 stat_guard_split 就是用来证明这条分支**真的被走到过**的。
 *
 * NOT_MAPPED 视为成功 —— 本函数的目标是"不可访问",已经不可访问就是达成,
 * 这也让它在"刚失败的分配需回滚"等重复调用场景下是幂等的。
 */
static kstack_err_t guard_unmap(kstack_pool_t *p, u32 guard)
{
    vmap_err_t e = vmap_unmap(p->vm, guard);

    if (e == VMAP_ERR_NOT_MAPPED) {
        return KSTACK_OK;
    }

    if (e == VMAP_ERR_IS_SECTION) {
        p->stat_guard_split++;
        e = vmap_split_section(p->vm, guard);
        if (e == VMAP_OK) {
            e = vmap_unmap(p->vm, guard);
        }
        if (e == VMAP_ERR_NOT_MAPPED) {
            return KSTACK_OK;
        }
    }

    return vmap_result(p, e);
}

/* 让 guard 页变成"不可访问" —— 按池配置的方式 */
static kstack_err_t guard_apply(kstack_pool_t *p, u32 guard)
{
    if (p->guard_kind == KSTACK_GUARD_AP_NONE) {
        vmap_attr_t none = vmap_attr_normal();
        vmap_err_t  e;

        none.ap3 = MMU_AP_NONE;

        e = vmap_lookup(p->vm, guard);

        /*
         * ⚠ 这里**不能**"拆完段就返回":拆段填出来的是恒等映射,
         *   也就是**可访问**的。第一版就是这么写的,于是 AP=none 的 guard
         *   在"该段尚未拆过"时静默变成一个普通可读写页 ——
         *   而它看上去"成功"了(返回 VMAP_OK)。
         *   拆完必须**重新取一次**,再按取到的状态落到 map 或 set_attr。
         */
        if (e == VMAP_RESULT_SECTION) {
            vmap_err_t s = vmap_split_section(p->vm, guard);

            if (s != VMAP_OK) {
                return vmap_result(p, s);
            }
            e = vmap_lookup(p->vm, guard);
        }

        if (e == 0u) {
            return vmap_result(p, vmap_map(p->vm, guard, guard, &none));
        }

        return vmap_result(p, vmap_set_attr(p->vm, guard, &none));
    }

    return guard_unmap(p, guard);
}

/*
 * ★ 把一个页从"任何状态"变成"我要的映射" —— vmap 的三态语义决定这必须分三步 ★
 *
 * 顺序不能换,而且第 2 步是最容易漏的:
 *
 *   1. **split** —— 段映射下先拆成小页。幂等:已经是页表时直接返回 OK。
 *   2. **unmap** —— 拆段会把**整段 256 页**都填成恒等映射,于是接下来
 *      `vmap_map()` 对这些页会以 `VMAP_ERR_ALREADY` 拒绝 ——
 *      vmap 刻意不静默覆盖已有映射(理由见 vmap.h 第 10 项)。
 *      漏掉这一步的症状很典型:**第一页成功、从第二页起全部 ALREADY**,
 *      也就是"分配 1MB 的栈只在第一页成功"。
 *   3. **map** —— 建立调用方要的映射。此后本页的映射完全由本槽拥有。
 *
 * ⚠ 由此推出一条对 guard 至关重要的不变量:
 *   **一个 1MB 段只会被拆一次,而拆一定发生在任何一页被单独控制之前或同时。**
 *   拆段填的是恒等映射,所以"拆段"本身是可见的、也是幂等的;而一旦
 *   某页被 unmap 成 guard,它所在的段**必然已经拆过**(guard_apply 会保证),
 *   因此不存在"guard 先被取消映射、之后又被一次拆段重新映射回来"的时序。
 *   换句话说 guard 不会静默消失 —— 这正是 M4-5 要的性质。
 */
static kstack_err_t page_claim(kstack_pool_t *p, u32 va, u32 pa, const vmap_attr_t *attr)
{
    vmap_err_t e;

    /*
     * `vmap_split_section` 对**既不是段也不是页表**的项(即 fault)也返回
     * `VMAP_ERR_IS_SECTION` —— 名字有误导,语义其实是"没有段可拆"。
     * 那种情况下后面 unmap 会得到 NOT_MAPPED、map 会正常建立,所以这里放行。
     */
    e = vmap_split_section(p->vm, va);
    if (e != VMAP_OK && e != VMAP_ERR_IS_SECTION) {
        return vmap_result(p, e);
    }

    e = vmap_unmap(p->vm, va);
    if (e != VMAP_OK && e != VMAP_ERR_NOT_MAPPED) {
        return vmap_result(p, e);
    }

    return vmap_result(p, vmap_map(p->vm, va, pa, attr));
}

/* ------------------------------------------------------------------ */
/* 句柄校验                                                            */
/* ------------------------------------------------------------------ */

/*
 * 三类拒绝,分得比"非法"细一档 —— 与 palloc 同一立场:
 * 句柄里的槽号越界、几何对不上、已经空闲,是三件不同的事,
 * 合成一个"非法"会让排障时少掉最关键的那一比特信息。
 */
static kstack_err_t handle_check(const kstack_pool_t *p, const kstack_t *k)
{
    u32 sb;

    if (p == NULL || k == NULL) {
        return KSTACK_ERR_BAD_ARGS;
    }
    if (!p->inited) {
        return KSTACK_ERR_NOT_INIT;
    }
    if (k->slot >= p->slot_count) {
        return KSTACK_ERR_BAD_HANDLE;
    }

    sb = slot_base_of(p, k->slot);

    /*
     * 几何必须与本池算出来的一致 —— 这条能挡住"来自另一个池的句柄":
     * 那种句柄的槽号可能合法,但 base/top 是按另一个池的起点算的。
     */
    if (k->guard != sb || k->base != (sb + KSTACK_PAGE_SIZE) ||
        k->top != (sb + (p->slot_pages * KSTACK_PAGE_SIZE)) || k->pages != p->stack_pages) {
        return KSTACK_ERR_BAD_HANDLE;
    }

    if (!k->in_use || (p->bitmap[k->slot >> 5] & (1u << (k->slot & 31u))) == 0u) {
        return KSTACK_ERR_DOUBLE_FREE;
    }

    return KSTACK_OK;
}

/* ------------------------------------------------------------------ */

kstack_err_t kstack_pool_init(kstack_pool_t *p, vmap_t *vm, u32 pool_base, u32 pool_size, u32 stack_pages,
                              kstack_guard_t guard_kind, u32 *bitmap, u32 bitmap_words,
                              kstack_flush_fn flush)
{
    u32 slot_bytes;
    u32 slots;

    if (p == NULL) {
        return KSTACK_ERR_BAD_ARGS;
    }
    if (vm == NULL || bitmap == NULL || flush == NULL) {
        return KSTACK_ERR_BAD_ARGS;
    }
    if (stack_pages == 0u || bitmap_words == 0u) {
        return KSTACK_ERR_BAD_ARGS;
    }
    if ((pool_base & (KSTACK_PAGE_SIZE - 1u)) != 0u) {
        return KSTACK_ERR_ALIGN;
    }

    slot_bytes = (stack_pages + 1u) * KSTACK_PAGE_SIZE;

    if (pool_size < slot_bytes) {
        return KSTACK_ERR_BAD_ARGS;
    }

    slots = pool_size / slot_bytes;
    if (slots > KSTACK_MAX_SLOTS) {
        slots = KSTACK_MAX_SLOTS;
    }
    if (slots == 0u) {
        return KSTACK_ERR_BAD_ARGS;
    }
    /* bitmap 必须装得下 slots 位 */
    if (bitmap_words < ((slots + 31u) / 32u)) {
        return KSTACK_ERR_BAD_ARGS;
    }

    memset(bitmap, 0, bitmap_words * sizeof(u32));

    p->vm           = vm;
    p->pool_base    = pool_base;
    p->slot_count   = slots;
    p->stack_pages  = stack_pages;
    p->slot_pages   = stack_pages + 1u;
    p->bitmap       = bitmap;
    p->bitmap_words = bitmap_words;
    p->flush        = flush;
    p->guard_kind   = guard_kind;

    p->used_slots     = 0u;
    p->peak_used      = 0u;
    p->stat_alloc     = 0u;
    p->stat_free      = 0u;
    p->stat_flush     = 0u;
    p->stat_guard_split = 0u;
    p->last_vmap_err  = VMAP_OK;
    p->inited         = true;

    return KSTACK_OK;
}

/*
 * 把一个槽整体恢复成"没有任何页可访问"。
 *
 * 分配失败时的回滚与 kstack_free 共用这一段 —— 两条路径要达成的状态
 * 是同一个("槽空闲,guard 不可访问,栈页未映射"),分成两份实现早晚会分叉。
 *
 * 返回第一个非 OK 的错误码;调用方决定是否容忍(回滚路径容忍,free 不容忍)。
 */
static kstack_err_t slot_release(kstack_pool_t *p, u32 slot)
{
    kstack_err_t first = KSTACK_OK;
    kstack_err_t e;
    u32          i;

    e = guard_unmap(p, slot_guard_of(p, slot));
    if (e != KSTACK_OK) {
        first = e;
    }

    for (i = 0; i < p->stack_pages; i++) {
        u32         va = slot_stack_base_of(p, slot) + (i * KSTACK_PAGE_SIZE);
        vmap_err_t  ve = vmap_unmap(p->vm, va);

        if (ve == VMAP_OK || ve == VMAP_ERR_NOT_MAPPED || ve == VMAP_ERR_IS_SECTION) {
            /* IS_SECTION:这一段压根没被拆过,也就不可能被本槽映射过 */
            continue;
        }
        if (first == KSTACK_OK) {
            first = vmap_result(p, ve);
        }
    }

    flush_range(p, slot_guard_of(p, slot), slot_top_of(p, slot));

    return first;
}

kstack_err_t kstack_alloc(kstack_pool_t *p, kstack_t *out)
{
    vmap_attr_t normal;
    kstack_err_t e;
    u32          slot;
    u32          guard;
    u32          base;
    u32          i;

    if (p == NULL || out == NULL) {
        return KSTACK_ERR_BAD_ARGS;
    }
    if (!p->inited) {
        return KSTACK_ERR_NOT_INIT;
    }

    slot = slot_find_free(p);
    if (slot >= p->slot_count) {
        return KSTACK_ERR_NO_SLOT;
    }

    /*
     * ★ 先占槽、再映射 ★
     *
     * 顺序反过来的话,映射中途失败会留下一个"半个槽被映射、
     * bitmap 却说是空闲"的状态 —— 下一次分配会在已映射的页上
     * 得到 VMAP_ERR_ALREADY,错误现场离真正的原因(上一次分配失败)很远。
     */
    slot_mark(p, slot, true);

    guard = slot_guard_of(p, slot);
    base  = slot_stack_base_of(p, slot);
    normal = vmap_attr_normal();

    /*
     * ★ 顺序:先 guard,再栈页 ★
     *
     * 第一版是反过来的(先映射栈页、最后处理 guard),理由是"拆段会把整段
     * 填成恒等映射,所以 guard 必须最后做"。那个理由只对**同一段内**成立,
     * 而且它带来一个更糟的后果:
     *
     *   若 guard 落在**上一个段**(栈区起点贴着段边界时会这样),映射栈页
     *   只拆了栈区那一段;等轮到 guard 时再去拆它那一段,而 L2 表池可能
     *   已经耗尽 —— 于是分配失败,而**回滚也清不掉这个 guard**(它要靠的
     *   正是同一张拿不到的表)。结果是"失败却留下一个半配置的槽"。
     *
     * 改成 guard 优先之后:
     *   - guard 那一段**先**被拆开并当场取消映射,于是无论后面在哪一步失败,
     *     回滚时的 guard_unmap 都是幂等的(已经是 NOT_MAPPED);
     *   - 后面拆栈区那一段**不会**影响 guard —— 若两者同段,拆段是幂等的
     *     (`vmap_split_section` 对已拆的段直接返回 OK,不重填表项),
     *     所以 guard 不会被"后来的第一次拆段"重新映射回来。
     *
     * 这一条与 page_claim 注释里的不变量是同一件事的两面。
     */
    e = guard_apply(p, guard);
    if (e != KSTACK_OK) {
        goto rollback;
    }

    /* 逐页建立栈区映射(恒等) */
    for (i = 0; i < p->stack_pages; i++) {
        u32 va = base + (i * KSTACK_PAGE_SIZE);

        e = page_claim(p, va, va, &normal);
        if (e != KSTACK_OK) {
            goto rollback;
        }
    }

    /*
     * 3. TLB。区间覆盖 guard 与全部栈页 —— 少刷 guard 那一页的话,
     *    拆段之前形成的**段表项**还在 TLB 里,guard 就静默失效了。
     */
    flush_range(p, guard, slot_top_of(p, slot));

    p->stat_alloc++;

    out->slot   = slot;
    out->guard  = guard;
    out->base   = base;
    out->top    = slot_top_of(p, slot);
    out->pages  = p->stack_pages;
    out->in_use = true;

    return KSTACK_OK;

rollback:
    /*
     * 尽力回滚。若回滚本身也失败(通常还是同一个 L2 池耗尽的原因),
     * 就**故意把槽留着不放**:泄漏一个槽远好过让后来的分配
     * 撞上一个半映射的槽,而那会表现成一个和本次失败毫无关系的症状。
     */
    if (slot_release(p, slot) == KSTACK_OK) {
        slot_mark(p, slot, false);
    }

    return e;
}

kstack_err_t kstack_free(kstack_pool_t *p, kstack_t *k)
{
    kstack_err_t e;

    e = handle_check(p, k);
    if (e != KSTACK_OK) {
        return e;
    }

    /*
     * 先把 guard 恢复。kstack_free 可能面对一个被 kstack_guard_disable()
     * 关过 guard 的槽(A/B 对照组用完之后),不恢复的话这个槽
     * 下次被分配出去时**没有 guard** —— 而那正是 M4-5 存在的理由。
     */
    e = slot_release(p, k->slot);
    if (e != KSTACK_OK) {
        return e;
    }

    slot_mark(p, k->slot, false);
    k->in_use = false;
    p->stat_free++;

    return KSTACK_OK;
}

/* ------------------------------------------------------------------ */
/* 破坏性 A/B:临时关掉 guard                                          */
/* ------------------------------------------------------------------ */

kstack_err_t kstack_guard_disable(kstack_pool_t *p, kstack_t *k)
{
    vmap_attr_t  normal = vmap_attr_normal();
    kstack_err_t r;
    vmap_err_t   cur;
    vmap_err_t   e;

    r = handle_check(p, k);
    if (r != KSTACK_OK) {
        return r;
    }

    cur = vmap_lookup(p->vm, k->guard);

    if (cur == VMAP_RESULT_SECTION) {
        /* guard 是未拆的段 —— 拆开它,拆出来就是恒等映射,已经"可访问"了 */
        e = vmap_split_section(p->vm, k->guard);
    } else if (cur == 0u) {
        e = vmap_map(p->vm, k->guard, k->guard, &normal);
    } else {
        e = vmap_set_attr(p->vm, k->guard, &normal);
    }

    if (e != VMAP_OK) {
        return vmap_result(p, e);
    }

    flush_range(p, k->guard, k->guard + KSTACK_PAGE_SIZE);

    return KSTACK_OK;
}

kstack_err_t kstack_guard_enable(kstack_pool_t *p, kstack_t *k)
{
    kstack_err_t e;

    e = handle_check(p, k);
    if (e != KSTACK_OK) {
        return e;
    }

    e = guard_apply(p, k->guard);
    if (e != KSTACK_OK) {
        return e;
    }

    flush_range(p, k->guard, k->guard + KSTACK_PAGE_SIZE);

    return KSTACK_OK;
}

/* ------------------------------------------------------------------ */
/* 破坏性验证:栈溢出探针                                              */
/* ------------------------------------------------------------------ */

/*
 * ⚠ 下面三个用 uintptr_t 而不是 u32。
 *
 *   在目标板(ARM32)上两者数值相同,但在宿主(x86_64)上 u32 会把
 *   指针截断,再转回指针就是野地址。宿主单测要用真实的宿主缓冲区
 *   来核对探针的地址算术,所以类型必须诚实地表达"这个值会被解引用"。
 *   本项目在 vmap 的 l2_pool / l2_pool_pa 上已经踩过一次同类的坑。
 */

uintptr_t kstack_overflow_addr(uintptr_t base, u32 index)
{
    return base - 4u - ((uintptr_t)index * 4u);
}

u32 kstack_overflow_word(u32 index)
{
    /* 图案带上下标,用来区分"写进去了"与"本来就是那个值" */
    return 0x5A5A0000u ^ index;
}

u32 kstack_overflow_write(uintptr_t base)
{
    u32 i;

    for (i = 0; i < KSTACK_OVERFLOW_WORDS; i++) {
        *(volatile u32 *)kstack_overflow_addr(base, i) = kstack_overflow_word(i);
    }

    return KSTACK_OVERFLOW_WORDS;
}

bool kstack_overflow_visible(uintptr_t base)
{
    u32 i;

    for (i = 0; i < KSTACK_OVERFLOW_WORDS; i++) {
        if (*(volatile u32 *)kstack_overflow_addr(base, i) != kstack_overflow_word(i)) {
            return false;
        }
    }

    return true;
}

static kstack_pool_t *g_probe_pool;
static kstack_t      *g_probe_stack;

void kstack_probe_register(kstack_pool_t *pool, kstack_t *stack)
{
    g_probe_pool  = pool;
    g_probe_stack = stack;
}

const kstack_t *kstack_probe_stack(void)
{
    return g_probe_stack;
}

u32 kstack_probe_overflow(void)
{
    if (g_probe_stack == NULL || !g_probe_stack->in_use) {
        return 0u;
    }

    /*
     * 第一个字落在 base-4,也就是 **guard 页的最后一个字**。
     * 用整数地址而不是 k->base 的指针算术:越过对象边界做指针算术
     * 在 C 里是未定义行为,而这里本来就是"故意越界"。
     */
    return kstack_overflow_write((uintptr_t)g_probe_stack->base);
}

bool kstack_probe_clobbered(void)
{
    if (g_probe_stack == NULL) {
        return false;
    }

    return kstack_overflow_visible((uintptr_t)g_probe_stack->base);
}

kstack_err_t kstack_probe_guard_disable(void)
{
    if (g_probe_pool == NULL || g_probe_stack == NULL) {
        return KSTACK_ERR_NOT_INIT;
    }

    return kstack_guard_disable(g_probe_pool, g_probe_stack);
}

kstack_err_t kstack_probe_guard_enable(void)
{
    if (g_probe_pool == NULL || g_probe_stack == NULL) {
        return KSTACK_ERR_NOT_INIT;
    }

    return kstack_guard_enable(g_probe_pool, g_probe_stack);
}

/* ------------------------------------------------------------------ */
/* 自检                                                                */
/* ------------------------------------------------------------------ */

/*
 * 合成实例。
 *
 * ⚠ pool_base 取 0x400FF000 而不是一个整齐的 1MB 边界,这是**刻意的**:
 *   槽大小 = 257 页 = 0x101000,与 1MB 不同余,所以槽边界逐槽相对 1MB
 *   段漂移 0x1000。取这个起点会**同时**造出两种 guard 位置:
 *
 *     - 槽 0:base = 0x40100000 段对齐 -> guard = 0x400FF000 落在**上一个段**,
 *       而那一段没有被任何栈区映射碰过,`vmap_unmap()` 会返回
 *       VMAP_ERR_IS_SECTION —— 必须先拆段。这是最容易漏、后果最隐蔽
 *       (guard 静默失效)的一条分支;
 *     - 槽 1:guard = 0x40200000 段对齐 -> 先被自己的栈区映射拆段覆盖,
 *       再被 guard_apply 取消映射。
 *
 *   两条路径都必须被走到,断言见自检第 22/33/34/35/36 项。
 */
#define ST_POOL_PAGES  32u
#define ST_STACK_PAGES 256u /* 1MB,与板级和源 OS 一致 */
#define ST_SLOT_PAGES  (ST_STACK_PAGES + 1u)
#define ST_POOL_BASE   0x400FF000u
#define ST_POOL_SIZE   (ST_POOL_PAGES * ST_SLOT_PAGES * KSTACK_PAGE_SIZE)

/* 池跨 0x400FF000..0x4211F000 共 34 个段,留到 48 张表 */
#define ST_L2_COUNT 48u

/*
 * ⚠ L1 用 8192 对齐:16KB 对齐是 TTBR0 的硬件要求(见 kernel.ld 的 ASSERT),
 *   而 Windows 上静态数组的最大对齐就是 8192。本自检不把表交给 MMU。
 */
static u32 st_l1[4096] __attribute__((aligned(8192)));
static u32 st_l2[ST_L2_COUNT * VMAP_L2_ENTRIES] __attribute__((aligned(1024)));
static u32 st_bitmap[KSTACK_BITMAP_WORDS];
static u32 st_bitmap2[KSTACK_BITMAP_WORDS];

static vmap_t       st_vm;
static kstack_pool_t st_pool;
static kstack_pool_t st_pool_ap;
static kstack_t      st_a;
static kstack_t      st_b;
static kstack_t      st_c;

static u32 st_flush_calls;
static u32 st_flush_first_begin;
static u32 st_flush_first_end;
static u32 st_flush_last_begin;
static u32 st_flush_last_end;

static void st_flush(u32 va_begin, u32 va_end)
{
    if (st_flush_calls == 0u) {
        st_flush_first_begin = va_begin;
        st_flush_first_end   = va_end;
    }

    st_flush_last_begin = va_begin;
    st_flush_last_end   = va_end;
    st_flush_calls++;
}

/* 合成物理地址:宿主指针是 64 位,塞不进 32 位描述符 */
#define ST_L2_PA 0x20000000u

/* 预置:整个池区间建为段映射(模拟真实的 DDR 恒等映射) */
static void st_seed_sections(void)
{
    vmap_attr_t normal = vmap_attr_normal();
    u32         base;

    memset(st_l1, 0, sizeof(st_l1));

    for (base = ST_POOL_BASE & ~(MMU_SECTION_SIZE - 1u); base < (ST_POOL_BASE + ST_POOL_SIZE);
         base += MMU_SECTION_SIZE) {
        st_l1[vmap_l1_index(base)] =
            mmu_section_descriptor(base, mmu_l1_section_attr(normal.ap3, normal.tex, normal.c, normal.b,
                                                             normal.domain, normal.shareable, normal.xn));
    }
}

u32 kstack_selftest(void)
{
    kstack_pool_t bad;
    kstack_t      bad_handle;
    u32           i;
    u32           ok;

    memset(st_l2, 0, sizeof(st_l2));
    memset(st_bitmap, 0, sizeof(st_bitmap));
    memset(st_bitmap2, 0, sizeof(st_bitmap2));
    memset(&bad, 0, sizeof(bad));
    st_seed_sections();

    /* ---- 1. 参数拒绝 ---- */
    if (kstack_pool_init(&bad, &st_vm, ST_POOL_BASE, ST_POOL_SIZE, 0u, KSTACK_GUARD_UNMAPPED, st_bitmap,
                         KSTACK_BITMAP_WORDS, st_flush) != KSTACK_ERR_BAD_ARGS) {
        return 1;
    }
    if (kstack_pool_init(&bad, &st_vm, ST_POOL_BASE, ST_POOL_SIZE, ST_STACK_PAGES, KSTACK_GUARD_UNMAPPED,
                         st_bitmap, KSTACK_BITMAP_WORDS, NULL) != KSTACK_ERR_BAD_ARGS) {
        return 2; /* 没有 TLB 钩子 = guard 会静默失效,必须拒绝 */
    }
    if (kstack_pool_init(&bad, &st_vm, ST_POOL_BASE, ST_POOL_SIZE, ST_STACK_PAGES, KSTACK_GUARD_UNMAPPED,
                         NULL, KSTACK_BITMAP_WORDS, st_flush) != KSTACK_ERR_BAD_ARGS) {
        return 3;
    }
    if (kstack_pool_init(&bad, &st_vm, ST_POOL_BASE + 1u, ST_POOL_SIZE, ST_STACK_PAGES,
                         KSTACK_GUARD_UNMAPPED, st_bitmap, KSTACK_BITMAP_WORDS, st_flush) !=
        KSTACK_ERR_ALIGN) {
        return 4;
    }
    if (kstack_pool_init(&bad, &st_vm, ST_POOL_BASE, ST_SLOT_PAGES * KSTACK_PAGE_SIZE - 1u, ST_STACK_PAGES,
                         KSTACK_GUARD_UNMAPPED, st_bitmap, KSTACK_BITMAP_WORDS, st_flush) !=
        KSTACK_ERR_BAD_ARGS) {
        return 5;
    }
    if (kstack_pool_init(&bad, NULL, ST_POOL_BASE, ST_POOL_SIZE, ST_STACK_PAGES, KSTACK_GUARD_UNMAPPED,
                         st_bitmap, KSTACK_BITMAP_WORDS, st_flush) != KSTACK_ERR_BAD_ARGS) {
        return 6;
    }

    /* ---- 2. vmap 实例 ---- */
    if (vmap_init(&st_vm, st_l1, st_l2, ST_L2_PA, ST_L2_COUNT, ST_POOL_BASE,
                  ST_POOL_BASE + ST_POOL_SIZE) != VMAP_OK) {
        return 7;
    }

    st_flush_calls = 0u;

    if (kstack_pool_init(&st_pool, &st_vm, ST_POOL_BASE, ST_POOL_SIZE, ST_STACK_PAGES,
                         KSTACK_GUARD_UNMAPPED, st_bitmap, KSTACK_BITMAP_WORDS, st_flush) != KSTACK_OK) {
        return 8;
    }

    /* ---- 3. 池被正确切分 ---- */
    if (st_pool.slot_count != ST_POOL_PAGES) return 9;
    if (st_pool.slot_pages != ST_SLOT_PAGES) return 10;
    if (st_pool.used_slots != 0u || st_pool.peak_used != 0u) return 11;
    if (kstack_slot_base(&st_pool, ST_POOL_PAGES) != 0u) return 12; /* 越界必须返回 0 */

    /* 未初始化就分配 */
    if (kstack_alloc(&bad, &st_a) != KSTACK_ERR_NOT_INIT) return 13;

    /* ---- 4. 第一个栈:几何 ---- */
    if (kstack_alloc(&st_pool, &st_a) != KSTACK_OK) return 14;
    if (st_a.slot != 0u) return 15;
    if (st_a.guard != ST_POOL_BASE) return 16;
    if (st_a.base != ST_POOL_BASE + KSTACK_PAGE_SIZE) return 17; /* guard 紧贴在栈下面 */
    if (st_a.top != ST_POOL_BASE + (ST_SLOT_PAGES * KSTACK_PAGE_SIZE)) return 18;
    if (st_a.pages != ST_STACK_PAGES) return 19;
    if ((st_a.top & 7u) != 0u) return 20; /* ★ AAPCS 要求 SP 8 字节对齐 ★ */
    if (st_pool.used_slots != 1u || st_pool.peak_used != 1u) return 21;

    /* ---- 5. ★ guard 真的不可访问 ★ ---- */
    if (vmap_lookup(&st_vm, st_a.guard) != 0u) return 22;

    /* 栈区 256 页全部恒等映射 */
    for (i = 0; i < ST_STACK_PAGES; i++) {
        u32 va  = st_a.base + (i * KSTACK_PAGE_SIZE);
        u32 got = vmap_lookup(&st_vm, va);

        if (got == 0u || got == VMAP_RESULT_SECTION) return 23;
        if ((got & ~0xFFFu) != va) return 24;
    }

    /*
     * ---- 6. ★ TLB 钩子被调用,且区间覆盖了 guard ★ ----
     *
     * "改完页表忘了刷 TLB"在本模块里不是性能问题,而是 guard 静默失效
     * (拆段前 TLB 里留着段表项)。把它变成可检查的性质,比写一句注释强。
     */
    if (st_flush_calls == 0u) return 25;
    if (st_flush_first_begin != st_a.guard) return 26;
    if (st_flush_first_end != st_a.top) return 27;

    /* ---- 7. 第二个栈与第一个无缝相邻 ---- */
    if (kstack_alloc(&st_pool, &st_b) != KSTACK_OK) return 28;
    if (st_b.slot != 1u) return 29;
    if (st_b.guard != st_a.top) return 30; /* ★ 不浪费、不重叠 ★ */
    if (st_b.base != st_a.top + KSTACK_PAGE_SIZE) return 31;
    if (vmap_lookup(&st_vm, st_b.guard) != 0u) return 32;

    /*
     * ---- 8. ★ 段边界上的 guard ★ ----
     *
     * 由 ST_POOL_BASE 的选取保证:槽 0 的 base = 0x40100000 正好是一个
     * 1MB 段的第一页,于是它的 guard = 0x400FF000 落在**上一个段**的
     * 最后一页 —— 而那一段没有被槽 0 的栈区映射碰过(栈区整好占满
     * 段 0x401 的 256 页)。
     *
     * 所以第 22 项走的一定是"先拆段、再取消映射"那条分支。
     * 这里断言的是**几何性质**(guard 与栈区不在同一段),而不只是一个
     * 硬编码的地址 —— 换个池起点时前者仍然成立。
     *
     * 槽 1 的 guard 则是**段对齐**的(0x40200000):它先被槽 1 自己的
     * 栈区映射拆段覆盖、再被 guard_apply 取消映射,走的是另一条路径。
     * 两条路径都要有,所以两个槽都断言。
     */
    if ((st_a.guard >> MMU_SECTION_SHIFT) == (st_a.base >> MMU_SECTION_SHIFT)) return 33;
    if (st_pool.stat_guard_split == 0u) return 34; /* 分支没被走到 = 那项检查是空的 */
    if ((st_b.guard & (MMU_SECTION_SIZE - 1u)) != 0u) return 35;
    if ((st_b.guard >> MMU_SECTION_SHIFT) != (((st_b.base >> MMU_SECTION_SHIFT)))) return 36;

    /* ---- 9. 释放之后槽的状态 ---- */
    if (kstack_free(&st_pool, &st_a) != KSTACK_OK) return 37;
    if (st_pool.used_slots != 1u) return 38;
    if (kstack_slot_in_use(&st_pool, 0u)) return 39;
    if (vmap_lookup(&st_vm, st_a.guard) != 0u) return 40; /* guard 仍在 */
    if (vmap_lookup(&st_vm, st_a.base) != 0u) return 41;  /* 栈页已解除 */
    if (vmap_lookup(&st_vm, st_a.top - KSTACK_PAGE_SIZE) != 0u) return 42;

    /* 首次适配:释放槽 0 之后再要,应当拿回槽 0 */
    if (kstack_alloc(&st_pool, &st_c) != KSTACK_OK) return 43;
    if (st_c.slot != 0u) return 44;
    if (st_c.base != st_a.base || st_c.top != st_a.top) return 45;

    /* ---- 10. 各种坏句柄 ---- */
    if (kstack_free(&st_pool, &st_a) != KSTACK_ERR_DOUBLE_FREE) return 46;

    bad_handle      = st_b;
    bad_handle.slot = st_pool.slot_count; /* 越界槽号 */
    if (kstack_free(&st_pool, &bad_handle) != KSTACK_ERR_BAD_HANDLE) return 47;

    bad_handle      = st_b;
    bad_handle.base = st_b.base + KSTACK_PAGE_SIZE; /* 几何对不上 */
    if (kstack_free(&st_pool, &bad_handle) != KSTACK_ERR_BAD_HANDLE) return 48;

    bad_handle       = st_b;
    bad_handle.guard = st_b.guard - KSTACK_PAGE_SIZE; /* guard 位置对不上 */
    if (kstack_free(&st_pool, &bad_handle) != KSTACK_ERR_BAD_HANDLE) return 49;

    if (kstack_free(NULL, &st_b) != KSTACK_ERR_BAD_ARGS) return 50;
    if (kstack_free(&st_pool, NULL) != KSTACK_ERR_BAD_ARGS) return 51;
    if (kstack_alloc(NULL, &bad_handle) != KSTACK_ERR_BAD_ARGS) return 52;

    /* ---- 11. guard 的开关(A/B 对照组用)---- */
    if (kstack_guard_disable(&st_pool, &st_b) != KSTACK_OK) return 53;
    {
        u32 got = vmap_lookup(&st_vm, st_b.guard);

        if (got == 0u || got == VMAP_RESULT_SECTION) return 54;
        if ((got & ~0xFFFu) != st_b.guard) return 55; /* 恒等映射回来 */
    }
    if (kstack_guard_enable(&st_pool, &st_b) != KSTACK_OK) return 56;
    if (vmap_lookup(&st_vm, st_b.guard) != 0u) return 57;

    /* ---- 12. AP=none 的 guard:补 M2-4 那笔账 ---- */
    if (kstack_pool_init(&st_pool_ap, &st_vm, ST_POOL_BASE, ST_POOL_SIZE, ST_STACK_PAGES,
                         KSTACK_GUARD_AP_NONE, st_bitmap2, KSTACK_BITMAP_WORDS, st_flush) != KSTACK_OK) {
        return 58;
    }
    if (kstack_alloc(&st_pool_ap, &st_a) != KSTACK_OK) return 59;
    {
        u32 got = vmap_lookup(&st_vm, st_a.guard);

        /* AP=none 的 guard 是**已映射**的 —— 拦住访问的是 AP 而不是"没映射" */
        if (got == 0u || got == VMAP_RESULT_SECTION) return 60;
        if ((got & ~0xFFFu) != st_a.guard) return 61;

        /* 小页的 AP[1:0] 在 bit5:4,AP[2] 在 bit9 */
        if (((got >> 4) & 0x3u) != 0u || ((got >> 9) & 0x1u) != 0u) return 62;

        if (kstack_guard_disable(&st_pool_ap, &st_a) != KSTACK_OK) return 63;
        got = vmap_lookup(&st_vm, st_a.guard);
        if (((got >> 4) & 0x3u) != 0x3u) return 64; /* 变回全权限 */

        if (kstack_guard_enable(&st_pool_ap, &st_a) != KSTACK_OK) return 65;
        got = vmap_lookup(&st_vm, st_a.guard);
        if (((got >> 4) & 0x3u) != 0u) return 66; /* 又变成无权限 */
    }
    if (kstack_free(&st_pool_ap, &st_a) != KSTACK_OK) return 67;

    /* ---- 13. 池满 ---- */
    {
        u32 n = 0u;

        while (kstack_alloc(&st_pool, &st_a) == KSTACK_OK) {
            n++;
            if (n > ST_POOL_PAGES + 4u) return 68; /* 停不下来 = 记账坏了 */
        }

        if (n != ST_POOL_PAGES - 2u) return 69; /* 槽 0/1 已被占用 */
        if (kstack_alloc(&st_pool, &st_a) != KSTACK_ERR_NO_SLOT) return 70;
        if (st_pool.used_slots != ST_POOL_PAGES) return 71;
        if (st_pool.peak_used != ST_POOL_PAGES) return 72;

        /* bitmap 与 used_slots 必须一致(派生量对不上是静默失真) */
        ok = 0u;
        for (i = 0; i < ST_POOL_PAGES; i++) {
            if (kstack_slot_in_use(&st_pool, i)) {
                ok++;
            }
        }
        if (ok != st_pool.used_slots) return 73;

        /* 全部释放 */
        for (i = 0; i < ST_POOL_PAGES; i++) {
            kstack_t k;
            u32      sb = kstack_slot_base(&st_pool, i);

            k.slot   = i;
            k.guard  = sb;
            k.base   = sb + KSTACK_PAGE_SIZE;
            k.top    = sb + (ST_SLOT_PAGES * KSTACK_PAGE_SIZE);
            k.pages  = ST_STACK_PAGES;
            k.in_use = true;

            if (kstack_free(&st_pool, &k) != KSTACK_OK) return 74;
        }

        if (st_pool.used_slots != 0u) return 75;
        for (i = 0; i < KSTACK_BITMAP_WORDS; i++) {
            if (st_bitmap[i] != 0u) return 76;
        }
        /* 全部释放之后每个槽的 guard 都必须仍然不可访问 */
        for (i = 0; i < ST_POOL_PAGES; i++) {
            if (vmap_lookup(&st_vm, kstack_slot_base(&st_pool, i)) != 0u) return 77;
        }
    }

    /* ---- 14. ★ 映射中途失败:必须完整回滚,不留半映射 ★ ---- */
    {
        static u32    st_l2_small[VMAP_L2_ENTRIES] __attribute__((aligned(1024)));
        static vmap_t st_vm_small;
        kstack_pool_t small;
        kstack_err_t  e;

        st_seed_sections();
        memset(st_l2_small, 0, sizeof(st_l2_small));
        st_flush_calls = 0u;

        /* 容量 1:几乎立刻会在需要第二张 L2 表时失败 */
        if (vmap_init(&st_vm_small, st_l1, st_l2_small, ST_L2_PA, 1u, ST_POOL_BASE,
                      ST_POOL_BASE + ST_POOL_SIZE) != VMAP_OK) {
            return 78;
        }
        if (kstack_pool_init(&small, &st_vm_small, ST_POOL_BASE, ST_POOL_SIZE, ST_STACK_PAGES,
                             KSTACK_GUARD_UNMAPPED, st_bitmap2, KSTACK_BITMAP_WORDS, st_flush) !=
            KSTACK_OK) {
            return 79;
        }

        e = kstack_alloc(&small, &st_a);
        if (e != KSTACK_ERR_MAP) return 80;
        if (small.last_vmap_err != VMAP_ERR_NO_L2) return 81;

        /*
         * ★ 失败必须"不留痕" ★
         *
         * 分配失败后槽若不是空闲的,下一次分配会去动一个半映射的槽,
         * 而那会以 VMAP_ERR_ALREADY 的形式表现成一个和本次失败毫无关系的症状。
         */
        if (small.used_slots != 0u) return 82;
        if (st_bitmap2[0] != 0u) return 83;
        /*
         * 槽的栈页必须**没有留下页级映射**:要么是 0(未映射),
         * 要么还是 VMAP_RESULT_SECTION(那一段压根没被拆过 —— 也就是
         * 我们什么都没写进去)。**不能**直接断言等于 0:段映射下
         * 这一页本来就能访问,而"能访问"是拆段之前就存在的状态,
         * 不是我们留下的。第一版就是这么写错的。
         */
        {
            u32 got = vmap_lookup(&st_vm_small, ST_POOL_BASE + KSTACK_PAGE_SIZE);

            if (got != 0u && got != VMAP_RESULT_SECTION) return 84;
        }

        /*
         * 失败路径必须也刷过 TLB:它可能已经拆了段(拆段会把 guard 页
         * 一起映射出来),不刷的话残留的段表项会让 guard 一直可访问。
         */
        if (st_flush_calls == 0u) return 85;
    }

    /*
     * ---- 15. ★ AP=none 的 guard 必须落在**还没拆过的段**上 ★ ----
     *
     * 上面第 12 节是在一个"早就被拆得七零八落"的页表上做 AP=none 的检查,
     * 于是它**测不到**下面这个错误:
     *
     *   拆段填出来的是恒等映射,也就是**可访问**的;
     *   如果实现是"看到段就拆、拆完就返回",那 AP=none 的 guard
     *   在段还没拆过时会静默变成一个普通可读写页 —— 而且返回 VMAP_OK。
     *
     * 这正是第一版的写法。要抓到它,必须重新种一遍段映射,
     * 让 guard 所在的段回到"未拆"状态。
     */
    {
        static vmap_t vm;
        kstack_pool_t ap;

        st_seed_sections();
        memset(st_l2, 0, sizeof(st_l2));
        st_flush_calls = 0u;

        if (vmap_init(&vm, st_l1, st_l2, ST_L2_PA, ST_L2_COUNT, ST_POOL_BASE,
                      ST_POOL_BASE + ST_POOL_SIZE) != VMAP_OK) {
            return 86;
        }
        if (kstack_pool_init(&ap, &vm, ST_POOL_BASE, ST_POOL_SIZE, ST_STACK_PAGES, KSTACK_GUARD_AP_NONE,
                             st_bitmap, KSTACK_BITMAP_WORDS, st_flush) != KSTACK_OK) {
            return 87;
        }
        if (kstack_alloc(&ap, &st_a) != KSTACK_OK) return 88;

        {
            u32 got = vmap_lookup(&vm, st_a.guard);

            if (got == 0u || got == VMAP_RESULT_SECTION) return 89;
            if ((got & ~0xFFFu) != st_a.guard) return 90;
            /* ★ AP[1:0] 必须是 0b00(无访问),不是拆段留下的 0b11 ★ */
            if (((got >> 4) & 0x3u) != 0u) return 91;
            if (((got >> 9) & 0x1u) != 0u) return 92;
        }
        if (kstack_free(&ap, &st_a) != KSTACK_OK) return 93;
    }

    return 0;
}
