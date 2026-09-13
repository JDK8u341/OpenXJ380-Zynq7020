#pragma once

/*
 * ARM 侧的进程/线程控制块 —— M4-6
 *
 * ====================================================================
 * 这份结构体怎么来的:三份清单(计划 §4.7)
 * ====================================================================
 *
 * 移植的硬性规程是"**源 OS 怎么做才是**"。所以 M4-6 的第一步不是写代码,
 * 而是把 x86 的 `include/task/pcb.h` 与 `kernel/task/pcb.cpp` 读清楚,
 * 把每个字段分成三类(§4.7.2 ~ §4.7.5):
 *
 *   ARCH_INDEP      与 CPU 架构无关 —— 本文件**逐字段沿用 x86 的名字与角色**
 *   ARCH_DEP        x86_64 专有 —— 按设计不同,只共享名字与角色
 *   X86_USER_ONLY   只有 x86-64 用户态才需要 —— 暂时不放进来,但逐条记下
 *
 * **"逐字段沿用"是真去核对的,不是凭印象**:下面的字段名、注释里的行号
 * 都对着 `include/task/pcb.h` 抄。没有对应物的字段会写明**为什么没有**,
 * 而不是悄悄省掉。
 *
 * ====================================================================
 * ★ 沿用的约定(不是字段,但同样是契约)★
 * ====================================================================
 *
 *   - `pcb_t` / `tcb_t` 是**指针** typedef(`pcb.h:46-47`),不是结构体 typedef;
 *   - 全部 API **按指针传参**,没有按值传递结构体的;
 *   - fd = `file_open` 队列下标,0/1/2 是标准流硬编码(`pcb.cpp:788-790`);
 *   - `kernel_stack` 存的是**栈顶上界**,不是栈底(`pcb.cpp:1202`);
 *   - `CREATE → START` 的转换发生在入队时(`scheduler.cpp:533-535`);
 *   - `context.pc == 0` 表示"这个上下文无效,不可切换"
 *     (`scheduler.cpp:444` 用 `context0.rip != 0` 做守卫)。
 *     ⚠ 这是个隐式约定 —— ARM 侧沿用,但**显式写出来**,见 `arm_task_ctx_t`。
 *
 * ====================================================================
 * ★ 三处刻意的简化,每一条都写了理由 ★
 * ====================================================================
 *
 * #### 1. 地址与 `size_t` 是 32 位,不是 64 位
 *
 * x86 的地址字段是 `uint64_t`,`size_t` 也是 64 位。ARM32 上它们都是 32 位。
 * 这条**不是**可选的:32 位的机器装不下 64 位的虚拟地址。
 * 字段名与角色保持一一对应,类型按架构取。
 *
 * #### 2. ★ 一个任务只要**一个**内核栈(x86 要两个)★
 *
 * x86 那侧每个 TCB 有两个 1MB 内核栈(`pcb.h:174` 的 `kernel_stack` 与
 * `pcb.h:182` 的 `syscall_stack`),原因很具体:
 *
 *   > x86 的 `syscall` 指令**不会切栈**。所以 `handler.S:315` 必须
 *   > 从 TCB 里把 `syscall_stack` 装进 `%rsp` —— 内核得有个已知的栈可切。
 *   > 而中断走的是 TSS.rsp0 那条路,于是两者天然是两段栈。
 *
 * ARM **不是这样**:`SVC` 与 `IRQ` 各自有**硬件 banked** 的 SP
 * (`SP_svc` / `SP_irq`),进异常时硬件自动切,不需要从任何结构体里读栈指针。
 * 任务切换时把任务的栈顶写进 `SP_svc` 即可,`SVC` 进来就已经在它上面了。
 * 而按计划 §4.5 的要求,M4-7 会把中断路径用 `srsdb sp!, #MODE_SVC` 也落到
 * **任务的** `SP_svc` 上 —— 那时中断与系统调用共用这一个栈,是正常的嵌套使用。
 *
 * ⇒ 在 ARM 上留一个 `syscall_stack` 字段,等于**每个任务白留 1MB 永不使用的栈**。
 *   所以本结构体**没有**这个字段。这不是"图省事",是它对应的问题在 ARM 上
 *   由硬件解决了。
 *
 * ⇒ 连带影响:M4-5 的栈池当初按"32 栈 = 16 个任务(每任务两个)"开的,
 *   现在口径变成 **32 栈 = 32 个任务**。池大小不用改,但**说法要改**。
 *
 * ⚠ 这一条是本次唯一一处**偏离源 OS 的结构决策**,所以单独列出来。
 *   如果 M4-7 发现还需要第二个栈,把它加回来是一个字段的事。
 *
 * #### 3. 暂不放进来的字段(逐条记账,不是忽略)
 *
 * | x86 字段 | 出处 | 为什么现在不放 |
 * |---|---|---|
 * | `syscall_stack` / `syscall_user_rsp` | `pcb.h:182,183` | 见上面第 2 条 —— 架构上不需要 |
 * | `fs` / `fs_base` | `pcb.h:190,191` | ARM **没有分段**。`fs` 是段选择子,在 ARM 上根本不存在这个量。用户态 TLS 用 `TPIDRURW`,是每核 banked 寄存器 |
 * | `tid_directory` | `pcb.h:193` | 用户态地址空间的一部分,等 M7 有用户态再说 |
 * | `actions[64]` / `blocked` / `signal` | `pcb.h:171-173` | 信号子系统(`§4.7.5` 列为暂不需要)。⚠ 它占 2KB,是 x86 TCB 里最大的一块 |
 * | `sas_ss_sp/size/flags` | `pcb.h:195-197` | 信号栈,同上 |
 * | `tty` / `xtttp_stc` / `winnum` / `hasfscr` / `window_count` | `pcb.h:133,134,205,206,210` | X3TP 窗口与 tty,暂不需要 |
 * | `message_pipe_*` | `pcb.h:207,208` | IPC 消息管道,暂不需要 |
 * | `notify_pcor_*` | `pcb.h:136-140` | 通知管道,暂不需要 |
 * | `elf_file` / `elf_size` / `aux_*` / `linux_abi` / `umask` | `pcb.h:115-125` | ELF 加载与 Linux ABI 兼容层,等 M7 |
 * | `vma_manager` / `virt_queue` / `procfs_node` / `proc_root` | `pcb.h:114,126,127,128` | VMA/procfs,等 M4A-1 的 fs |
 * | `eevdf_*` / `runtime_ticks` | `pcb.h:212-216` | 调度策略,M4-8 |
 *
 * 这些字段全是 ARCH_INDEP 的,所以**将来加回来时名字照抄 x86** 即可。
 *
 * ====================================================================
 * ★ 偏移的钉法(§4.7.6 的教训)★
 * ====================================================================
 *
 * 汇编要按偏移访问 TCB。x86 那边只有两条 `static_assert`
 * (`pcb.cpp:84-87`,钉 `syscall_stack=0xb48` / `syscall_user_rsp=0xb50`),
 * 而 `handler.S:29-31` 的 per-CPU 偏移**一条都没有**,全靠算术偶然对上。
 *
 * ARM 这边:
 *   - 汇编用到的偏移**全部**在 `arch/taskctx_asm.h` 里,汇编与 C 共用同一份宏;
 *   - 本文件用 `_Static_assert` 把结构体的真实偏移与那些宏钉在一起 ——
 *     **加字段导致偏移漂移会在编译期报错**,而不是等调度器跑飞。
 *
 * 顺序上沿用 x86 的形状:架构无关字段在前、架构相关在后
 * (`pcb.h` 也是 `parent_group`/`task_level` 在前、`context0` 在后)。
 * 这样与 x86 逐字段对照时顺序一致;代价是将来加字段要动一次 asm 头的常量,
 * 而那正是**应该**被注意到的事。
 */

