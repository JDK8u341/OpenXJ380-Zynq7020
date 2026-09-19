/*
 * 上游 API 在 ARM 上的落地层 —— M4A-1.2b
 *
 * ====================================================================
 * 这个文件为什么是 C++(而移植侧的其它文件都是 C)
 * ====================================================================
 *
 * 它存在的**唯一理由**就是"把上游的函数名接到移植侧的架构原语上",
 * 因此它必然同时站在两个世界里 —— 这正是本项目那条边界规矩
 * (见 docs/ZYNQ7020_INTEGRATION_PLAN.md §7 第 4 条)说的"交界"。
 *
 * 而交界必须是 **C++**:上游的 `include/task/scheduler.h` 声明那些函数时
 * **没有** `extern "C"`,所以上游调它们用的是 C++ 名字修饰;
 * 一份 C 实现给出的是未修饰符号,链不上(实测报的是
 * `undefined reference to 'scheduler_yield()'`)。
 *
 * ====================================================================
 * ★★ 但"站在交界"不等于"把两个世界的头都 include 进来" ★★
 * ====================================================================
 *
 * 这一点是**实测撞出来的**:同时 include `<arch/sched.h>` 与 `<task/pcb.h>`
 * 会当场炸,而且是三类硬冲突:
 *
 *     include/mm/alloc/alloc.h:52  conflicting declaration of C function
 *                                  'bool heap_init(uint8_t*, size_t)'
 *         —— 上游的 `heap_init` 与移植侧 `<arch/heap.h>` 的同名函数
 *            (参数完全不同)撞车
 *     include/task/pcb.h:46-47     conflicting declaration 'typedef ... pcb_t/tcb_t'
 *         —— 两边**各自**定义了 `tcb_t`/`pcb_t`(上游 `struct
 *            thread_control_block` vs 移植侧 `struct arm_thread_control_block`)
 *     include/task/pcb.h:51-53     'CREATE'/'RUNNING'/'WAIT' conflicts
 *         —— 两边的 TaskStatus 枚举值撞车
 *
 * ⇒ 所以本文件的规矩是:
 *
 *   **只 include 上游的头;移植侧要用的原语**手写 `extern "C"` 声明。**
 *
 * 这不是权宜之计,而是这条边界的**正确形状**:交界处本来就该是
 * "两边各自的一份声明",而不是"两边的实现细节都摊开"。
 * 手写的那几个声明由 `tests/test_arm32_upstream_api.py` 对着
 * `arch/arm32/include/arch/` 下的头文件做机械比对 —— 漂移会被抓住。
 *
 * ⚠ 也**只**做"转发/安装",不写业务逻辑:逻辑在 `arch/sched.h`、
 *   `arch/cpu.h`、`arch/timer.h` 里(它们已被板上自检覆盖)。
 */

#include <fs/vfs/vfs.h>
#include <krlibc.h>
#include <lock_queue.h>
#include <mm/page.h>
#include <stdarg.h> /* va_list:下面那两条格式化转发要展开可变参数 */
#include <task/pcb.h>
#include <task/scheduler.h>

/* ====================================================================
 * 移植侧原语的声明(手写,刻意不 include 移植侧的头 —— 理由见文件头)
 * ====================================================================
 *
 * 每一条后面都写着它在移植侧的出处,由契约测试核对签名。
 */
extern "C" {
void     sched_yield(void);        /* arch/sched.h:434 */
void     sched_sleep_ns(uint64_t); /* arch/sched.h:507 */
void     sched_wake_task(void *);  /* arch/sched.h:515 —— 参数类型见下面的说明 */
uint32_t timer_read_ticks_low(void); /* arch/timer.h:24 */
void     console_puts(const char *); /* arch/console.h:18 */
int      console_vprintf(const char *fmt, va_list args);             /* arch/console.h:34 */
int      console_vsprintf(char *buf, const char *fmt, va_list args); /* arch/console.h:37 */
}
/*
 * ⚠ `arch_irq_disable()` / `arch_irq_enable()` **刻意没有**在这里声明:
 *   它们是 `arch/cpu.h` 里的 `static inline`,而架构覆盖层
 *   (`upstream/cpu/lock.h` → `#include <arch/cpu.h>`)已经把那个头带进来了。
 *   再写一份 `extern "C"` 声明会与之冲突(实测:
 *   "conflicting declaration ... with 'C' linkage")—— 因为 inline 函数
 *   根本不产生符号,本来就不需要声明。
 */

