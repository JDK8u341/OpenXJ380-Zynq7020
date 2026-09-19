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

#include <fs/fatfs/fatfs.h> /* fatfs_init / FATFS 的格式化与挂载(M4A-1.4) */
#include <fs/fatfs/ff.h>     /* f_mkfs / MKFS_PARM / FM_FAT / FF_MAX_SS */
#include <fs/vfs/devfs.h>    /* devfs_setup()(M4A-1.5:改用上游那份 devfs)*/
#include <fs/vfs/vfs.h>
#include <krlibc.h>
#include <lock_queue.h>
#include <mm/page.h>
#include <mutex.h> /* mutex_t:落地层要给出上游那三个 C++ 链接的互斥函数(M4A-1.4) */
#include <pipe.h>  /* pipe_info_t / pipe_specific_t / PIPE_BUFF(M4A-1.5 的管道)*/
#include <rtc.h>   /* tm / mktime / realtime_ns(纯算术部分照搬上游,见下) */
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
uint64_t timer_read_ns(void);        /* arch/timer.h:27 —— FATFS 的时间戳用它(M4A-1.4) */
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

/* ====================================================================
 * ★★ FATFS 需要的 8 个符号(M4A-1.4)★★
 * ====================================================================
 *
 * 把 `driver/fs/fatfs/` 那 5 个文件(含 2MB 的 `ffunicode.cpp`)编进 ARM 后,
 * 链接缺口**正好 8 个**,全部来自两个上游文件:
 *
 *     driver/fs/fatfs/fatfs.cpp   mutex_create / mutex_lock / mutex_unlock
 *                                 mktime / realtime_ns / ahci_is_qemu_environment
 *     driver/fs/fatfs/diskio.cpp  ahci_is_qemu_environment
 *                                 alloc_frames / phys_to_virt
 *
 * ## ⚠ 这 8 个**不能**写成 `extern "C"`
 *
 * 实测目标文件里的名字:
 *
 *     $ arm-none-eabi-nm out/arm32/upstream/driver/fs/fatfs/fatfs.o | grep ' U '
 *     U _Z12mutex_createP5mutexb      U _Z6mktimeP2tm
 *     U _Z10mutex_lockP5mutex         U _Z11realtime_nsv
 *     U _Z12mutex_unlockP5mutex       U _Z24ahci_is_qemu_environmentv
 *     $ ... diskio.o
 *     U _Z12alloc_framesj             U _Z12phys_to_virty
 *
 * 全是**修饰名**(上游这些声明都不在 `extern "C"` 块里)。
 * ⇒ 写成 `extern "C"` 就是坑表 55 那个错误的重演:C 侧给出未修饰符号,
 *   而调用点要的是修饰过的,**源码上两个名字一模一样**。
 * ⇒ 所以下面这 8 个定义**故意不带** `extern "C"`,而且它们产生的修饰名
 *   由 `tests/test_arm32_fatfs.py` 对着上游目标文件的符号表钉住。
 */

/* ---- ① 互斥锁:转发到移植侧那把已验证的 yield 型互斥 ---- */

/*
 * ⚠ 上游 `mutex_t` 与移植侧 `mutex_t` **布局不同**,不能互相 reinterpret:
 *
 *     上游: { spin_t lock; state; owner; lock_queue *wait_queue; size_t rcc; rec; }
 *     移植: {               state; owner;                       u32 rcc;    rec; }
 *
 * 移植侧**刻意**省掉了 `spin_t lock`(锁由 `enter_state`/`leave_state`
 * 钩子负责)与 `wait_queue`(源 OS 的实现里它只在 create/destroy 出现过,
 * 是化石 —— 见 arch/mutex.h)。
 *
 * ⇒ 做法:把上游那个对象当成**一块不透明存储**交给移植侧那把 mutex 用
 *   (移植侧只占前 16 字节,而**上游的对象更大**)。这样:
 *     - 不复制第二套互斥逻辑 —— 语义只有一份,就是 M4-11.1 板上验过的;
 *     - 不动移植侧那个已验证的结构 —— 它的证据链保持完整;
 *     - 状态存在**上游对象自己**里,不需要旁表,也不需要额外分配。
 *
 * ⚠ 前提有两条,各有一条静态断言或契约测试钉住:
 *     1. 移植侧 `mutex_t` 只占 16 字节(`src/mutex.c` 的 `_Static_assert`);
 *     2. ARM 图里**没有**上游的 mutex 实现(`kernel/task/mutex.cpp` 不在
 *        构建里)⇒ 那 16 字节在 ARM 上只有这一条读写路径。
 *        下面那条 `static_assert` 是第 2 条的另一半:上游对象必须**够大**。
 */