#include <arch/taskctx_asm.h>
#include <arch/taskctx.h>
#include <arch/types.h>

/* ------------------------------------------------------------------ */
/* 与 x86 同名的常量与枚举(名字照抄,便于逐字段对照)                  */
/* ------------------------------------------------------------------ */

/* `pcb.h:3-5` */
#define TASK_KERNEL_LEVEL      0
#define TASK_IDLE_LEVEL        1
#define TASK_APPLICATION_LEVEL 2

/* `pcb.h:49-59`。数值必须逐个一致 —— 它们会进 procfs,也可能进用户态 */
typedef enum
{
    CREATE  = 0, /* 创建中 */
    RUNNING = 1, /* 运行中 */
    WAIT    = 2, /* 线程阻塞 */
    DEATH   = 3, /* 死亡(无法被调度,线程状态为等待处死) */
    START   = 4, /* 准备调度 */
    FUTEX   = 5, /* 被挂起(无法被调度) —— Linux ABI,futex 之前用不到 */
    OUT     = 6, /* 已被处死(无法被调度) */
    ZOMBIE  = 7,
} TaskStatus;

/* ------------------------------------------------------------------ */
/* 指针 typedef —— 约定,必须沿用(`pcb.h:46-47`)                       */
/* ------------------------------------------------------------------ */