/* ---- 调度器:上游名字 → 移植侧实现 ---- */

/*
 * ← `include/task/scheduler.h:10`
 *
 * 移植侧的 `sched_yield()`(`arch/sched.h:434`)语义与上游一致:
 * 让出当前任务,走**同一条切换路径**(在 ARM 上是 `svc #ARM_SVC_YIELD`)。
 */
void scheduler_yield(void)
{
    sched_yield();
}

/*
 * ← `include/task/scheduler.h:11`
 *
 * `uint64_t` 与移植侧的 `u64` 是同一个类型(都是 `unsigned long long`,
 * 32 位 ARM 上 8 字节)。
 */
void scheduler_sleep_ns(uint64_t nano)
{
    sched_sleep_ns(nano);
}

/*
 * ← `include/task/scheduler.h:12`
 *
 * ★★ **这一条做不到"真转发",原因值得记下来** ★★
 *
 * 上游的签名是 `scheduler_wake_task(tcb_t task)`,而上游的 `tcb_t` 是
 * `struct thread_control_block *` —— 移植侧**另有一个同名的** `tcb_t`
 * (`struct arm_thread_control_block *`,`arch/tcb.h`),两者是**不同的类型、
 * 不同的布局**。M4-6 造移植侧 TCB 时是"逐字段照抄上游",但结构体本身
 * 仍是两个(见计划 §4.7 的三份清单)。
 *
 * ⇒ 把上游的 tcb 直接转成移植侧的 tcb 交给 `sched_wake_task()` 是**撒谎**:
 *   今天没有任何对象同时是这两者。真要接通,得先让两边共用同一个 TCB
 *   定义 —— 那是 **M4A-3(B5)对齐驱动接口**或 **M7(进程层)**的事。
 *
 * 所以这里**大声拒绝**:计数 + 打一行(只打一次)。
 * 它今天是**不可达**的 —— 唯一的调用者 `pipefs.cpp` 不在 ARM 的构建图里;
 * 万一将来被走到,是看得见的,而不是"睡下去再也醒不过来"。
 */
static u32 g_wake_task_unsupported;

void scheduler_wake_task(tcb_t task)
{
    (void)task;
    if (g_wake_task_unsupported == 0u) {
        console_puts(" ARM: scheduler_wake_task() unsupported (tcb_t types differ; see upstream_api.cpp)\n");
    }
    g_wake_task_unsupported++;
}

u32 arm_wake_task_unsupported_count(void)
{
    return g_wake_task_unsupported;
}

/* ---- 中断开关:上游名字 → 移植侧实现 ---- */

/*
 * ← `include/proto.hpp:104-105`(上游把这两句放在那个"万能头"里,
 *   而不是某个 cpu/ 头 —— 这里照抄签名,不引用那份头)
 *
 * 上游:`close_interrupt`/`open_interrupt` 是它们的宏别名
 * (`proto.hpp:23-24`);ARM 侧没有那两个宏 —— 用到的话会当场编不过,
 * 那是对的:宏是编译期的,不能靠链接期补。
 */
void disable_intr(void)
{
    arch_irq_disable();
}

void enable_intr(void)
{
    arch_irq_enable();
}

/* ====================================================================
 * 进程层最小切片(2026-09-18,M4A-1.2b)
 * ====================================================================
 *
 * 上游的 VFS 是**围着进程写的**:它要 `get_current_task()->parent_group`
 * 的 fd 表、cwd、tty、pid(见 `driver/fs/vfs/vfs.cpp:1039/1156/1230/1253/1472`)。
 * 而移植侧**还没有进程**(那是 M7 的事,计划 §4.6)。
 *
 * 这里提供的是上游**自己就有**的那个东西:**内核进程**。
 * 上游把内核线程挂在 `kernel_group` 上(`kernel/task/pcb.cpp:2726`
 * 的 `kernel_group = kernel_pcb`),所以"所有内核线程共享一个进程上下文"
 * 不是我们发明的退化,而是上游的既有语义。
 *
 * ⚠ 本切片**只做这么多**(三条都记进 README 的「退化实现清单」):
 *   1. 一个静态内核进程 + 一个静态 TCB 影子,**没有** fork/exec/进程组;
 *   2. **所有** ARM 线程通过 `get_current_task()` 看到的是**同一个**上下文
 *      (上游是每个 tcb 各自 `parent_group`;真做到那一步要等用户态);
 *   3. `cwd` 在 VFS 起来之前是 NULL,由 `arm_kernel_process_set_cwd()` 补。
 */

