/*
 * VFS 起搏 + 验收(M4A-1.2b 的验收项)
 *
 * 判据为什么是这几条、分层理由、以及"为什么调用在 C++ 而判据在这里",
 * 都写在 <arch/vfs_check.h> 的文件头上。这里只写实现上的取舍。
 */

#include <arch/vfs_check.h>

#include <arch/console.h>
#include <krlibc.h>

/* ------------------------------------------------------------------ */
/* 落地层转发(手写声明,理由见 vfs_check.h 与 upstream_api.cpp)        */
/* ------------------------------------------------------------------ */

/*
 * 移植侧 C 只能这样声明:VFS 的公开 API 是 **C++ 链接**的
 * (`include/fs/vfs/vfs.h` 整个文件没有 `extern "C"`),C 给不出那些符号名。
 * 定义在 `arch/arm32/src/upstream_api.cpp`,**全是 `extern "C"`**
 * (契约测试 `tests/test_arm32_vfs.py` 对着两边的文本逐条比对)。
 */
extern int arm_vfs_bringup(void);
extern int arm_vfs_create(const char *path);
extern int arm_vfs_write(const char *path, const void *data, unsigned int len);
extern int arm_vfs_read(const char *path, void *buf, unsigned int len);
extern int arm_vfs_child_count(const char *path);
extern int arm_vfs_fsid(const char *path);
extern int arm_vfs_cwd_is_root(void);

/* ------------------------------------------------------------------ */
/* 探针                                                                */
/* ------------------------------------------------------------------ */

/*
 * 载荷长度在 <arch/vfs_check.h> —— 自检报告的判据也要用同一个数。
 */
#define VFS_CHECK_NEG_PATH "/xj380_root_probe.txt"

/* 载荷/回读缓冲。刻意用 `static`(**不在栈上**):
 * VFS 的读写回调会拿着这个指针过一遍 `do_update()` 与 `memcpy`,
 * 32 字节虽小,但启动早期 kmain 的栈是 8 KiB 级别的稀缺资源 ——
 * 这里多占 64 字节不值得。 */
static u8               g_payload[VFS_CHECK_PAYLOAD_LEN];
static u8               g_readback[VFS_CHECK_PAYLOAD_LEN];
static vfs_check_t      g_result;
static bool             g_ran;

