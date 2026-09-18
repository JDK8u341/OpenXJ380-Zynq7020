/*
 * devfs 骨架 —— M4A-1.1b
 *
 * ====================================================================
 * 它是什么、不是什么
 * ====================================================================
 *
 * 上游的 devfs 是**真的文件系统**(driver/fs/vfs/dev.cpp,325 行):
 * 在 /dev 下建 VFS 节点、挂 device_handle、实现 open/read/write/ioctl。
 *
 * 这里只有一张"**名字 → 设备 id**"的节点表。它的全部作用是让
 * `regist_device()` 有真实语义 —— 注册之后,设备能被**名字**找回来,
 * 而不只是"表里多了一行"。
 *
 * ⚠ 这是骨架,**不是完成态**(计划 §4.4 第 4 条明写:不允许把"只登记
 *   不建节点"当成完成态)。今天 `/dev` 下没有任何节点,也没有文件操作;
 *   真正的 devfs 随 M4A-1.2 的 VFS 一起来。
 *
 * ====================================================================
 * 语义:哪些照抄上游,哪些是骨架自己的
 * ====================================================================
 *
 * 照抄的(上游 dev.cpp 的可观察行为):
 *   - `devfs_register(path, id)` 会把 path **复制一份**存进
 *     `device_ctl[id].path`(上游 :322 的 strdup),`delete_device` 负责 free;
 *     path 传 NULL 时 `.path` 就是 NULL;
 *   - `devfs_delete(name)` 的参数是「/dev 下的节点名」—— 上游是
 *     `buf = "/dev/" + path` 然后 unlink 那个节点(:300-314),所以参数
 *     语义就是"节点名",不是"父目录"。
 *
 * ⚠ 一处**纠错**(不是照抄):上游 `delete_device()` 在 `dev.path != NULL` 时
 *   传的是 `dev.path`(那是**父目录**),于是 `devfs_delete` 会去 unlink
 *   `/dev/<父目录>` 这个节点 —— 删错了对象。只有当 `dev.path == NULL` 时
 *   它才传 `dev.drive_name`(对的)。
 *   今天**四个调用方全都传 NULL**(ide/ahci/nvme/xhci 都是
 *   `regist_device(NULL, dev)`),所以那条错路从没被走到 —— 是潜伏的,不是
 *   正在发作的。我们这边统一按**节点名**处理(即上游走对的那条)。
 *
 * 骨架自己的(上游没有,因为上游有真的目录树):
 *   - `path`(父目录)被**接受但不生效** —— 骨架没有目录。今天所有调用方
 *     都传 NULL,所以这一点不影响任何现有调用;M4A-1.2 引入真 devfs 时
 *     它才有意义。
 *   - ⇒ 相应地,**同名不同父**的两个设备在骨架里会撞在一起(`devfs_lookup`
 *     按名字找)。真 devfs 到位后这个限制自然消失。
 *   - `devfs_lookup()` / `devfs_node_count()` / `devfs_reset()` 是给自检用的
 *     (上游靠 `vfs_open("/dev/xxx")`,不需要它们)。
 */

#include <arch/device.h>
#include <arch/errno.h>

#include <krlibc.h>

/* 骨架上限。上游没有上限(节点是动态分配的),这里给一个并**记住它是个上限** */
#define DEVFS_MAX_NODES 64u

typedef struct
{
    char  *name; /* strdup 得到的节点名(= 设备的 drive_name) */
    size_t id;   /* 对应的设备槽位 */
} devfs_node_t;

static devfs_node_t g_nodes[DEVFS_MAX_NODES];
static size_t       g_node_count;

/*
 * ★ 破坏性 A/B 开关(见 <arch/device.h> 的说明)。
 *
 * 置 1 时 register 走完"把路径副本存进设备表"那一步就返回 EOK,
 * **不建节点** —— 这正是计划 §4.4 点名的 B5-b 退化形态。
 * 自检用它证明往返确实依赖 devfs,而不是只看 get_device 就通过。
 */
static u32 g_devfs_disabled;

void devfs_ab_set_disabled(u32 on)
{
    g_devfs_disabled = on != 0u ? 1u : 0u;
}

static devfs_node_t *find_node(const char *name)
{
    size_t i;

    if (name == NULL) {
        return NULL;
    }

    for (i = 0; i < g_node_count; i++) {
        if (g_nodes[i].name != NULL && strcmp(g_nodes[i].name, name) == 0) {
            return &g_nodes[i];
        }
    }

    return NULL;
}

errno_t devfs_register(const char *path, size_t id)
{
    device_t    *device;
    devfs_node_t *slot;

    if (id >= DEVICE_TABLE_SIZE) {
        return ENODEV;
    }

    device = &device_ctl[id];

    /*
     * 路径副本存进设备表 —— 上游 :322 就是这一句。
     * ⚠ 先释放旧的:同一个槽位被重复注册时,不释放就是一次泄漏,
     *   而"重复注册"在调试期很常见(反复 init)。
     */
    if (device->path != NULL) {
        free(device->path);
        device->path = NULL;
    }
    if (path != NULL) {
        device->path = strdup(path);
        if (device->path == NULL) {
            return ENOMEM;
        }
    }

    /*
     * 节点名取自 drive_name(上游 :55 的 `strdup(device->drive_name)`),
     * 而不是 path —— path 是父目录。
     */
    if (device->drive_name[0] == '\0') {
        /* 没有名字就没法建节点。上游会 append 一个空名字的孩子(基本是坏的),
           这里当成错误报出来 —— 悄悄建一个空名节点更难查。 */
        return EINVAL;
    }

    /* ★ 破坏性 A/B:只登记不建节点(B5-b 退化形态)。放在这里而不是函数开头
       是刻意的 —— 路径副本那一步要照常走完,否则对照组就变成"注册整个失败",
       失去了"只差 devfs 这一环"的针对性。 */
    if (g_devfs_disabled != 0u) {
        return EOK;
    }

    slot = find_node(device->drive_name);
    if (slot != NULL) {
        /* 同名重复注册:覆盖 id(上游会建出两个同名孩子,那是它的 bug) */
        slot->id = id;
        return EOK;
    }

    if (g_node_count >= DEVFS_MAX_NODES) {
        return ENOMEM;
    }

    g_nodes[g_node_count].name = strdup(device->drive_name);
    if (g_nodes[g_node_count].name == NULL) {
        return ENOMEM;
    }
    g_nodes[g_node_count].id = id;
    g_node_count++;

    return EOK;
}

errno_t devfs_delete(const char *name)
{
    devfs_node_t *node;
    size_t        i;

    node = find_node(name);
    if (node == NULL) {
        return ENOENT;
    }

    free(node->name);

    /* 用最后一个节点填洞 —— 顺序不重要,而保持 [0, count) 连续让遍历简单 */
    i = (size_t)(node - &g_nodes[0]);
    g_node_count--;
    if (i != g_node_count) {
        g_nodes[i] = g_nodes[g_node_count];
    }
    g_nodes[g_node_count].name = NULL;
    g_nodes[g_node_count].id   = 0u;

    return EOK;
}

int devfs_lookup(const char *name)
{
    devfs_node_t *node = find_node(name);

    return node != NULL ? (int)node->id : -1;
}

size_t devfs_node_count(void)
{
    return g_node_count;
}

void devfs_reset(void)
{
    size_t i;

    for (i = 0; i < g_node_count; i++) {
        free(g_nodes[i].name);
        g_nodes[i].name = NULL;
        g_nodes[i].id   = 0u;
    }

    g_node_count = 0u;
}