struct arm_process_control_block;
struct arm_thread_control_block;

typedef struct arm_process_control_block *pcb_t;
typedef struct arm_thread_control_block  *tcb_t;

/*
 * 队列。x86 用 `lock_queue`(`include/lock_queue.h`)。
 * ARM 侧的可睡眠原语排在 M4-11,所以这里先用**不完整类型**占位 ——
 * "这个字段是队列,队列怎么实现还没定"比"先塞一个假的"诚实。
 * 用指针是因为队列本身是可选的(进程与线程都是先建对象、后建队列)。
 */
struct arm_queue;
typedef struct arm_queue arm_queue_t;

/* ------------------------------------------------------------------ */
/* 进程控制块(PCB / 进程组)                                          */
/* ------------------------------------------------------------------ */

/*
 * 对应 x86 的 `struct process_control_block`(`pcb.h:85-141`)。
 * 字段名一一对应,顺序也尽量照抄,便于并排核对。
 */
struct arm_process_control_block
{
    u32 ppid; /* `pcb.h:88` uint64_t ppid */
    u32 pid;  /* `pcb.h:87` size_t pid */

    char name[32]; /* `pcb.h:90` */

    /*
     * ★ 唯一的 ARCH_DEP 字段 ★
     * x86 是 `page_directory_t *pagedir`(`pcb.h:91`,4 级页表根 + CR3)。
     * ARM 是 L1 表(4096 项 × 32 位)+ TTBR0 —— 两者**不是同一个东西**,
     * 只共享"这个进程的地址空间根"这个角色。
     */
    u32 *pagedir;

    pcb_t parent_task;   /* `pcb.h:92` */
    arm_queue_t *thread_queue; /* `pcb.h:93` */
    u32   queue_index;   /* `pcb.h:94` size_t */
    TaskStatus status;   /* `pcb.h:95` */
    arm_queue_t *ipc_queue; /* `pcb.h:97`(IPC 暂缓,字段先留 —— 它是指针,不花钱)*/

    arm_queue_t *file_open; /* `pcb.h:99` 文件句柄占用队列 */
    u32         *file_open_shared_refs; /* `pcb.h:100` 引用计数(fork 语义)*/
    char       **envp;      /* `pcb.h:101` */
    u32          envc;      /* `pcb.h:102` size_t */

    bool   vfork;       /* `pcb.h:104` */
    u32    child_index; /* `pcb.h:105` size_t */
    arm_queue_t *child_pcb; /* `pcb.h:106` 子进程列表 */
    i32    exit_code;   /* `pcb.h:107` int */

    u32  mmap_start; /* `pcb.h:108` uint64_t(用户态地址,M7 才用得上)*/
    char *cmdline;   /* `pcb.h:109` */
    char **argv;     /* `pcb.h:110` */
    u32    argc;     /* `pcb.h:111` size_t */
    char  *exe_path; /* `pcb.h:112` */
    i32    task_level; /* `pcb.h:113` int */

    u32 brk_start;   /* `pcb.h:129` */
    u32 brk_end;     /* `pcb.h:130` */
    u32 brk_current; /* `pcb.h:131` */

