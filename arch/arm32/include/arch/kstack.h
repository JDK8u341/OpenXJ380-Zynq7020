#pragma once

/*
 * 内核栈池 + guard page —— M4-5
 *
 * ====================================================================
 * 为什么需要它
 * ====================================================================
 *
 * M4-6/M4-7 要给每个任务发内核栈。若把这件事留给调用方"自己 malloc 一块内存
 * 当栈用",有两类错误会**不会当场出错**,而是很久以后以一个完全无关的症状崩掉:
 *
 *   1. **栈溢出** —— 写穿了栈底就进相邻内存。表现是"某个毫不相干的变量
 *      莫名其妙变了",而且只在栈用得深的那次调用里出现;
 *   2. **栈顶对齐** —— ARM 的 AAPCS 要求公开接口处 SP 8 字节对齐。
 *      不对齐不会报错,只会在某条 `vldr d16` 上炸 Undefined Instruction
 *      (本项目在 M4-2 已经因为 FPU 相关的事吃过一次同类亏)。
 *
 * 所以本模块的产出不是"能分配内存",而是 **一个越界就当场被抓住的栈**:
 * 每个栈的**紧下方留一页不可访问**(guard page),栈往下长穿它立刻产生
 * Data Abort,DFAR 正好落在 guard 页里。
 *
 * ====================================================================
 * ★ 槽位布局(guard 在栈的**下面**,不是上面)★
 * ====================================================================
 *
 * 栈是**向下**长的,所以 guard 必须在低地址一侧:
 *
 *      地址递增 ->
 *      +----------------+ <- slot_base(i) == guard(i)
 *      | guard 页(4KB) |    ★ 不可访问 ★
 *      +----------------+ <- base(i),栈区底
 *      |  栈页 0        |
 *      |  ...           |
 *      |  栈页 N-1      |
 *      +----------------+ <- top(i),初始 SP(高地址一端)
 *      | guard(i+1)     | <- 下一个槽的 guard,紧邻
 *
 * 于是有两条可核对的不变量(自检里逐条断言):
 *
 *   - `guard(i) == base(i) - KSTACK_PAGE_SIZE`
 *   - `guard(i+1) == top(i)` —— 槽与槽**无缝相邻**:不浪费、不重叠
 *
 * 一个自然的疑问:guard(i+1) 岂不就是 slot(i) 的栈顶那一页?
 * **不是。** slot(i) 的栈区是 `[base(i), top(i))`,右端是开区间 ——
 * `top(i)` 这一页属于 slot(i+1) 的 guard,slot(i) 永远不写它
 * (满递减栈第一次写落在 `top-4`)。
 *
 * 也就是说 slab 的粒度是"页",而**语义边界**落在页的边界上:
 * 一个槽的第 0 页是 guard,剩下 N 页是可用栈区。
 *
 * ====================================================================
 * ★ guard 用"不映射"实现,不用"没权限"实现 ★
 * ====================================================================
 *
 * 两种做法都能让访问失败,但**报出来的 DFSR 不同**,而且适用范围不同:
 *
 *   | 做法             | 机制                     | FS[4:0]        |
 *   |------------------|--------------------------|----------------|
 *   | L2 项清零        | 转换故障                 | 0x07 level 2   |
 *   | 映射但 AP=0b000  | 权限故障(需 DACR=client)| 0x0F level 2   |
 *
 * ⚠ 是 **level 2** 而不是 level 1:L1 项是好的页表描述符,
 *   出问题的是 **L2 小页项**。level 1 对应"L1 描述符本身就是 fault"
 *   (比如整段没有映射)。这两个不能混 —— 混了就会把
 *   "guard 拦住了"和"这一段根本没映射"当成同一件事。
 *
 * ⚠ 取 FS 时**不能用 `dfsr & 0x1F`**:DFSR 的 bits[7:4] 是 Domain,
 *   正好压在状态位上面。本内核 domain=15,真值 0x07 会被读成 0x17。
 *   完整说明见 src/gic.c 的 fsr_status_of()。这个坑是 guard page
 *   第一次上板时才暴露出来的。
 * 默认用**不映射**:
 *   - 页表里根本没有这一项,连投机访问都会被拦住;
 *   - 不依赖 DACR 的模式(本内核现在是全 client,但这是可变的);
 *   - 也不依赖 XN 之类"只管取指"的位。
 *
 * `AP=0b000` 那条路仍然保留(`KSTACK_GUARD_AP_NONE`),理由见
 * `kstack_guard_t` 的说明 —— 它是本项目欠 M2-4 的一笔账。
 *
 * ====================================================================
 * ★ 谁负责 TLB 维护 ★
 * ====================================================================
 *
 * 这是本模块刻意**没有**沿用"纯模块不管 CP15、由调用方收尾"那条惯例的地方。
 *
 * 理由:改完页表忘了失效 TLB,后果不是"慢一点",而是 **guard 静默失效** ——
 * 拆段之前那一整段是**段映射**,TLB 里留着段表项,于是 guard 页照样能访问,
 * 而页表里看上去完全正确。这种失败模式没有任何下游错误可查。
 *
 * 所以本模块**自己拥有** TLB 维护这件事,但把它做成一个**函数指针**
 * (`kstack_flush_fn`,由调用方在 init 时传入)。好处是两头都占:
 *   - 模块内部绝不会漏刷;
 *   - 纯逻辑部分仍然能在宿主机上编译,而且宿主单测可以**断言这个钩子
 *     被调用过、且区间覆盖了 guard 页** —— "忘记刷 TLB"于是从
 *     "只能靠人记得"变成了**可自动检查**的性质。
 *
 * ⚠ 已知限制(留给 M4-7):`arch_tlb_invalidate_page` 只失效**本核**的 TLB。
 *   等任务真的跑在 CPU1 上时,CPU1 那边可能还留着启动阶段的段表项,
 *   那时 guard 对 CPU1 是失效的。届时要么改用 TLBIMVAA 广播,
 *   要么用一个 IPI 让对端自己刷。**现在栈只由 CPU0 用,所以没有这个问题,
 *   但这不是"不需要处理",只是"还没轮到它"。**
 *
 * ====================================================================
 * 分层
 * ====================================================================
 *
 * 本文件与 src/kstack.c **不含 MMIO / CP15**:页表就是普通内存
 * (`vmap` 负责),TLB 通过钩子外置。所以宿主编译器能直接编译并跑自检 ——
 * 见 tests/test_arm32_kstack.py。
 *
 * 页表那部分交给 `vmap`(M4-4),本模块只负责
 * "按槽切分 + 保住 guard + 记账"。
 */