extern "C" {
void arm_mutex_create(void *m, bool recursive);
int  arm_mutex_lock(void *m);
int  arm_mutex_unlock(void *m);
}

static_assert(sizeof(mutex_t) >= 16u,
              "上游 mutex_t 必须至少容得下移植侧那份 mutex 的 16 字节存储");

void mutex_create(mutex_t *mtx, bool recursive)
{
    arm_mutex_create(mtx, recursive);
}

int mutex_lock(mutex_t *mtx)
{
    return arm_mutex_lock(mtx);
}

int mutex_unlock(mutex_t *mtx)
{
    return arm_mutex_unlock(mtx);
}

/* ---- ② 时间:文件时间戳 ---- */

/*
 * `mktime(tm *)` —— **逐行照搬** `driver/rtc.cpp:100-127`(连同它依赖的
 * `is_leap_year` 与 `days_in_month`)。
 *
 * 为什么不直接编 `driver/rtc.cpp`:那个文件 = CMOS 读写(0x70/0x71 端口)
 * + 纯算术。Zynq 上**没有 CMOS 这个器件**,整份搬过来只能靠假端口访问
 * 顶上去 —— 那正是本项目禁止的做法(见 `get_current_directory()` 那条)。
 * 而 `mktime` 是**纯算术**,照搬它没有任何硬件假设。
 *
 * ⚠ 上游这个 `mktime` **不是** ISO C 的语义,照搬必须连这两点一起搬:
 *     1. `tm_year` 是**完整年份**(不是"1900 起的年数");
 *     2. `tm_mon` 是 **1..12**(不是 ISO 的 0..11)。
 *   `driver/fs/fatfs/fatfs.cpp:293-312` 正是按这两点构造 `tm` 的
 *   (`year = 1980 + …`、`month = 1..12`)⇒ 移植侧**必须**保持同一语义,
 *   否则文件时间会**整体错位**而且不报错。
 *   `tests/test_arm32_fatfs.py` 把这里和上游那份**逐日期比对**。
 */
static const int g_days_in_month[2][12] = {
    {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31},
    {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31},
};

static bool arm_is_leap_year(int year)
{
    return (year % 4 == 0) && ((year % 100 != 0) || (year % 400 == 0));
}

int64_t mktime(tm *time)
{
    int64_t seconds = 0;
    int     leap;

    int year  = time->tm_year;
    int month = time->tm_mon - 1;
    month -= (month > 11) ? 11 : 0;
    int day = time->tm_mday - 1;

    for (int y = 1970; y < year; y++) {
        leap = arm_is_leap_year(y);
        seconds += (365 + leap) * 86400;
    }

    leap = arm_is_leap_year(year);
    for (int m = 0; m < month; m++) {
        seconds += g_days_in_month[leap][m] * 86400;
    }

    seconds += day * 86400;
    seconds += time->tm_hour * 3600;
    seconds += time->tm_min * 60;
    seconds += time->tm_sec;

    return seconds;
}

/*
 * `realtime_ns()` —— 墙钟。**ARM 侧今天没有墙钟**:
 * 上游实现在 `driver/rtc.cpp:180`,读 PC 的 CMOS。Zynq 上没有那个器件
 * ⇒ 那份实现不是"还没搬",而是"搬过来也没有硬件"。
 *
 * ⇒ 返回**开机以来的纳秒数**(移植侧的全局定时器),记进 README 的退化清单
 *   (D24):文件时间戳是"启动后的相对时间",不是真实日期。
 *   替换条件:出现墙钟源(Zynq PS RTC 驱动,或由控制台设一次时间)。
 *
 * ⚠ 它**不是常量**,也不是 0:FATFS 会把它当"挂载时刻"和"mtime"用,
 *   单调递增才让"后写的文件更新"这类判断成立。
 */
uint64_t realtime_ns()
{
    return timer_read_ns();
}