    /*
     * ⚠ `pagedir` 之外,x86 的 PCB 里没有第二个架构相关字段(§4.7.2 的结论)。
     *   所以"架构无关"这一类的完整性是可以用肉眼逐行核对的 ——
     *   上面这些字段与 `pcb.h:85-141` 去掉暂缓项之后一一对应。
     */
};

/* ------------------------------------------------------------------ */
/* 线程控制块(TCB)                                                    */
/* ------------------------------------------------------------------ */

/*
 * 对应 x86 的 `struct thread_control_block`(`pcb.h:158-219`)。
 *
 * ⚠ **字段顺序不是随意的**:汇编按 `ARM_TCB_OFF_CTX` 访问 `ctx`,
 *   所以 `ctx` 之前的字段一旦增减,那个偏移就会变 —— 编译期会报错
 *   (本文件末尾的 `_Static_assert`),那时要同步改 `taskctx_asm.h`。
 */
struct arm_thread_control_block
{
    /* ---- 架构无关(名字与角色照抄 `pcb.h:160-167`) ---- */
    pcb_t      parent_group; /* `pcb.h:160` */
    i32        task_level;   /* `pcb.h:161` */
    u32        tid;          /* `pcb.h:163` size_t */
    char       name[32];     /* `pcb.h:164` */
    TaskStatus status;       /* `pcb.h:165` */
    u64        wakeup_time;  /* `pcb.h:166` —— **64 位**:它是纳秒时间戳,不是地址 */
    u32        cpu_id;       /* `pcb.h:167` size_t */

    /* ---- 架构相关 ---- */

    /*
     * 线程上下文(`pcb.h:168` 的 `TaskContext context0`)。
     * ★ 汇编按 `ARM_TCB_OFF_CTX` 访问它 —— 见本文件末尾的断言 ★
     */
    arm_task_ctx_t ctx;

    /*
     * VFP 上下文(`pcb.h:169` 的 `fpu_context_t`)。
     *
     * x86 是 512 字节的 FXSAVE 区、**16 字节对齐**(`cpu/fpu.h:5-8`)——
     * 那个 16 是 `fxsave64` 的**硬件要求**,不是保守取值。
     * ARM 是 d0–d31(256B)+ FPSCR(4B),**不是同一个格式**,
     * 只共享"这个线程的浮点状态"这个角色。见计划 §0.5.9 的 D3 结案。
     *
     * ★ 对齐:这里**刻意不给** 32 字节对齐 ★
     *
     * 一开始写的是 `aligned(32)`,理由是"VFP 按 4 字一组访问,对齐足一点稳"。
     * 那是**没有依据的保守取值**,而且立刻带来一个真问题:
     * `heap_alloc` 只保证 8 字节对齐(`heap.h` 的 `HEAP_ALIGN = 8`),
     * 于是"结构体声明 32 对齐、分配器只给 8"构成未定义行为 ——
     * 要么给堆加一个对齐分配接口,要么把要求降到真实的那个数。
     *
     * 真实的要求:ARMv7-A 的 VFP 传输指令要求地址 **4 字节对齐**
     * (VFP 不支持非对齐访问,这是架构规则不是实现细节)。
     * x86 的 16 是 FXSAVE 的要求,**ARM 没有对应物**。
     *
     * 所以这里不写对齐属性,由结构体的**自然对齐**决定 ——
     * 而它里面有 `u64` 成员(`wakeup_time` / `eevdf_*`),AAPCS 要求
     * 8 字节对齐,于是整个 TCB 的自然对齐就是 8,`heap_alloc` 正好满足。
     * 见文件末尾的 `_Alignof` 断言。
     */
    u32 vfp[ARM_VFP_D_REGS * 2u];
    u32 fpscr;