static struct process_control_block g_kernel_pcb;
static struct thread_control_block  g_kernel_tcb;

/* `extern pcb_t kernel_group;`(`include/task/pcb.h:221`)—— 一个**指针变量** */
pcb_t kernel_group = &g_kernel_pcb;

tcb_t get_current_task(void)
{
    return &g_kernel_tcb;
}

/*
 * 建内核进程上下文。**必须在堆可用之后**调用(`queue_init()` 要 malloc)。
 * 幂等:重复调用先销毁旧 fd 表,不泄漏。
 */
void arm_kernel_process_init(void)
{
    if (g_kernel_pcb.file_open != NULL) {
        queue_destroy(g_kernel_pcb.file_open);
        g_kernel_pcb.file_open = NULL;
    }

    memset(&g_kernel_pcb, 0, sizeof(g_kernel_pcb));
    memset(&g_kernel_tcb, 0, sizeof(g_kernel_tcb));

    g_kernel_pcb.pid  = 0; /* 上游的 kernel_pcb 是 PID 0 */
    g_kernel_pcb.ppid = 0;
    strncpy(g_kernel_pcb.name, "kernel", sizeof(g_kernel_pcb.name) - 1u);

    /* fd 表:上游 `create_process_group` 里也是 `queue_init()` 建的 */
    g_kernel_pcb.file_open = queue_init();

    /*
     * `tty` 留 NULL。这不是偷懒:上游 `vfs.cpp:1253` 自己就写着
     * "`parent_group->tty == NULL` 时返回 `get_default_tty()`" ——
     * 内核进程没有控制终端本来就是上游允许的状态。
     */
    g_kernel_pcb.tty = NULL;

    g_kernel_tcb.parent_group = &g_kernel_pcb;
    g_kernel_tcb.task_level   = 0;
    strncpy(g_kernel_tcb.name, "kmain", sizeof(g_kernel_tcb.name) - 1u);
    g_kernel_tcb.cwd          = NULL; /* 由 set_cwd 在 VFS 起来之后补 */
}

/*
 * VFS 起来之后把根目录填进 cwd —— 上游 `vfs.cpp:1230` 用
 * `get_current_task()->cwd` 拼绝对路径,根目录下它必须是合法节点。
 */
void arm_kernel_process_set_cwd(vfs_node_t root)
{
    g_kernel_tcb.cwd = root;
}

/* ---- 页层:ARM 上没有的两件事,明确拒绝而不是假装 --- */

/*
 * ← `include/mm/page.h:203`(`page_directory_t *get_current_directory();`)
 *
 * **返回 NULL 是这里唯一诚实的答案**:它是 x86 的页目录(CR3 那一套),
 * 而 ARMv7 的地址空间是 TTBR0/TTBR1 + 短描述符,移植侧的页表模型
 * (`arch/arm32/src/vmap.c`)与 `page_directory_t` **不是一回事**。
 *
 * 编一个假的 `page_directory_t` 只会把失败推到一个更远、更难查的地方。
 * 今天唯一的调用点是 `general_map()`,它在 ARM 上本来就不该被走到。
 */
page_directory_t *get_current_directory(void)
{
    return NULL;
}

/*
 * ← `include/mm/page.h:128`
 *
 * 语义是"把一段范围映射到**当前进程地址空间里的一个随机虚拟地址**"
 * (ASLR 式的 `general_map`)—— 它预设了**用户地址空间**,移植侧没有。
 *
 * ★ 所以**大声拒绝**,而不是静默返回:静默返回会让紧随其后的
 *   `read_callback(file, (void *)addr, ...)`(`vfs.cpp:1051`)往一个
 *   **没有映射过的地址**写 —— 那是 Data Abort,而现场离真正的原因隔着
 *   好几层。拒绝计数可以读出来。
 *
 * 它今天**不可达**:唯一调用点 `general_map()` 是模块/ELF 加载路径,
 * 移植侧两样都没有(计划 §8 明确列出)。
 */