/* ---- ③ x86 专属的两个符号:响亮拒绝 + 计数器 ---- */

/*
 * 先看这三个为什么绑在一起:
 *
 *   `diskio.cpp` 的预读优化(**QEMU/AHCI 专用**)流程是
 *       fatfs_readahead_allowed()  ← 就是 ahci_is_qemu_environment()
 *         → fatfs_cache_ensure()  → alloc_frames() + phys_to_virt()
 *
 * `ahci_is_qemu_environment()` 在 Zynq 上如实返回 **false** —— 这不是
 * 假实现,是**正确回答**("这台机器上没有 AHCI")。后果是整条预读路径
 * 在 ARM 上**不可达**,而 `alloc_frames` / `phys_to_virt` 只被那条路径调用。
 *
 * ⇒ 那两个函数按本项目的规矩做**响亮拒绝**,返回值取调用方**已经处理**的
 *   失败值(于是即使真被调用也不会把错误推到更远的地方):
 *        alloc_frames → 0      (`if (phys == 0) return false;`)
 *        phys_to_virt → NULL   (`if (cache->data == NULL) return false;`)
 *
 * ★ 而"不可达"这件事是**可判的**,不是嘴上说的:
 *   两个函数各自计数,计数必须**恒为 0** —— 板上自检读它
 *   (`arm_fatfs_x86_path_calls`)。这就是"预读确实没跑"的证据。
 */
static unsigned int g_fatfs_x86_path_calls;

bool ahci_is_qemu_environment()
{
    /* Zynq 上没有 AHCI 控制器 ⇒ 不是"QEMU 的 AHCI 环境"。如实回答。 */
    return false;
}

uint64_t alloc_frames(size_t count)
{
    (void)count;
    g_fatfs_x86_path_calls++;
    /* 0 = 分配失败(调用方按失败处理)。ARM 侧的页分配器是 palloc,
       它不是 x86 那个"帧"模型 —— 见 README 的退化清单 D23。 */
    return 0;
}

void *phys_to_virt(uint64_t phys_addr)
{
    (void)phys_addr;
    g_fatfs_x86_path_calls++;
    /* ARM 没有 x86 那个 HHDM 直映射窗口。返回 NULL 让调用方按失败处理。 */
    return NULL;
}

extern "C" unsigned int arm_fatfs_x86_path_calls(void)
{
    return g_fatfs_x86_path_calls;
}

/* ====================================================================
 * ★★ FATFS 起搏 + 验收转发(M4A-1.4)★★
 * ====================================================================
 *
 * ## 为什么 RAM 盘是一个 **tmpfs 文件**
 *
 * 计划 §4.6 的**决策 3**:"M4A-1.4 先接 RAM 盘 …… RAM 盘与真实块设备走
 * **完全相同**的 `diskio` 路径"。查过 `diskio.cpp` 之后,这条路比预想的更直:
 *
 *     disk_read(pdrv, buff, sector, count)
 *        → fatfs_get_node_by_number(pdrv)      // 一个 **vfs_node_t**
 *        → vfs_read(node, buff, sector*512, count*512)
 *
 * **这个 OS 的块层就是 VFS**:没有独立的 `block_device_t`,`disk_*` 全部
 * 落成"按字节偏移读写一个 VFS 节点"。于是"RAM 盘"= 一个能按偏移读写的
 * 节点,而 tmpfs 文件**本来就是**那个东西(M4A-1.2b 已在板上验过
 * 建/读/写/列目录,以及 32 字节往返)。⇒ **零新增块驱动代码**,
 * 而且走的正是决策 3 说的那条同路径。
 *
 * ## 为什么格式化用 `FM_FAT`(FAT12/16 族)、而不是上游那个 `fatfs_format_node()`
 *
 * 上游的格式化辅助函数把 `opt.fmt` **写死成 `FM_FAT32 | FM_SFD`**
 * (`driver/fs/fatfs/fatfs.cpp:57`)。FAT32 按规范要求卷内至少 ~65525 个簇
 * (512 字节簇 ⇒ **≥ 34MB**),而这个 RAM 盘要装在内核堆里,堆是
 * **正好 32MB**(`src/kmain.c` 的 `HEAP_PAGES = 8192`)⇒ **装不下**。
 *
 * ⇒ 不往上堆内存(那是拿"改大内存"掩盖设计),改用 `FM_FAT | FM_SFD`
 *   调**同一个上游 API** `f_mkfs()`。差别只在**格式化参数**:
 *   同一份 FATFS 代码、同一个 `diskio`、同一套挂载机制。
 *
 * ⚠ **子类型(FAT12 还是 FAT16)不由我们指定** —— `FM_FAT` 只说是这一族,
 *   由 FatFs 按算出来的簇数自己选(`ff.cpp:6718` 附近那段重试逻辑)。
 *   8MB 的卷上**实测选到 FAT12**,不是 FAT16:
 *       8 扇区/簇(4KB)⇒ 2043 簇 ⇒ FAT 占 **7 扇区**
 *       (2043 簇的 FAT12 需要 ceil(2043*1.5/512) = 6 扇区;FAT16 需要 8 扇区)
 *   这条是**量出来的,不是推出来的**:板子把引导扇区原样打出来,
 *   宿主机独立解析后得到的结论(`tmp-test/verify_fat_bootsector.py`)。
 *   我原先在移植侧写死"必须是 FAT16",于是对一个**正确**的卷报了假 FAIL
 *   —— 记在坑表里,免得下次又在"我以为什么"上写判据。
 *
 * ## 为什么不用上游的 `fatfs_format_node(node)`
 *
 * 除了上面那条 FAT32 的硬限制,还有一条更本质的:它内部走
 * `alloc_number()` —— 那是 `fatfs.cpp` 里的 **`static`** 函数,落地层拿不到;
 * 而 `drive_number_mapping` 是**非 static** 的全局数组(`fatfs.cpp:20`)。
 * ⇒ 落地层自己挑一个空槽位填节点,再调 `f_mkfs`(与上游辅助函数
 *   同一套三步:填映射 → `f_mkfs` → `f_unmount` → 清映射)。
 */