#include <arch/types.h>
#include <arch/vmap.h>

#define KSTACK_PAGE_SIZE (1u << VMAP_PAGE_SHIFT) /* 4096 */
#define KSTACK_PAGE_MASK (~(KSTACK_PAGE_SIZE - 1u))

/*
 * 槽数上限。定成编译期常量是为了让 bitmap 能静态开出来 ——
 * 与 palloc 同一个理由:分配器不能给自己分配存储。
 *
 * 64 而不是紧贴实际用量(板级现在是 32):留出的余量不花钱(8 字节),
 * 而"池开大一点就编不过"是个没有信息量的失败。
 */
#define KSTACK_MAX_SLOTS   64u
#define KSTACK_BITMAP_WORDS ((KSTACK_MAX_SLOTS + 31u) / 32u)

typedef enum
{
    KSTACK_OK = 0,
    KSTACK_ERR_NOT_INIT,
    KSTACK_ERR_BAD_ARGS,     /* 空指针 / stack_pages 为 0 / 段大小非页整数倍 */
    KSTACK_ERR_ALIGN,        /* pool_base 未按页对齐 */
    KSTACK_ERR_BAD_HANDLE,   /* 句柄不属于本池(槽号越界、base 对不上) */
    KSTACK_ERR_DOUBLE_FREE,  /* 释放一个已经空闲的槽 */
    KSTACK_ERR_NO_SLOT,      /* 池里没有空闲槽 */
    KSTACK_ERR_MAP,          /* 底层 vmap 失败;具体码见 pool->last_vmap_err */
} kstack_err_t;