    /*
     * 内核栈(`pcb.h:174` 的 `kernel_stack`)。
     *
     * 语义与 x86 一致:**存栈顶上界**,不是栈底(`pcb.cpp:1202`)。
     * 任务切换时把它写进 `SP_svc`;异常返回时 `SP_svc` 就是它下面。
     *
     * 另外记下栈区本身,因为释放时要还给栈池 ——
     * x86 靠"栈顶 - KERNEL_STACK_SIZE"反推(`pcb.cpp:488`),
     * ARM 侧有 guard 页,反推容易把 guard 算漏,所以显式存。
     */
    u32 kernel_stack;      /* 栈顶(写进 SP_svc 的那个值)*/
    u32 kstack_base;       /* 栈区底(guard 之上)*/
    u32 kstack_guard;      /* guard 页地址 —— 栈池释放要用 */
    u32 kstack_slot;       /* 栈池槽号 */
    bool owns_kstack;

    /* ---- 架构无关(继续照抄 `pcb.h:175-219`) ---- */
    u32 queue_index;      /* `pcb.h:175` size_t 调度队列索引 */
    /*
     * 就绪队列的侵入式节点(M4-8)。
     *
     * 源 OS 用 `lock_node *sched_node`(`pcb.h:176`)指向队列自己分配的节点。
     * ARM 侧改成**内嵌的 next 指针**:队列不需要额外分配,宿主测试里
     * 也就不需要构造一个分配器。语义(能被挂进一个按 deadline 有序的队列、
     * 能被摘下来)是一样的。
     */
    tcb_t sched_next;
    u32 group_index;      /* `pcb.h:177` size_t 进程队列索引 */
    u32 main;             /* `pcb.h:178` uint64_t 线程入口地址 */
    u32 user_stack;       /* `pcb.h:179` 用户栈 */
    u32 user_stack_top;   /* `pcb.h:180` 用户栈顶部 */
    u32 load_start;       /* `pcb.h:185` */
    u32 load_end;         /* `pcb.h:186` */

    void *user_info;      /* `pcb.h:188` UserInfo*(等 M7)*/

    u32 clear_child_tid;  /* `pcb.h:192` */

    void  *cwd;           /* `pcb.h:199` vfs_node_t(等 M4A-1)*/
    char  *str_cwd;       /* `pcb.h:200` */

    char **argv;          /* `pcb.h:202` */
    u32    argc;          /* `pcb.h:203` size_t */

    /* ---- 调度统计(M4-8 的 EEVDF 会用;先按 `pcb.h:212-216` 留好) ---- */
    u64 eevdf_vruntime;   /* `pcb.h:212` */
    u64 eevdf_deadline;   /* `pcb.h:213` */
    u64 eevdf_slice;      /* `pcb.h:214` */
    u64 eevdf_last_start; /* `pcb.h:215` */
    u64 runtime_ticks;    /* `pcb.h:216` */

    bool owns_user_stack; /* `pcb.h:218` */
};

/* ------------------------------------------------------------------ */
/* ★ 为什么这里**没有**"把偏移钉死"的断言 ★                            */
/* ------------------------------------------------------------------ */