/* `driver/fs/fatfs/fatfs.cpp:20`(非 static,上游自己在 diskio.cpp 里也 extern 它)*/
extern vfs_node_t drive_number_mapping[10];

/* 卷最大 10 个(上游就是 10)*/
#define ARM_FATFS_DRIVES 10

static int arm_fatfs_free_drive(void)
{
    for (int i = 0; i < ARM_FATFS_DRIVES; i++) {
        if (drive_number_mapping[i] == NULL) {
            return i;
        }
    }
    return -1;
}

extern "C" int arm_fatfs_init(void)
{
    /* ← `driver/fs/fatfs/fatfs.cpp:831`:`mutex_create()` + `vfs_regist("fatfs", …)`。
       返回 void,所以"注册失败"只能从它自己打的那行日志看出来;这里返回 0。 */
    fatfs_init();
    return 0;
}

/*
 * 建 RAM 盘文件并把大小撑开。
 *
 * ⚠ `vfs_resize()` **吞掉** resize 回调的失败(回调是 `void`),所以
 *   "设成功了没有"**不能靠它的返回值** —— 这里重新打开一次、把
 *   `node->size` **读回来**交出去。判据看的是读回来的值。
 *   `disk_size()` 用的正是 `node->size / 512`(`diskio.cpp:186`),
 *   所以这一步不成立,后面 `f_mkfs` 会因为"卷里 0 个扇区"而失败。
 */
extern "C" int arm_fatfs_make_ramdisk(const char *path, unsigned int bytes)
{
    vfs_node_t node;
    int        rc;

    rc = (int)vfs_mkfile(path);
    if (rc != 0) {
        return -1;
    }

    node = vfs_open(path);
    if (node == NULL) {
        return -2;
    }

    (void)vfs_resize(node, (uint64_t)bytes);

    node = vfs_open(path);
    if (node == NULL) {
        return -3;
    }
    return (node->size == (size_t)bytes) ? 0 : -4;
}

extern "C" unsigned long arm_fatfs_node_size(const char *path)
{
    vfs_node_t node = vfs_open(path);

    return (node == NULL) ? 0UL : (unsigned long)node->size;
}

/*
 * 格式化(FAT16)。三步与上游 `fatfs_format_node()` 一致:
 * 占一个 drive 槽 → `f_mkfs("N:", …)` → `f_unmount("N:")`。
 */
