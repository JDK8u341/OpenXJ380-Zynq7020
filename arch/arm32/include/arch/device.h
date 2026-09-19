#pragma once

/*
 * 设备管理器 + devfs 骨架 —— M4A-1.1b
 *
 * ====================================================================
 * 与上游 include/device.h 的关系(别混淆)
 * ====================================================================
 *
 * 这**不是**上游那份的复制,而是"移植侧按上游形状写的"一份:结构体字段、
 * 回调形状、函数签名逐项对齐 —— 目标是 M4A-3(合流决策里的 B5)把两边
 * 并成一套时,驱动源码不必改。
 *
 * **为什么不直接 #include 上游那份**:它第 5 行 `#include "proto.hpp"`,
 * 而 proto.hpp 又拉 `<ps2/keyboard.h>`、`<efi/fbc.h>`、mm 下的头文件、`<smp/smp.h>`
 * —— 整个 x86 主干。这一步要的只是 `device_t` 的**形状**。
 *
 * ★ 上游 `driver/device.cpp` 在 1.1b 阶段**搬不进来**(与 C++ 无关,原因有三):
 *   `regist_device` 依赖 `partition_device_added()`(在 VFS/分区层)、
 *   `device_manager_init()` 依赖 256 个 mutex、
 *   `delete_device()` 的块路径依赖 `get_current_task()->parent_group->pagedir`
 *   与 `user_range_mapped()`(**进程层,M7**)。
 *   ⇒ 那些属于 M4A-1.3 / M4A-3。本文件只做"注册 → 取回 → 注销"这条往返。
 *
 * ⚠ **形状由 tests/test_arm32_device.py 逐字段对着上游 include/device.h 钉住**
 *   (与 errno 的逐值比对同一个套路):上游改了字段而这里没跟上,测试会报。
 *
 * ====================================================================
 * ★ 与 M3 的描述层是**两套东西**
 * ====================================================================
 *
 *   `arch/plat_device.h`(M3)  —— 硬件**描述层**的节点:"这块 PL 里有个
 *                                axi_gpio",数据来自 XSA 生成的 board_devices.c
 *   本文件(M4A-1.1b)         —— 设备**管理器**的槽位:"这个设备已注册,
 *                                有一个 id、一个 /dev 下的名字"
 *
 * 上游只有后者。**M4A-3 的活就是把前者的数据喂给后者** —— 在那之前,
 * 描述层里 probe 成功的驱动**还不会**自动出现在这里。
 *
 * ====================================================================
 * 适配记录(与上游的差异,都是有意的)
 * ====================================================================
 *
 * 1. `size_t` 在 ARM 上是 32 位、x86_64 上是 64 位 ⇒ `size`/`sector_size`/
 *    `vdiskid`/`max_size` 这些字段的**宽度随架构**。这是指针宽度差异的
 *    必然后果,不是笔误 —— 两边共用的只能是**字段名与顺序**。
 * 2. 上游 `get_device()` 不检查 `id` 上界(`device_ctl[id].flag` 直接下标);
 *    256 是硬编码的。这里加了上界检查:越界读会让调用方拿到一段**别人的
 *    内存**并当成设备用,而"返回 NULL"至少是可诊断的。
 * 3. 块层的那几个函数(`device_read` / `device_write` / `blk_device_*` /
 *    `device_mmap` / `rw_device`)**没有声明** —— 它们属于 M4A-1.3(块设备)。
 *    不声明而不是声明成空实现:那样调用方会得到一句清楚的
 *    "implicit declaration",而不是链到一半才发现是空壳。
 */

#include <arch/types.h>
#include <krlibc.h>

/*
 * 前向声明:上游这个类型来自 <vbuffer.h>(`struct vecbuf`)。
 * 这里只需要"它是个不完整类型"就够 —— 下面两个回调只拿它的**指针**,
 * 而指针参数不依赖被指类型的布局。真正的定义随 M4A-1.2 的 VFS 一起来。
 */
struct vecbuf;