/*
 * guard 页的实现方式。
 *
 * `KSTACK_GUARD_AP_NONE` 不是为了"另一种 guard",而是为了补一笔旧账:
 * M2-4 把 DACR 从全 manager 切成全 client 时,当时的理由是
 * "所有区域的 AP 都是 0b011(全权限),所以行为上应当完全不变" ——
 * 也就是说 **AP 到底有没有被硬件执行,当时并没有被验证过**。
 * 一个 AP=0b000 的页会让这件事变成可观测的:它必须产生
 * DFSR = 0x0E(permission fault, page)而不是 0x06。
 */
typedef enum
{
    KSTACK_GUARD_UNMAPPED = 0, /* L2 项清零 -> 转换故障(DFSR 0x06)*/
    KSTACK_GUARD_AP_NONE  = 1, /* 映射但 AP=0b000 -> 权限故障(DFSR 0x0E)*/
} kstack_guard_t;

/*
 * TLB 维护钩子。实现在 src/kstack_hw.c(kstack_tlb_flush_range),
 * 由调用方在 kstack_pool_init 时传入 —— 见文件顶部"谁负责 TLB 维护"。
 *
 * 区间是**左闭右开** [va_begin, va_end),且两端都按页对齐。
 */
typedef void (*kstack_flush_fn)(u32 va_begin, u32 va_end);

/*
 * 板级用的那个钩子(实现在 src/kstack_hw.c,CP15)。
 * 纯逻辑部分不引用它 —— 宿主单测传自己的桩函数,于是还能顺带
 * 断言"钩子被调用过、且区间覆盖了 guard 页"。
 */
void kstack_tlb_flush_range(u32 va_begin, u32 va_end);

/* 一个栈的句柄。**按值**返回给调用方,由调用方保存 */
typedef struct
{
    u32  slot;   /* 槽号 */
    u32  base;   /* 栈区底(低地址),= guard + 1 页 */
    u32  top;    /* 栈顶(初始 SP,高地址一端) */
    u32  guard;  /* guard 页地址,= base - 1 页 */
    u32  pages;  /* 栈区页数 */
    bool in_use;
} kstack_t;

typedef struct
{
    vmap_t *vm; /* 页表操作(M4-4)。本模块不自己碰描述符 */

    u32 pool_base;
    u32 slot_count;
    u32 stack_pages; /* 每个栈的数据页数(不含 guard) */
    u32 slot_pages;  /* stack_pages + 1 */

    u32 *bitmap; /* 由调用方提供,1 = 已分配。长度须 >= KSTACK_BITMAP_WORDS */
    u32  bitmap_words;

    kstack_flush_fn flush;
    kstack_guard_t  guard_kind;

    u32 used_slots;
    u32 peak_used; /* 历史最高,用于观察泄漏趋势(与 palloc 同一约定)*/
    u32 stat_alloc;
    u32 stat_free;
    u32 stat_flush;       /* 冲刷次数。宿主单测断言它 > 0 —— "忘了刷"是可检查的 */
    u32 stat_guard_split; /* guard 落在未拆的段上、走了"先拆再取消映射"的次数 */

    vmap_err_t last_vmap_err; /* 最近一次底层失败的原因,便于诊断 */
    bool       inited;
} kstack_pool_t;

/*
 * 初始化。池的物理内存由**调用方**从 palloc 一次性取得并把地址传进来
 * (板级是恒等映射,所以 VA == PA,这里用一个 pool_base 表达)。
 *
 * ⚠ 为什么不在 alloc/free 里向 palloc 要页、还页:
 *   池的意义就是"内存预先提交、反复复用"。逐次向 palloc 要页既慢,
 *   又做不到"归还后再拿到同一段"(palloc 不保证连续、也不保证复用),
 *   于是 guard 的位置会随分配次序漂移,出问题时无法复现。
 *   所以 `kstack_free` **不**把页还给 palloc —— 池的生命周期 = 系统生命周期。
 *
 * @param pool_size   字节数;不足一个槽(或多余部分凑不满一个槽)的部分会被忽略
 * @param stack_pages 每个栈的数据页数。**不含** guard 页
 * @param guard_kind  见 kstack_guard_t;通常用 KSTACK_GUARD_UNMAPPED
 * @param bitmap      调用方提供的位图,长度须 >= KSTACK_BITMAP_WORDS
 * @param flush       TLB 维护钩子;传 NULL 会被拒绝(见文件顶部,这不是可选的)
 */