extern "C" int arm_fatfs_format(const char *path)
{
    char      drive_path[4];
    void     *work;
    MKFS_PARM opt;
    FRESULT   res;
    int       drive;

    vfs_node_t node = vfs_open(path);

    if (node == NULL) {
        return -1;
    }

    drive = arm_fatfs_free_drive();
    if (drive < 0) {
        return -2;
    }
    drive_number_mapping[drive] = node;

    drive_path[0] = (char)('0' + drive);
    drive_path[1] = ':';
    drive_path[2] = '\0';
    drive_path[3] = '\0';

    memset(&opt, 0, sizeof(opt));
    opt.fmt   = FM_FAT | FM_SFD; /* FAT12/16 族,无分区表(超级软盘布局)。
                                    子类型由 FatFs 按簇数自选 —— 8MB 上实测 FAT12 */
    opt.n_fat = 1;

    work = malloc((size_t)FF_MAX_SS);
    if (work == NULL) {
        drive_number_mapping[drive] = NULL;
        return -3;
    }

    res = f_mkfs(drive_path, &opt, work, (uint)FF_MAX_SS);
    free(work);
    f_unmount(drive_path);

    drive_number_mapping[drive] = NULL;

    return (res == FR_OK) ? 0 : (int)res;
}

/*
 * 挂载:`vfs_mount(src, 挂载点目录)`。
 *
 * `vfs_mount()` 的实现是"**挨个试每个已注册文件系统的 mount 回调**"
 * (`vfs.cpp:246` 的 `for (i = 1; i < fs_nextid; i++)`),所以这里不需要
 * 指名道姓说"用 fatfs",把路径交出去即可 —— 这也是源 OS `mount_root()`
 * 的用法。`src` 是**路径**(RAM 盘文件),于是 `fatfs_mount()` 里
 * `is_virtual_fs(src)` 为假、走"把路径 `vfs_open()` 成卷"那条分支。
 */
extern "C" int arm_fatfs_mount(const char *src, const char *mnt)
{
    vfs_node_t node;

    /* 目录可能已经存在(重复调用),所以**不看返回值**,看能不能打开 */
    (void)vfs_mkdir(mnt);

    node = vfs_open(mnt);
    if (node == NULL) {
        return -1;
    }
    if (node->type != file_dir) {
        return -2;
    }
    return ((int)vfs_mount(src, node) == 0) ? 0 : -3;
}

/*
 * 把 RAM 盘的**原始扇区**读出来(按字节偏移 0 读 len 字节)。
 *
 * 这是"**与主机侧比对**"那条验收判据的入口(计划 §4.6 M4A-1.4 的验收写法):
 * 板子把卷的引导扇区原样打出来,宿主机用**自己的** FAT 解析去核
 * (0x55AA 结束标志、BPB 里的 bytes/sector、total sectors 是否等于我们
 * 设的 8MB、FAT16 的类型串)。板子自说自话不算数 —— 那正是本项目
 * "证据分层"的第三条:**让另一个系统去读同一个产物**。
 */
extern "C" int arm_fatfs_read_raw(const char *path, void *buf, unsigned int len)
{
    vfs_node_t node = vfs_open(path);

    if (node == NULL) {
        return -1;
    }
    return (int)vfs_read(node, buf, 0u, (size_t)len);
}