const vfs_check_t *vfs_check_run(void)
{
    int i;

    if (g_ran) {
        return &g_result;
    }
    g_ran = true;

    memset(&g_result, 0, sizeof(g_result));
    g_result.fsid_root  = -1;
    g_result.fsid_tmp   = -1;
    g_result.list_before = -1;
    g_result.list_after  = -1;

    /*
     * ① 起搏:内核进程上下文 → `vfs_init()` → `tmpfs_setup()` → cwd。
     *
     * ⚠ 必须最先做,而且**必须在堆之后**(调用方保证):
     *   `arm_kernel_process_init()` 里的 `queue_init()` 与 `vfs_init()`
     *   里的 `vfs_node_alloc()` 都走 malloc。
     */
    g_result.bringup = arm_vfs_bringup();
    if (g_result.bringup != 0) {
        /* 起搏失败 ⇒ 后面每一步都没有意义。字段留在初值上,
         * 报告里会看到 -1/0,与"跑过并失败"区分得开 */
        console_printf(" VFS check   : bringup FAILED (%d)\n", g_result.bringup);
        return &g_result;
    }

    /*
     * ② 基线:建文件**之前** `/tmp` 有几个子项。
     *
     * 有了它,"列目录"这条判据才是**非空泛**的:只看"之后 >= 2"的话,
     * 一个恒返回大数的假实现也能过;而"之后比之前多 2"要求
     * `vfs_child_append()` 真的把两个文件挂进了这棵目录树。
     */
    g_result.list_before = arm_vfs_child_count("/tmp");

    /* ③ 建两个文件 —— "建" */
    g_result.create_a = arm_vfs_create("/tmp/xj380_a.txt");
    g_result.create_b = arm_vfs_create("/tmp/xj380_b.txt");

    /* ④ 写 —— 载荷是可验证的模式,不是一串 0(全 0 的缓冲区读回来也是 0) */
    for (i = 0; i < VFS_CHECK_PAYLOAD_LEN; i++) {
        g_payload[i] = (u8)((i * 7u) + 3u);
    }
    memset(g_readback, 0xAA, sizeof(g_readback)); /* 哨兵:没读到就留着 0xAA */

    g_result.payload_len = VFS_CHECK_PAYLOAD_LEN;
    g_result.write_a     = arm_vfs_write("/tmp/xj380_a.txt", g_payload, VFS_CHECK_PAYLOAD_LEN);

    /* ⑤ 读 + 逐字节比对 —— 这一条是整组判据里最强的一层 */
    g_result.read_a = arm_vfs_read("/tmp/xj380_a.txt", g_readback, VFS_CHECK_PAYLOAD_LEN);

    g_result.roundtrip = 0;
    if (g_result.read_a == VFS_CHECK_PAYLOAD_LEN &&
        memcmp(g_payload, g_readback, VFS_CHECK_PAYLOAD_LEN) == 0) {
        g_result.roundtrip = 1;
    }

    /* ⑥ 列目录 —— "列" */
    g_result.list_after = arm_vfs_child_count("/tmp");

    /* ⑦ 挂载判据:`/tmp` 必须属于**另一个**文件系统实例 */
    g_result.fsid_root = arm_vfs_fsid("/");
    g_result.fsid_tmp  = arm_vfs_fsid("/tmp");

    /* ⑧ 内核进程的 cwd 生效了没有(`set_cwd` 有没有真的落到 tcb 上) */
    g_result.cwd_root = arm_vfs_cwd_is_root();

    /*
     * ⑨ ★ 反面控制:在**根**文件系统上做同一件事,必须**不成立**。
     *
     * 为什么非要它(见 vfs_check.h):上游给"没有文件系统的节点"填的回调是
     * `empty_func`(`static void empty_func() {}` —— **返回 void**,
     * 却被塞进 `size_t (*)(...)` 的槽里)。于是"调用成功"这件事在这里
     * **本来就不携带信息**:返回值是未定义的。
     * 只有"根 fs 上往返不成立"才能证明 ④⑤ 的成功来自 tmpfs 的回调,
     * 而不是来自那个空函数。
     *
     * ⚠ 这一步**不破坏任何东西**:它只在根目录的孩子表里多挂一个节点,
     *   并把 `g_readback` 重新填成哨兵。不删文件、不卸挂载。
     */
    memset(g_readback, 0xAA, sizeof(g_readback));
    g_result.neg_create = arm_vfs_create(VFS_CHECK_NEG_PATH);
    g_result.neg_write  = arm_vfs_write(VFS_CHECK_NEG_PATH, g_payload, VFS_CHECK_PAYLOAD_LEN);
    g_result.neg_read   = arm_vfs_read(VFS_CHECK_NEG_PATH, g_readback, VFS_CHECK_PAYLOAD_LEN);

    g_result.negative_ok = 0;
    if (!(g_result.neg_write == VFS_CHECK_PAYLOAD_LEN &&
          g_result.neg_read == VFS_CHECK_PAYLOAD_LEN &&
          memcmp(g_payload, g_readback, VFS_CHECK_PAYLOAD_LEN) == 0)) {
        g_result.negative_ok = 1;
    }

    console_printf(" VFS check   : roundtrip=%s write=%d read=%d list=%d->%d fsid=%d/%d%s\n",
                   g_result.roundtrip ? "PASS" : "FAIL",
                   g_result.write_a, g_result.read_a,
                   g_result.list_before, g_result.list_after,
                   g_result.fsid_root, g_result.fsid_tmp,
                   g_result.negative_ok ? "" : " (negative control FAILED)");

    return &g_result;
}