/* 上游 device.h 第 3 行的同名宏,一起搬过来以免将来共享源码时缺它 */
#define SECTORS_ONCE 8

typedef enum
{
    DEVICE_BLOCK,
    DEVICE_STREAM,
    DEVICE_FB,
} device_flag_t;

typedef size_t (*read_vbuff)(int drive, struct vecbuf *buffer, size_t number, size_t lba);
typedef size_t (*write_vbuff)(int drive, struct vecbuf *buffer, size_t number, size_t lba);
typedef size_t (*readf)(int drive, uint8_t *buffer, size_t number, size_t lba);
typedef size_t (*writef)(int drive, uint8_t *buffer, size_t number, size_t lba);

/*
 * ⚠ 前向声明 —— 上游**没有**这一行,因为它是 C++:在 C++ 里,
 *   参数表里的 `struct _device *` 会把 `_device` 引入**外层作用域**;
 *   而 C 里只引入**原型作用域**,于是下面那个 `ioctlf` 会报
 *   "declared inside parameter list will not be visible outside of this
 *    definition"(`-Werror` 下直接编不过)。
 *   这是上游头文件在 C 侧的**唯一**真实障碍 —— 不是风格问题。
 */
struct _device;

typedef int (*ioctlf)(struct _device *device, size_t req, void *handle);
typedef int (*pollf)(size_t events);
typedef void *(*mapf)(int drive, void *addr, size_t len);

typedef struct _device
{
    size_t (*read_vbuf)(int drive, struct vecbuf *buffer, size_t number, size_t lba);
    size_t (*write_vbuf)(int drive, struct vecbuf *buffer, size_t number, size_t lba);
    size_t (*read)(int drive, uint8_t *buffer, size_t number, size_t lba);
    size_t (*write)(int drive, uint8_t *buffer, size_t number, size_t lba);
    int (*ioctl)(struct _device *device, size_t req, void *handle);
    int (*poll)(size_t events);
    void *(*map)(int drive, void *addr, size_t len);
    int           flag;
    size_t        size;
    size_t        sector_size;
    device_flag_t type;
    char          drive_name[50];
    size_t        vdiskid;
    char         *path;
    uint64_t      max_size;
} device_t;

/*
 * 设备表槽数。上游没有这个宏 —— 它是 device.cpp 里写死的 256
 * (`device_t device_ctl[256]`、`id_allocator_create(256)`、
 * `for (size_t i = 0; i < 256; i++)`)。这里给个名字,值保持一样。
 */
#define DEVICE_TABLE_SIZE 256u

/*
 * 设备表本身。上游也是全局的(`driver/device.cpp:12` 的
 * `device_t device_ctl[256]`),而 `driver/fs/vfs/dev.cpp` 直接用它。
 * 这里保持同样的可见性,免得 devfs 那边要额外开一个访问函数。
 */
extern device_t device_ctl[DEVICE_TABLE_SIZE];

/*
 * ★ 从下面这一行起,所有函数声明都放进 `extern "C"`(2026-09-18)。
 *
 * 为什么必须:**移植侧的实现是 C**(src/device.c),导出的符号是未修饰的
 * `regist_device`;而上游的 C++ 文件(如 driver/fs/vfs/vfs.cpp)默认按
 * **C++ 名字修饰**去找它 —— 实测链不上,报的是
 *     undefined reference to `regist_device(char const*, _device)'
 * (注意那是修饰过的签名)。加上 `extern "C"` 之后两边对上。
 *
 * ⚠ 对 x86 **零影响**:那边用的是上游自己的 `include/device.h`,
 *   根本不包含本文件。
 *
 * 这条与"上游 C++ 导出 C 符号 + 移植侧给 C 声明"那条边界规矩是**对称**的:
 * 这里是反过来 —— C 实现 + C++ 调用方。
 */