/* ====================================================================
 * ★★ devfs 与块层(M4A-1.5)★★
 * ====================================================================
 *
 * ## 移植侧那份 devfs **骨架退场**,改用上游那份
 *
 * M4A-1.1b 时移植侧写了一个"只登记不建节点"的 devfs 骨架(记在 README 的
 * 退化清单里,计划 §4.4 第 4 条也点名过它是 B5-b 退化形态)。
 * M4A-1.5 把它换掉,**但不是"顺手换"** —— 是链接期强制的:
 * 骨架与上游 `dev.cpp` **都定义** `devfs_register` / `devfs_delete`
 * (C 链接、同名),而本项目的内核链接带 `-Wl,-z,muldefs`
 * (源 OS 自己就带,见 `tools/gen_ninja.py` 里那段说明)⇒
 * **ld 会静默挑一个**,谁生效取决于命令行顺序。
 * 那种"看起来能跑"的重复定义正是本项目最忌讳的东西,所以必须显式二选一。
 * 选上游(源 OS 优先)的额外好处:骨架的退化项就此消掉,`/dev` 下是真的
 * VFS 节点,`by-name` 从"一张私表"变成"`vfs_open("/dev/xxx")`"。
 *
 * ## 三个符号的形状:又是"同名不同符号"
 *
 * `driver/fs/vfs/dev.cpp` 是 C++ 翻译单元,它要的是**修饰名**:
 *
 *     $ arm-none-eabi-nm out/arm32/upstream/driver/fs/vfs/dev.o | grep ' U '
 *     U _Z9disk_sizei                    ← 上游 device.h:59 的 disk_size(int)
 *     U _Z15blk_device_readiPvjj
 *     U _Z16blk_device_write8_device_tPKvjj
 *     U _Z18write_serial_stringPKc
 *
 * ⚠ `disk_size` 这条尤其值得记:移植侧的 `src/device.c` **已经有**一份
 *   C 链接的 `disk_size(int)`(kmain 的设备自检就在用它),
 *   但那是**未修饰**符号,与 dev.cpp 要的 `_Z9disk_sizei` 不是同一个
 *   (与 `sprintf` 那次完全同型,坑表 55)。
 *
 * ⚠ 那为什么不去把上游 `include/device.h:59` 的声明改成 `extern "C"`?
 *   因为上游 x86 侧**同时**有两个 `disk_size`:
 *     `driver/device.cpp:216`  `size_t disk_size(int)`
 *     `driver/fs/fatfs/diskio.cpp:181` `u32 disk_size(byte)`
 *   给前者加 C 链接就会与后者**在 C 里撞名**(C 没有重载)⇒
 *   **直接弄坏 x86 构建**。⇒ 由落地层给出那个 C++ 名、转发到移植侧实现,
 *   实现仍然只有一份(`arm_disk_size`,与 `arm_mutex_*` 同型)。
 */

extern "C" size_t arm_disk_size(int drive);

size_t disk_size(int drive)
{
    return arm_disk_size(drive);
}

/*
 * `write_serial_string` ← `driver/serial/serial_port.cpp`。
 * 移植侧的输出通道就是 PS UART 串口 ⇒ 转发到 console(与 `write_serial_fmt`
 * 同一处理,只是不带格式化)。
 */
void write_serial_string(const char *str)
{
    if (str != NULL) {
        console_puts(str);
    }
}

/*
 * ---- 块层:`blk_device_read` / `blk_device_write` ----
 *
 * 上游实现在 `driver/device.cpp`(216/318/415 行),而那个文件**还没进 ARM 图**
 * —— 它卡在分区层与进程层上(计划把它归到 M4A-3/B5)。
 *
 * 而 ARM 侧今天**根本没有块设备**:`sdhci0` 在描述表里,但驱动一行没写
 * (M4A-1.3 的活)。所以这两个函数在当前构建里是**不可达**的
 * (`devfs_read`/`devfs_write` 只在节点挂的是块设备时才会走到它们)。
 *
 * ⇒ 按本项目的规矩做**响亮拒绝**:返回 0(调用方按"读到 0 字节"处理)
 *   并计数。★ "不可达"是**可判的**:计数必须恒为 0
 *   (`arm_blk_device_calls()`),由板上自检读。
 *   替换条件写进 README 的退化清单(D25):M4A-1.3 接上真实块设备、
 *   并把 `driver/device.cpp` 的块层搬进来时。
 */
static unsigned int g_blk_device_calls;

size_t blk_device_read(int drive, void *buffer, size_t offset, size_t length)
{
    (void)drive;
    (void)buffer;
    (void)offset;
    (void)length;
    g_blk_device_calls++;
    return 0u;
}

size_t blk_device_write(device_t device, const void *buffer, size_t offset, size_t length)
{
    (void)device;
    (void)buffer;
    (void)offset;
    (void)length;
    g_blk_device_calls++;
    return 0u;
}

extern "C" unsigned int arm_blk_device_calls(void)
{
    return g_blk_device_calls;
}