static u32 g_page_map_unsupported;

void page_map_range_to_random(page_directory_t *directory, uint64_t addr, uint64_t length,
                              uint64_t flags)
{
    (void)directory;
    (void)addr;
    (void)length;
    (void)flags;
    if (g_page_map_unsupported == 0u) {
        console_puts(" ARM: page_map_range_to_random() unsupported (no user address space)\n");
    }
    g_page_map_unsupported++;
}

u32 arm_page_map_unsupported_count(void)
{
    return g_page_map_unsupported;
}

/* ---- 三个"缺的定义" ---- */

/*
 * ← `include/krlibc.h:145`,定义在上游 `kernel/krlibc.cpp:511`。
 *
 * 移植侧的 krlibc 子集**没有它**(M4-1 那批是按 driver/fs 的调用点选的,
 * 而 pathacat 定义在 kernel/ 里)。VFS 用它拼路径(`vfs.cpp:1163/1231`),
 * 所以照抄一份语义 —— 由契约测试对着上游那份做机械比对。
 */
char *pathacat(char *p1, char *p2)
{
    size_t len1;
    size_t len2;
    char  *result;

    if (p1 == NULL) {
        return NULL;
    }
    if (p2 == NULL) {
        return p1;
    }

    len1 = strlen(p1);
    len2 = strlen(p2);

    result = (char *)malloc(len1 + len2 + 2u);
    if (result == NULL) {
        return NULL;
    }

    memcpy(result, p1, len1);
    /* 中间补一个 '/' —— 与上游一致(上游 `krlibc.cpp:511` 也是这么拼的) */
    if (len1 > 0u && p1[len1 - 1u] != '/') {
        result[len1] = '/';
        len1++;
    }
    memcpy(result + len1, p2, len2);
    result[len1 + len2] = '\0';

    return result;
}

/*
 * ← `include/ps2/keyboard.h:99`,上游实现在 `driver/ps2/keyboard.cpp:357`
 *   (PS/2 控制器 —— x86 专属硬件)。
 *
 * 移植侧**没有键盘**:本板只有 PS UART 串口,输入走串口命令通道
 * (`arch/arm32/src/shell.c`),不是 PS/2。
 *
 * ⇒ 返回 0 是"没有输入"的**正确**答案,不是空壳:
 *   调用点 `vfs.cpp:1368` 拿它当"有没有按键"用,0 表示没有。
 *   真要在 ARM 上支持控制台输入,那是把 shell 的字符源接进 tty ——
 *   属 M4A-1.5(`dev`/`pty`)或之后。
 */
uint8_t get_keyboard_input(void)
{
    return 0;
}

/*
 * ← `driver/fs/vfs/vfs.cpp:47` 自己 `extern` 的那一句,上游实现在
 *   `kernel/syscall/xapi/xtui.cpp:41`(XAPI 的图形/文本输出)。
 *
 * 移植侧的输出通道就是 PS UART 串口,所以这里**转发到 console** ——
 * 这是真实现:`vfs.cpp:1278/1286` 用它打诊断,在 ARM 上就该出现在串口日志里。
 */
void p_xapi_output_kernel(const char *str)
{
    if (str != NULL) {
        console_puts(str);
    }
}

/*
 * ← 上游 `kernel/rng.cpp:125`(用 RDRAND/RDSEED —— x86 指令)。
 *
 * 移植侧没有硬件随机源,但**有**一个 333MHz 的全局定时器:
 * 每次调用取它的低 32 位掺进一个 xorshift 状态。
 *
 * ⚠ 这**不是密码学安全的** —— 但对本阶段唯一的用途
 *   (`vfs.cpp:1553` 给 `/dev/urandom` 填字节)够用,而且它是
 *   **真随机源驱动的**,不是返回常量。
 */
static u32 g_rng_state;

void get_random_bytes(void *buffer, size_t length)
{
    u8    *out = (u8 *)buffer;
    size_t i;

    if (out == NULL) {
        return;
    }

    if (g_rng_state == 0u) {
        g_rng_state = timer_read_ticks_low() ^ 0x9E3779B9u;
    }

    for (i = 0; i < length; i++) {
        g_rng_state ^= g_rng_state << 13;
        g_rng_state ^= g_rng_state >> 17;
        g_rng_state ^= g_rng_state << 5;
        out[i] = (u8)(g_rng_state ^ timer_read_ticks_low());
    }
}