#ifdef __cplusplus
extern "C" {
#endif

/* ---- 设备管理器 ---- */

/*
 * 建表。上游还顺手 create 了 256 把 blk_cached_bounce_lock 互斥 ——
 * 那是块层(M4A-1.3)的事,这里不做。
 */
int device_manager_init(void);

/*
 * 注册一个设备,返回它的 id(表满返回 -1)。
 *
 * ⚠ **按值传结构体** —— 与上游一致,不要"顺手优化"成指针:
 *   改指针会让两侧 ABI 分叉,而分叉的代价远大于一次拷贝(计划 §4.4 第 3 条)。
 *
 * ⚠ `vd.flag` 由**调用方**置非 0,本函数不碰它 —— 上游就是这样
 *   (ahci/ide/nvme/partition 都是 `dev.flag = 1;` 之后才注册)。
 *   `flag == 0` 的槽位对 `get_device()` 不可见。
 *
 * ⚠ 上游在本函数里还会:成功时对 `DEVICE_BLOCK` 调 `partition_device_added()`
 *   (自动分区扫描)。**这里没有做** —— 分区层属 M4A-1.3/1.4。
 *   在那之前,块设备注册了但**不会被自动扫分区**,这是已知且记录在案的缺口。
 */
int regist_device(const char *path, device_t vd);

/* 注销:删 devfs 节点、释放路径副本、归还 id */
void delete_device(int vdiskid);

/*
 * 按 id 取设备。槽位空(`flag == 0`)或 id 越界返回 NULL。
 * 见文件头"适配记录 2":上界检查是我们加的。
 */
device_t *get_device(size_t id);

/* 槽位在用(`flag > 0`)—— 上游同名同义 */
bool have_vdisk(int drive);

/* 设备容量;槽位空返回 0 */
size_t disk_size(int drive);

/* ---- devfs 骨架 ---- */

/*
 * 上游的 devfs 是**真的 VFS**(driver/fs/vfs/dev.cpp:在 /dev 下建节点、
 * 挂 device_handle、支持 open/read/write)。这里只有一张"名字 → id"的
 * 节点表,够 `regist_device` 有真实语义(注册后能被名字找到)。
 *
 * ⚠ 这是**骨架,不是完成态**:`/dev` 下并没有节点,也没有文件操作。
 *   计划 §4.4 第 4 条明写"不允许把'只登记不建节点'当成完成态" ——
 *   真正的 devfs 随 M4A-1.2 的 VFS 一起来。
 *
 * 语义照抄上游的两个可观察点:
 *   - `devfs_register` 会把 path **复制一份**存进 `device_ctl[id].path`
 *     (上游 `dev.cpp:322` 的 `strdup`),`delete_device` 负责 free;
 *   - path 传 NULL 表示"注册到 /dev 根下",此时 `.path` 为 NULL。
 */
errno_t devfs_register(const char *path, size_t id);
errno_t devfs_delete(const char *path);

/*
 * 按名字找设备 id(骨架特有,上游没有 —— 上游靠 `vfs_open("/dev/xxx")`)。
 * 找不到返回 -1。
 *
 * 它的存在是为了让"注册→取回"这条往返有一个**独立于设备表**的观测点:
 * 只查 `get_device(id)` 的话,即使 devfs 什么都没做也会通过。
 */
int devfs_lookup(const char *name);

/* 当前已注册的 devfs 节点数(自检用) */
size_t devfs_node_count(void);

/* 清空节点表(重复自检用;上游没有对应物) */
void devfs_reset(void);

/*
 * ★ 破坏性 A/B 开关(仅自检用):置 1 时 `devfs_register` **只登记不建节点**。
 *
 * 这正是计划 §4.4 第 4 条点名的 **B5-b 退化形态**。它的用处是证明
 * "注册→取回"那条往返**确实**依赖 devfs:关掉之后 `get_device` 照样成功、
 * 而 `devfs_lookup` 必须找不到 —— 少了任何一侧,判据就没有区分能力。
 *
 * 与本项目其它几组 A/B 同样的性质:**预期它坏**。
 */
void devfs_ab_set_disabled(u32 on);

#ifdef __cplusplus
} /* extern "C" */
#endif