/*
 * ---- 挂 `/dev` ----
 *
 * ← `devfs.h:17` 的 `void devfs_setup();`(C++ 链接 `_Z11devfs_setupv`)。
 * 内部 = `vfs_regist("devfs", …)` + `vfs_mkdir("/dev")` + `vfs_mount(/dev)`
 * + `vfs_mkdir("/dev/ptx")`。
 *
 * ⚠ 必须在 **VFS 起搏之后**调:`vfs_mkdir` / `vfs_open` 都要根目录先在。
 *   而在它**之前**注册的设备**不会**有节点 —— 上游 `devfs_register` 自己
 *   就是 `if (devfs_root == NULL) { return EOK; }`(dev.cpp:345)。
 *   ★ 这一点正是新 A/B 的一侧:**上游语义天然给出"挂载前无名、挂载后有名"**,
 *   不需要移植侧再造一个开关(旧的 `devfs_ab_set_disabled` 已随骨架删除)。
 */
extern "C" void arm_devfs_setup(void)
{
    devfs_setup();
}

/*
 * "`/dev` 下这个节点存在吗" —— 给移植侧 C 用的探针。
 *
 * 为什么需要它:节点是不是真的建出来了,唯一可信的观测点是
 * `vfs_open("/dev/xxx")`(那正是上游 `devfs_register0()` 建节点的方式)。
 * 而 `vfs_node_t` 是**上游类型**,移植侧 C 看不到(合流纪律 #4)⇒
 * 由落地层做这一次打开/关闭,只把"成不成"这一位交回去。
 */
extern "C" int arm_devfs_node_exists(const char *path)
{
    vfs_node_t node = vfs_open(path);

    if (node == NULL) {
        return 0;
    }
    vfs_close(node);
    return 1;
}

/* ====================================================================
 * ★★ 管道(pipefs)起搏 + 验收转发(M4A-1.5)★★
 * ====================================================================
 *
 * ## 为什么"建一个管道"要写在这里、而且要照抄一段顺序
 *
 * 上游**没有** `pipe_create()` 这样的 API:`pipefs` 只提供
 * `mount/open/read/write/close/…` 这些**回调**,而"把管道造出来"这件事
 * 写在**系统调用层**里(`kernel/syscall/sys.cpp:3703-3762` 的 `sys_pipe2`)。
 * 那段代码干的是:
 *
 *     两个节点(`vfs_node_alloc(pipefs_root, name)`,type=file_pipe,
 *              fsid=pipefs_id,同 inode、dev=PIEFS_REGISTER_ID)
 *     + 一个 `pipe_info_t`(buf=calloc(PIPE_BUFF)、read_fds=write_fds=1)
 *     + 两个 `pipe_specific_t`(write=false / true)
 *     + 把 `node->handle` 与 `info->read_spec/write_spec` 接起来
 *
 * ⇒ 落地层这边**逐句照搬前半段**(到 `node->handle` 接线为止),
 *   后半段(sys.cpp:3763-3786)是**每进程 fd 表**那一层,属 M7,验收不需要它。
 *
 * ⚠ 为什么不把这一段放进移植侧 C:它通篇是 `vfs_node_t` / `pipe_info_t`
 *   这些**上游类型**(合流纪律 #4:移植侧 C 看不到上游头)。
 *
 * ## 节点名
 *
 * 用 `xpipeN` 而不是上游 syscall 层的 `pipeN` —— 前缀分开是为了让
 * "谁建的"一眼可辨(ARM 侧今天没有 syscall 层,两边不会同时存在,
 * 但名字混在一起会让以后的排查多一次猜测)。
 *
 * ⚠ 名字与计数器都是本文件私有的:不去动上游的 `pipefd_id`(`sys.cpp` 在用它)。
 */

extern vfs_node_t pipefs_root; /* driver/fs/vfs/pipefs.cpp:8(非 static)*/
extern int        pipefs_id;   /* driver/fs/vfs/pipefs.cpp:9(非 static)*/

static vfs_node_t   g_pipe_read_node;
static vfs_node_t   g_pipe_write_node;
static unsigned int g_pipe_name_counter;
static uint64_t     g_pipe_inode_next = 1u;

extern "C" int arm_pipefs_setup(void)
{
    /* ← `pipefs.h`/`pipe.h:35` 的 `void pipefs_setup();`(C++ 链接 `_Z13pipefs_setupv`)。
       内部 = `vfs_regist("pipefs", …)` + `vfs_mkdir("/pipe")` + `vfs_mount(/pipe)`。 */
    pipefs_setup();
    return (pipefs_root != NULL) ? 0 : -1;
}