/* ====================================================================
 * ★★ 格式化器:上游名字(名字修饰过的符号)★★
 * ====================================================================
 *
 * ← 声明在 `include/proto.hpp:38` 与 `:42`,上游实现在
 *   `driver/serial/serial_port.cpp:529/708`(那是 x86 串口驱动,ARM 编不了)。
 *   上游真的会调它们:`driver/fs/vfs/vfs.cpp:651`(sprintf 拼路径)与
 *   `:1246/1277/1285/1382`(write_serial_fmt 打诊断)。
 *
 * ## 为什么这两个定义在 C++ 这一侧(而不是和别的项目一样写在 console.c)
 *
 * 因为**符号名字不一样**。`proto.hpp` 把这两条声明写在 `extern "C"` 块
 * **之外**(proto.hpp 里那三个 `extern "C"` 都不覆盖它们),于是上游调用点
 * 要的是经过 C++ 名字修饰的符号 —— 这一点只能从目标文件看出来:
 *
 *     $ arm-none-eabi-nm out/arm32/upstream/driver/fs/vfs/vfs.o
 *     U _Z16write_serial_fmtPKcz
 *     U _Z7sprintfPcPKcz
 *
 * 一开始移植侧确实是在 console.c 里定义了 C 链接的 `sprintf`/
 * `write_serial_fmt`(未修饰符号),链接器照样报
 * `undefined reference to 'sprintf(char*, char const*, ...)'` ——
 * 源码上两边的名字一模一样,**只有 nm 能看出不是同一个符号**。
 *
 * ⇒ 结论:符号必须由 C++ 这一侧给出;**实现仍然只有一份**,
 *   在 console.c(有宿主单测),这里只做 va_list 转发。
 *
 * ## 返回值与源 OS 一致
 *
 *   `sprintf` 返回**已写入的字符数**(不含结尾 NUL;上游返回
 *   `vwprintf` 的 size_t 结果);`write_serial_fmt` 返回 **0**
 *   (上游自己也不返回字符数,大量调用点当过程用)。
 */

int sprintf(char *buf, const char *fmt, ...)
{
    va_list args;
    int     written;

    va_start(args, fmt);
    written = console_vsprintf(buf, fmt, args);
    va_end(args);

    return written;
}

int write_serial_fmt(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    (void)console_vprintf(fmt, args);
    va_end(args);

    /* 与源 OS 一致:恒返回 0 */
    return 0;
}

/* ====================================================================
 * ★★ VFS 起搏 + 验收转发(M4A-1.2b 的验收判据)★★
 * ====================================================================
 *
 * ## 为什么需要这一组,而不是让移植侧 C 直接调 VFS
 *
 * 因为 `include/fs/vfs/vfs.h` **整个文件没有 `extern "C"`** ——
 * VFS 的每一个公开函数都是 **C++ 链接**的:
 *
 *     $ arm-none-eabi-nm out/kernel-arm.elf | grep -E 'vfs_init|vfs_mount'
 *     00115c94 T _Z8vfs_initv
 *     00113d64 T _Z9vfs_mountPKcP8vfs_node
 *
 * ⇒ 移植侧 C 给不出这些符号名(同一个坑 M4A-1.2b 收尾时在
 *   `sprintf` 上刚踩过,见坑表 55)。所以"调用 VFS"这件事必须发生在
 *   C++ 这一侧。
 *
 * ## 为什么这一组里**没有业务逻辑**
 *
 * 按本文件的规矩(见文件头):落地层只做**转发与安装**。
 * 于是这里的分工是:
 *   - **安装**:`arm_vfs_bringup()` —— 建内核进程上下文 → `vfs_init()`
 *     → `tmpfs_setup()` → 把根目录填进 cwd。顺序照源 OS
 *     `kernel/main.cpp:457` 与 `:558`(vfs_init 先、tmpfs 后)。
 *   - **转发**:下面每一个 `arm_vfs_*` 都是"展开一次上游调用、把它的
 *     返回值原样交回去",**不做判断**。
 *   - **判据**(什么算通过、阈值多少)全在移植侧 C:
 *     `arch/arm32/src/vfs_check.c` —— 那里才是"验收程序"。
 *
 * ⚠ `tmpfs_setup()` 在上游没有进任何头文件,是 `kernel/main.cpp:424`
 *   自己 `extern int tmpfs_setup();` 声明的(与 `mount_root()` 一样)。
 *   这里照做 —— **不改上游**,连声明方式都跟着上游。
 */