kstack_err_t kstack_pool_init(kstack_pool_t *p, vmap_t *vm, u32 pool_base, u32 pool_size, u32 stack_pages,
                              kstack_guard_t guard_kind, u32 *bitmap, u32 bitmap_words,
                              kstack_flush_fn flush);

/*
 * 分配一个栈。
 *
 * 成功时 out 里是完整句柄:`top` 可以直接当初始 SP 用(已按 8 字节对齐)。
 * 池满是返回 KSTACK_ERR_NO_SLOT,不做任何部分映射 —— 失败不留痕。
 */
kstack_err_t kstack_alloc(kstack_pool_t *p, kstack_t *out);

/*
 * 释放一个栈:guard 重新生效、栈区解除映射、槽位标记为空闲。
 *
 * 会拒绝:句柄不属于本池(BAD_HANDLE)、已经空闲(DOUBLE_FREE)。
 * 与 palloc 同一立场:静默接受双重释放会让 bug 推迟到很久以后才以无关症状出现。
 */
kstack_err_t kstack_free(kstack_pool_t *p, kstack_t *k);

/* 槽 i 的起始地址(即该槽 guard 页的地址)。index 越界返回 0 */
u32 kstack_slot_base(const kstack_pool_t *p, u32 index);

/* 槽 i 是否已分配。index 越界返回 true(把"越界"当"占用"处理更安全)*/
bool kstack_slot_in_use(const kstack_pool_t *p, u32 index);

/* ------------------------------------------------------------------ */
/* 破坏性 A/B:临时关掉某个栈的 guard                                   */
/* ------------------------------------------------------------------ */

/*
 * 把 guard 页改成"可访问"。
 *
 * ⚠ **这个函数的存在只为一个目的:做对照组。**
 *   要证明 guard 是承重的,唯一的办法是"同一个地址、同一条指令,
 *   只在 guard 有/无这一件事上不同,看行为是否随之改变"。
 *   生产代码路径里不应该出现它。
 */
kstack_err_t kstack_guard_disable(kstack_pool_t *p, kstack_t *k);

/* 把 guard 恢复成不可访问 */
kstack_err_t kstack_guard_enable(kstack_pool_t *p, kstack_t *k);

/* ------------------------------------------------------------------ */
/* 破坏性验证:制造一次栈溢出                                          */
/* ------------------------------------------------------------------ */

/*
 * 从栈底向下写 KSTACK_OVERFLOW_WORDS 个字,第一个字就落在 guard 页里。
 *
 * 模拟真实的栈溢出方向(SP 递减):地址依次为 base-4, base-8, ...
 *
 * - **guard 生效**时:第一次写就产生 Data Abort,DFAR = base-4,**函数不返回**;
 * - **guard 被关掉**时:整段写完返回,返回值 = 实际写入的字数,
 *   并可由 kstack_probe_clobbered() 核对那段内存确实被写脏了。
 *
 * 两种情形合起来才是第三级证据:不是"guard 页确实没映射"(那是读回),
 * 而是"去掉它行为就变了"。
 */
u32 kstack_probe_overflow(void);

/* 无 guard 情形下核对:guard 页顶部那一段确实被覆写成了探针图案 */
bool kstack_probe_clobbered(void);

/*
 * 登记破坏性验证用的池/栈与溢出次数字。
 *
 * 放在模块里而不是让 fault_test 自己持有,是因为"哪个栈用于验证"
 * 这件事只有 kmain 知道(它在栈池就绪之后才登记),而触发点
 * (fault_test_poll)在别处。
 *
 * 分两个槽位而不是一个:`guard_kind` 是**池级**配置,所以
 * "不映射"与"AP=0b000"这两种 guard 必然是**两个池**。
 * 合成一个只会让选择器 7 悄悄退化成"不映射"那一种 ——
 * 那样 6 和 7 看起来都验证过了,实际上只验了一种。
 */