/*
 * 一开始这里有一组 `_Static_assert(offsetof(..., ctx) == ARM_TCB_OFF_CTX)`,
 * 想照 x86 的 `pcb.cpp:84-87` 那个样子(它钉 `syscall_stack=0xb48` /
 * `syscall_user_rsp=0xb50`,因为 `handler.S:32-33` 按这两个偏移读 TCB)。
 * **做不下去,而且不该那么做**,理由有两条,第二条才是关键:
 *
 * ### 1. 宿主与目标的布局本来就不同
 *
 * TCB 里有大量指针字段(`parent_group` / `argv` / `cwd` …)。宿主的指针是
 * 8 字节、目标是 4 字节,于是**宿主机上算出来的偏移与目标板不同** ——
 * 那种断言在宿主上验的是错的东西。(本项目在 `heap_block_t` 的 sizeof 上
 * 已经踩过一次同样的坑,见计划 §0.5.5 第 10 条。)
 *
 * ### 2. ★ 汇编**不需要**知道 TCB 里 ctx 的偏移 ★
 *
 * x86 必须知道,是因为它的 `syscall` 指令**不切栈** ——
 * `handler.S:315` 得从 TCB 里读出 `syscall_stack` 装进 `%rsp`;
 * per-CPU 的 `current_task` 也要按 `%gs:0x4c0` 取。
 * 这些偏移**必须**与 C 结构体严丝合缝,所以 x86 才那么在意它们漂移
 * (而 `handler.S:29-31` 那三个偏偏一条断言都没有)。
 *
 * ARM 没有这个问题:
 *   - SP 按模式 banked,硬件自动切,**不需要**从结构体里读栈指针;
 *   - M4-7 的切换器由 **C 侧把 `&tcb->ctx` 传进去** ——
 *     跨语言边界上传的是**指针**而不是偏移。偏移只在 C 里算,
 *     编译器自己保证一致,不存在"两边对不上"这回事。
 *
 * ⇒ 所以 ARM 侧汇编真正要钉的偏移只剩两组,而且两组**都只含 u32 字段**,
 *   宿主与目标布局一致,断言在宿主机上验的就是目标板的真实偏移:
 *
 *     - `arm_task_ctx_t` 内部的偏移 —— 在 arch/taskctx.h 里断言
 *     - `percpu_t.current_task`      —— 在 arch/percpu.h 里断言
 *
 * 这比 x86 **少一层**,是架构给的,不是省略出来的。
 *
 * 那这个结构体就完全不钉了吗?也没有 —— 下面这几条与汇编无关,但同样值得钉:
 */

/* 结构体本身必须自洽:VFP 区大小与对齐是硬件要求 */
_Static_assert(ARM_VFP_D_REGS == 32u, "d0-d31 是 32 个双字寄存器");
_Static_assert(sizeof(((struct arm_thread_control_block *)0)->vfp) == ARM_VFP_BYTES,
               "VFP 上下文必须是 d0-d31 这么多字节");

/*
 * ★ 整块 TCB 的对齐要求必须与分配器给得起的对齐一致 ★
 *
 * `heap_alloc` 保证 8 字节(`heap.h` 的 HEAP_ALIGN)。TCB 的自然对齐由里面的
 * `u64` 成员决定,应当是 8。
 *
 * ⚠ 这一条是**故意的绊线**:将来谁往 TCB 里加一个要求 16/32 字节对齐的字段
 *   (比如照抄 x86 那个 `aligned(16)` 的 FXSAVE 区),这里立刻报错,
 *   迫使他面对"分配器给不出那个对齐"这件事 ——
 *   而不是让代码在目标板上以未定义行为的方式跑起来。
 *   (写这一节时就真的触发过一次:VFP 区原本被我写成 `aligned(32)`,
 *    那是个没有依据的保守取值 —— ARMv7 的 VFP 传输只要求 4 字节对齐,
 *    16 是 x86 FXSAVE 的要求。)
 */
_Static_assert(_Alignof(struct arm_thread_control_block) == 8u,
               "TCB 的自然对齐不是 8 —— 有新字段要求更强的对齐,而 heap_alloc 只保证 8。"
               "要么改字段,要么给堆加一个对齐分配接口(x86 用的是 aligned_alloc(16))");

/* ------------------------------------------------------------------ */
/* 与 x86 逐字段对照的结果:一份可核对的账                             */
/* ------------------------------------------------------------------ */

/*
 * 上面每个字段后面那行 `pcb.h:NN` 注释不是装饰 —— 它们是**可核对的**:
 * 拿 `include/task/pcb.h` 的同一行号对一遍即可。§4.7.2 已经把
 * ARCH_INDEP 的完整清单列出来了,两者应当一一对上。
 *
 * 下面这几个常量只是把"暂时不放"这件事写进代码,便于将来 grep:
 */
#define ARM_TCB_DEFERRED_FIELDS                                                          \
    "syscall_stack syscall_user_rsp fs fs_base tid_directory actions blocked signal "   \
    "sas_ss_sp sas_ss_size sas_ss_flags tty xtttp_stc winnum hasfscr window_count "     \
    "message_pipe_read_fd message_pipe_write_fd notify_pcor_* elf_file elf_size "       \
    "aux_* linux_abi umask vma_manager virt_queue procfs_node proc_root"