/* 上游没有导出声明,照 `kernel/main.cpp:424` 的做法自己声明 */
int tmpfs_setup(void);

extern "C" int arm_vfs_bringup(void)
{
    /*
     * ① 内核进程上下文。**必须在堆之后**(`queue_init()` 要 malloc)。
     *    上游对应物:`create_process_group()`(`kernel/task/pcb.cpp`),
     *    ARM 侧只是"一个静态实例",理由见上面进程层那一节。
     */
    arm_kernel_process_init();

    /* ② `vfs_init()` —— 上游 `kernel/main.cpp:457`。返回 bool(false = 失败) */
    if (!vfs_init()) {
        return -1;
    }

    /* ③ `tmpfs_setup()` —— 上游 `kernel/main.cpp:558`。
     *    内部 = `vfs_regist("tmpfs", …)` + `vfs_mkdir("/tmp")` +
     *    `vfs_open("/tmp")` + `vfs_mount(TMPFS_REGISTER_ID, tmp)`,
     *    成功返回 0、失败返回 -EIO/-ENOENT */
    if (tmpfs_setup() != 0) {
        return -2;
    }

    /* ④ 根目录填进 cwd —— 上游 `vfs.cpp:1230` 用
     *    `get_current_task()->cwd` 拼绝对路径,根目录下它必须是合法节点 */
    arm_kernel_process_set_cwd(get_rootdir());

    return 0;
}

extern "C" int arm_vfs_create(const char *path)
{
    /* `errno_t vfs_mkfile(const char *)`(`vfs.h:246`),0 = 成功 */
    return (int)vfs_mkfile(path);
}

extern "C" int arm_vfs_write(const char *path, const void *data, unsigned int len)
{
    vfs_node_t node = vfs_open(path);

    if (node == NULL) {
        return -1;
    }
    /* `size_t vfs_write(node, void *, offset, size)`(`vfs.h:359`)
       —— 上游形参不是 const,这里显式去掉 const,不改上游签名 */
    return (int)vfs_write(node, (void *)data, 0u, (size_t)len);
}

extern "C" int arm_vfs_read(const char *path, void *buf, unsigned int len)
{
    vfs_node_t node = vfs_open(path);

    if (node == NULL) {
        return -1;
    }
    return (int)vfs_read(node, buf, 0u, (size_t)len);
}

/*
 * 目录项计数。**"列目录"在本 VFS 里就是这个意思** ——
 * 上游没有 `readdir`:目录的子项就是 `vfs_node_t::child`(`vfs.h:200`,
 * 类型 `list_t`),由路径解析/创建路径上的 `vfs_child_append()` 填。
 * ⇒ 数 `list_length(dir->child)`。
 */
extern "C" int arm_vfs_child_count(const char *path)
{
    vfs_node_t dir = vfs_open(path);

    if (dir == NULL || dir->type != file_dir) {
        return -1;
    }
    return (int)list_length(dir->child);
}

/*
 * 节点所属**文件系统实例**的 id(`vfs_node_t::fsid`)。
 *
 * 为什么它是"挂载真的发生了"的判据:tmpfs 的 `tmpfs_mount()` 里
 * 写着 `node->fsid = tmpfs_id;`(`tmpfs.cpp:111`),而 `tmpfs_id` 来自
 * `vfs_regist()` 的返回值(`fs_nextid++`)。挂载没发生的话,`/tmp`
 * 仍是根文件系统的一个普通目录 ⇒ 它的 fsid 与 `/` **相同**。
 */
extern "C" int arm_vfs_fsid(const char *path)
{
    vfs_node_t node = vfs_open(path);

    if (node == NULL) {
        return -1;
    }
    return (int)node->fsid;
}

/* 内核进程的 cwd 是不是根目录(判 `arm_kernel_process_set_cwd()` 生效)*/
extern "C" int arm_vfs_cwd_is_root(void)
{
    tcb_t task = get_current_task();

    return (task != NULL && task->cwd != NULL && task->cwd == get_rootdir()) ? 1 : 0;
}