extern "C" int arm_pipe_create(void)
{
    char         name[16];
    vfs_node_t   node_read;
    vfs_node_t   node_write;
    uint64_t     inode;
    pipe_info_t *info;
    pipe_specific_t *read_spec;
    pipe_specific_t *write_spec;

    if (pipefs_root == NULL) {
        return -1; /* 没挂载(调用方应先 arm_pipefs_setup)*/
    }

    sprintf(name, "xpipe%u", g_pipe_name_counter++);
    node_read = vfs_node_alloc(pipefs_root, name);
    if (node_read == NULL) {
        return -2;
    }
    node_read->type = file_pipe;
    node_read->fsid = pipefs_id;

    sprintf(name, "xpipe%u", g_pipe_name_counter++);
    node_write = vfs_node_alloc(pipefs_root, name);
    if (node_write == NULL) {
        return -3;
    }
    node_write->type = file_pipe;
    node_write->fsid = pipefs_id;

    /* 同一个管道 ⇒ 同一个 inode 号(上游 sys_pipe2 也是这么标的)*/
    inode              = g_pipe_inode_next++;
    node_read->inode   = inode;
    node_write->inode  = inode;
    node_read->dev     = PIEFS_REGISTER_ID;
    node_write->dev    = PIEFS_REGISTER_ID;

    info = (pipe_info_t *)malloc(sizeof(pipe_info_t));
    if (info == NULL) {
        return -4;
    }
    memset(info, 0, sizeof(pipe_info_t));
    info->buf = (char *)calloc(1u, PIPE_BUFF);
    if (info->buf == NULL) {
        free(info);
        return -5;
    }
    info->read_fds  = 1;
    info->write_fds = 1;
    info->ptr       = 0;

    /*
     * ⚠ 上游 `sys_pipe2` 这里还有一句 `info->lock = SPIN_INIT;` —— 我们**省了**,
     *   理由:上面刚 `memset(info, 0, …)`,而移植侧的 `SPIN_INIT` 展开就是
     *   `{0, 0}`(`arch/cpu.h:506`),两者等价。(写上去在 C++ 里也合法,
     *   只是"清零之后再赋一遍零"会让读者以为那个字段有别的初值。)
     */

    read_spec  = (pipe_specific_t *)malloc(sizeof(pipe_specific_t));
    write_spec = (pipe_specific_t *)malloc(sizeof(pipe_specific_t));
    if (read_spec == NULL || write_spec == NULL) {
        return -6;
    }
    read_spec->write  = false;
    read_spec->info   = info;
    read_spec->node   = node_read;
    write_spec->write = true;
    write_spec->info  = info;
    write_spec->node  = node_write;

    info->read_spec  = read_spec;
    info->write_spec = write_spec;

    node_read->handle  = read_spec;
    node_write->handle = write_spec;

    g_pipe_read_node  = node_read;
    g_pipe_write_node = node_write;
    return 0;
}

extern "C" int arm_pipe_write(const void *data, unsigned int len)
{
    if (g_pipe_write_node == NULL) {
        return -1;
    }
    return (int)vfs_write(g_pipe_write_node, (void *)data, 0u, (size_t)len);
}

extern "C" int arm_pipe_read(void *buf, unsigned int len)
{
    if (g_pipe_read_node == NULL) {
        return -1;
    }
    return (int)vfs_read(g_pipe_read_node, buf, 0u, (size_t)len);
}

/*
 * 管道里当前有多少字节。
 *
 * `pipefs_stat()` 干的就是这一件事:`node->size = pipe->ptr`
 * (`pipefs.cpp:247-253`)。⇒ 先 `vfs_update()`(它触发 stat 回调)再读 `size`。
 *
 * ★ 为什么它是这一组判据的关键:`arm_pipe_write()` 返回 32 只说明
 *   "它说它写了 32";而"管道里**现在**有 32 字节"是**另一个观测点** ——
 *   读完之后它必须回到 0。两侧都成立才说明数据真的进了管道缓冲区,
 *   而不是某个空壳函数把入参原样返回。
 */
extern "C" int arm_pipe_fill(void)
{
    if (g_pipe_read_node == NULL) {
        return -1;
    }
    vfs_update(g_pipe_read_node);
    return (int)g_pipe_read_node->size;
}