void kstack_probe_register(kstack_pool_t *pool, kstack_t *stack);

/* 上面两个 guard 开关的探针版本(作用于已登记的栈) */
kstack_err_t kstack_probe_guard_disable(void);
kstack_err_t kstack_probe_guard_enable(void);

/* 已登记的探针栈;未登记返回 NULL */
const kstack_t *kstack_probe_stack(void);

/*
 * AP=0b000 那一路的独立登记与触发。
 *
 * 它的 A/B 比"不映射"那一路**更紧**:两个阶段里 guard 页**都是映射着的**,
 * 唯一的差别就是 AP 是不是 0b000。于是它同时回答两个问题:
 *   1. `KSTACK_GUARD_AP_NONE` 这条实现路径在真硬件上成不成立;
 *   2. ★ **DACR = client 模式下 AP 到底有没有被硬件执行** ★ ——
 *      这是 M2-4 欠下的一笔账:当时把 DACR 从全 manager 切成全 client,
 *      理由是"所有区域的 AP 都是 0b011,所以行为应当完全不变",
 *      也就是说 **AP 有没有被强制执行,从来没有被验证过**。
 */
void kstack_probe_register_ap(kstack_pool_t *pool, kstack_t *stack);

/* 对 AP 探针栈跑同一段溢出。guard 生效时不返回 */
u32  kstack_probe_overflow_ap(void);
bool kstack_probe_clobbered_ap(void);
kstack_err_t kstack_probe_guard_disable_ap(void);
kstack_err_t kstack_probe_guard_enable_ap(void);
const kstack_t *kstack_probe_stack_ap(void);

/* 一次溢出要写多少个字。64 字 = 256 字节,足以越过 guard 页的第一行 */
#define KSTACK_OVERFLOW_WORDS 64u

/*
 * 探针的地址算术与图案 —— 从"真正会解引用地址的那两个函数"里抽出来,
 * 于是宿主单测可以在**真实的宿主缓冲区**上核对这些性质,
 * 而不需要真的有一页不可访问的内存:
 *
 *   - 第一个字落在 base-4,也就是 guard 页的**最后一个字**;
 *   - 第 i 个字落在 base-4-4i,地址**单调递减**(与 SP 方向一致);
 *   - ★ 整段写入必须**装得进一页** ★ —— `KSTACK_OVERFLOW_WORDS` 一旦
 *     被改到超过 1024,"无 guard"那组对照就会一路写穿 guard 页
 *     冲进下面那个槽的栈区,于是 A/B 会破坏一对无关的栈。
 *     这条不该靠人记得,宿主单测直接断言。
 *
 * ⚠ 用 uintptr_t 而不是 u32:目标板上两者数值相同,但宿主是 64 位,
 *   u32 会把指针截断成野地址(本项目在 vmap 的 l2_pool 上踩过一次)。
 */
uintptr_t kstack_overflow_addr(uintptr_t base, u32 index);
u32       kstack_overflow_word(u32 index);

/* 真正会解引用的两个:只在"地址确实可用"时调用 */
u32  kstack_overflow_write(uintptr_t base);
bool kstack_overflow_visible(uintptr_t base);

/* ------------------------------------------------------------------ */

/*
 * 自检:在**独立的合成实例**上跑,返回 0 = 全过,非 0 = 第几项失败。
 *
 * 用合成实例而不是真实池(与 palloc/heap/vmap 同一约定):错误路径
 * (池满、双重释放、映射中途失败)需要精确控制状态,在真实池上做
 * 会受当前用量影响,而且会真的吃掉内核在用的栈。
 *
 * ⚠ 合成实例刻意选一个**与 1MB 段不对齐**的池起点,因为那会触发
 *   一个真实存在但很容易漏掉的分支:某个槽的 guard 页正好落在
 *   **上一段**的第一页,那一段还没被拆过,`vmap_unmap()` 会返回
 *   `VMAP_ERR_IS_SECTION` —— 必须先拆段。见 kstack.c 的 guard_unmap()。
 */
u32 kstack_selftest(void);
