/*
 * 设备管理器 —— M4A-1.1b
 *
 * 上游对应物是 driver/device.cpp(540 行)。这里只做**注册 → 取回 → 注销**
 * 这条往返,以及它的前置(id 分配 + devfs 登记)。
 *
 * 上游那 540 行里,本阶段**没做**的部分及原因(都在头文件里记过):
 *   - 256 把 blk_cached_bounce_lock + bounce 缓冲逻辑 → 块层,M4A-1.3
 *   - partition_device_added() 自动分区扫描 → 分区层,M4A-1.3/1.4
 *   - device_read/write、blk_device_*、device_mmap、rw_device → 块层
 *   - 那些函数的 user_range_mapped()/PCB 依赖 → 进程层,M7
 */

#include <arch/device.h>
#include <arch/errno.h>
#include <arch/id_alloc.h>

#include <arch/console.h>
#include <krlibc.h>

device_t device_ctl[DEVICE_TABLE_SIZE];

static id_allocator_t *g_dev_allocator;

int device_manager_init(void)
{
    size_t i;

    for (i = 0; i < DEVICE_TABLE_SIZE; i++) {
        device_ctl[i].flag = 0; /* 上游 :533 —— 0 表示槽位未使用 */
        /*
         * ⚠ 上游这一轮循环**没有**清 path。后果是:对同一个槽位重复 init,
         *   上一次注册留下的路径副本会**泄漏**(指针被下一次注册覆盖)。
         *   我们连它一起清 —— 见文件末尾的偏差记录。
         */
        device_ctl[i].path = NULL;
    }

    g_dev_allocator = id_allocator_create(DEVICE_TABLE_SIZE);

    /*
     * ⚠ 上游无条件 `return 0`,分配失败要等到第一次 regist_device 才暴露
     *   (那时 id_alloc(NULL) 返回 -1)。这里把失败报出来 —— 调用方
     *   (kmain)拿到非 0 就能当场判定,而不是等到某个驱动注册不上。
     */
    return g_dev_allocator == NULL ? -1 : 0;
}

int regist_device(const char *path, device_t vd)
{
    int     i;
    errno_t ret;

    if (g_dev_allocator == NULL) {
        return -1;
    }

    i = id_alloc(g_dev_allocator);
    if (i == -1) {
        return -1;
    }

    device_ctl[i]         = vd;
    device_ctl[i].vdiskid = (size_t)i;

    ret = devfs_register(path, (size_t)i);
    if (ret != EOK) {
        console_printf("Registers (%s)device error: %d\n", vd.drive_name, (int)ret);
    } else {
        console_printf("Registers (%s)device success\n", vd.drive_name);
        /*
         * ⚠ 上游这里还有一句:type == DEVICE_BLOCK 时调
         *   `partition_device_added(i)` 触发**自动分区扫描**。
         *   **我们没有做** —— 分区层属 M4A-1.3/1.4。
         *   ⇒ 已知缺口:在那之前,块设备能注册、能被 get_device 取回,
         *     但**不会被自动扫出分区**。写在这里而不是悄悄跳过,
         *     是因为"注册成功"这四个字很容易让人以为后面都通了。
         */
    }

    /*
     * ⚠ 与上游一致:devfs 登记失败时**仍然返回 id**。
     *   上游的语义是"设备已经在表里了,只是 /dev 下没有节点",
     *   调用方拿到的是有效 id。改成返回 -1 会让两侧语义分叉。
     */
    return i;
}

void delete_device(int vdiskid)
{
    device_t dev;

    if (vdiskid < 0 || (size_t)vdiskid >= DEVICE_TABLE_SIZE) {
        return;
    }

    dev = device_ctl[vdiskid]; /* 上游 :193 也是先拷一份 */

    /*
     * 节点名 = drive_name。上游这里写的是 `if (dev.path != NULL)
     * devfs_delete(dev.path); else if (drive_name[0]) devfs_delete(drive_name);`
     * —— 但 devfs_delete 的参数其实是「/dev 下的节点名」(上游 dev.cpp:300-314
     * 拼的是 "/dev/<参数>" 再 unlink 那个节点),而 dev.path 是**父目录**,
     * 所以那条分支会删错对象。走对的是 drive_name 那条。
     * 我们统一用 drive_name。详见 src/devfs.c 的说明。
     */
    if (dev.drive_name[0] != '\0') {
        (void)devfs_delete(dev.drive_name);
    }

    if (device_ctl[vdiskid].path != NULL) {
        free(device_ctl[vdiskid].path);
        /*
         * ★ 这一句是**我们加的**:上游 free 的是局部拷贝里的 path,
         *   设备表那一条仍然指着已释放的内存。槽位被重新注册时,
         *   devfs_register 会先 free 旧指针 ⇒ **二次释放**。
         *   上游靠"regist_device 整体覆盖 vd"侥幸躲开,我们不靠运气。
         */
        device_ctl[vdiskid].path = NULL;
    }

    /*
     * ★★ 槽位必须**真的**清掉 —— 这一条是 M4A-1.5 上板时被自检抓出来的 ★★
     *
     * 上游 `delete_device()`(`driver/device.cpp:183-211`)结尾写的是:
     *
     *     device_t dev = device_ctl[vdiskid];   // ← 局部**拷贝**
     *     dev.path = NULL; dev.flag = 0; dev.vdiskid = 0;   // ← 改的是拷贝
     *
     * 也就是说:**上游的意图清清楚楚**(把槽位标成空闲、清掉 path/vdiskid),
     * 但那三句写在了拷贝上,设备表那一条**一点没变**。
     *
     * 后果是"幽灵设备":`delete_device(id)` 之后
     *   - `get_device(id)` 仍然返回**非 NULL**(flag 还是 1)——
     *     里面 `path` 指向已释放内存;
     *   - `have_vdisk(id)` 仍然为真,`disk_size(id)` 照旧给数;
     *   ⇒ 任何"删掉之后还会被查到"的路径(分区扫描、块层)都会拿到一个
     *     已经失效的句柄。上游自己没发作,是因为四个调用方删完就不再用。
     *
     * ⇒ 按**上游写出来的意图**修(与本文件其它几处"照意图不照字面"一致,
     *   也与 D16 那条同型):把这三个字段真的清在设备表上。
     *   判据在板上:设备自检里 `delete_device(id_a)` 之后
     *   `get_device(id_a)` **必须**是 NULL(`device_roundtrip`)。
     */
    device_ctl[vdiskid].flag    = 0;
    device_ctl[vdiskid].vdiskid = 0;

    if (g_dev_allocator != NULL) {
        (void)id_free(g_dev_allocator, (uint32_t)vdiskid);
    }
}

device_t *get_device(size_t id)
{
    /*
     * ⚠ 上界检查是我们加的。上游是 `device_ctl[id].flag` 直接下标 ——
     *   `get_device(300)` 会读到**别的内存**并当成设备用,而调用方
     *   (partition.cpp 有 5 处)不会觉得有什么不对。返回 NULL 至少可诊断。
     *
     * 注意 flag 检查与上游一致:id 有效但槽位空 ⇒ NULL。
     */
    if (id >= DEVICE_TABLE_SIZE) {
        return NULL;
    }

    if (device_ctl[id].flag == 0) {
        return NULL;
    }

    return &device_ctl[id];
}

bool have_vdisk(int drive)
{
    if (drive < 0 || (size_t)drive >= DEVICE_TABLE_SIZE) {
        return false;
    }

    return device_ctl[drive].flag > 0;
}

size_t disk_size(int drive)
{
    if (!have_vdisk(drive)) {
        return 0u;
    }

    return device_ctl[drive].size;
}

/*
 * ★ 落地层入口:只换名字、不换语义(M4A-1.5)★
 *
 * 上游 `driver/fs/vfs/dev.cpp` 是 C++ 翻译单元,它要的是**修饰名**
 * `_Z9disk_sizei`;而这份实现是 C(未修饰的 `disk_size`)—— 两者不是同一个
 * 符号(与 `sprintf` 那次同型,坑表 55)。
 * 为什么不让上游声明变 `extern "C"`:上游 x86 侧同时有 `size_t disk_size(int)`
 * (`driver/device.cpp:216`)与 `u32 disk_size(byte)`(`diskio.cpp:181`),
 * 给前者加 C 链接会在 C 里撞名(C 没有重载)、直接弄坏 x86 构建。
 * ⇒ 由落地层给出 C++ 名并转发到这里;实现仍然只有一份。
 */
size_t arm_disk_size(int drive)
{
    return disk_size(drive);
}

/* ------------------------------------------------------------------ */
/* 块层:按**字节偏移**读写(M4A-3/B5)                                  */
/* ------------------------------------------------------------------ */

/*
 * 这一对是上游 `driver/device.cpp:318/415` 的**端口原生版**:
 * 接口、返回值和那套 LBA/偏移算术照上游,但**去掉了 x86 的 DMA bounce 机制**。
 *
 * ## 为什么去掉 bounce(这是本笔的核心决定)
 *
 * 上游那套 bounce + direct-span(`blk_acquire_bounce`/`blk_direct_span_bytes`)
 * 存在的理由是"**目的缓冲区可能不在 DMA 安全窗口里**" —— 那是 x86 HHDM 布局
 * 下的问题:用户缓冲区可能是另一套页表映射的物理页,设备不能直接 DMA 进去,
 * 于是先搬进一块**保证物理连续**的中转缓冲。M4A-3/B5 侦察时量过:它拖着
 * **8 个 x86 页层符号**(`page_map_range`/`translate_address`/`free_frames`…),
 * 而那些在 ARM 上**不是欠账而是架构差异**(ARM 用 palloc/vmap,没有那个窗口)。
 *
 * ⇒ ARM 侧**目的地址恒为内核地址**(今天没有用户态;M7 到来时这里要重新审),
 *   所以三段式(头 / 整扇区 / 尾)就够:
 *     头:偏移不在扇区边界 ⇒ 先读**一扇区**到中转,拷出需要的那几个字节;
 *     中:整扇区 ⇒ **直接**读进/写出目的缓冲,按 `SECTORS_ONCE` 分块;
 *     尾:不足一扇区 ⇒ 读出那一扇区再拷需要的部分(写方向还要写回)。
 *
 * ## ⚠ 三条与上游的**已知差异**(都写在这里而不是藏着)
 *
 *   1. **去掉了 `blk_user_buffer_valid()`** —— 那是 x86 的"这块地址映射了吗"检查。
 *      ARM 侧今天没有用户地址空间可查,所以改为**只拒绝 NULL**(并顺手做了
 *      `vdiskid` 的边界检查)。⚠ M7 有用户态之后,这里必须补一个等价物,
 *      否则"用户传进来的野指针"会直接进设备回调。
 *   2. **中转缓冲是 `static`**(一块,不重入)。上游是每设备一份、按需分配。
 *      今天块层只在启动自检与内核上下文里被调用(单线程),所以一块够用;
 *      ⚠ 一旦有并发调用者(比如多个线程同时读盘),这里要改成加锁或每调用一份。
 *   3. **中间整扇区按 `SECTORS_ONCE` 分块**,而不是上游那种"一次给到底"。
 *      理由:单次回调的传输量应该有上界 —— 我们没有 x86 那个 bounce 窗口
 *      去隐式限制它,而 SDHCI 一类的控制器对一次传输的长度是有限制的。
 */

#define BLK_SCRATCH_BYTES 4096u

static u8  g_blk_scratch[BLK_SCRATCH_BYTES];
static u32 g_blk_calls; /* 块层被调用过多少次(自检用:今天应当是 0 —— 还没有块设备)*/

u32 blk_device_calls(void)
{
    return g_blk_calls;
}

size_t blk_device_read(int drive, void *buffer, size_t offset, size_t length)
{
    device_t device;
    u64      sector_size;
    u64      sector;
    u64      offset_in_block;
    u64      remaining;
    u64      total = 0;
    u8      *dest  = (u8 *)buffer;

    if (!have_vdisk(drive) || length == 0u) {
        return 0u; /* 与上游一致:没有这个盘 / 长度为 0 ⇒ 0 */
    }
    if (buffer == NULL) {
        return (size_t)-1; /* ARM 侧没有 x86 那个"地址有效吗"检查,至少挡住 NULL */
    }

    device = device_ctl[drive];

    if (device.type == DEVICE_STREAM) {
        if (device.read == NULL) {
            return (size_t)-1;
        }
        /* ⚠ 流设备把这两个参数**原样**转交(上游也是这么做的):
           对块设备它们是 (扇区数, LBA),对流设备它们由驱动自己解释 */
        return device.read(drive, (uint8_t *)buffer, length, offset);
    }

    if (device.sector_size == 0u || device.read == NULL ||
        device.sector_size > BLK_SCRATCH_BYTES) {
        return (size_t)-1;
    }

    sector_size     = device.sector_size;
    sector          = (u64)offset / sector_size;
    offset_in_block = (u64)offset % sector_size;
    remaining       = (u64)length;

    if (offset_in_block != 0u) {
        u64 head = sector_size - offset_in_block;

        if (head > remaining) {
            head = remaining;
        }
        if (device.read(device.vdiskid, g_blk_scratch, 1u, sector) != 1u) {
            return (size_t)-1;
        }
        memcpy(dest, g_blk_scratch + offset_in_block, (size_t)head);
        dest += head;
        remaining -= head;
        total += head;
        sector++;
    }

    /* 中间:整扇区直接进目的缓冲(ARM 上不需要中转),按 SECTORS_ONCE 分块 */
    while (remaining >= sector_size) {
        u64 secs = remaining / sector_size;

        if (secs > (u64)SECTORS_ONCE) {
            secs = (u64)SECTORS_ONCE;
        }
        if (device.read(device.vdiskid, dest, (size_t)secs, (size_t)sector) != (size_t)secs) {
            return (size_t)-1;
        }
        dest += secs * sector_size;
        remaining -= secs * sector_size;
        total += secs * sector_size;
        sector += secs;
    }

    if (remaining > 0u) {
        if (device.read(device.vdiskid, g_blk_scratch, 1u, sector) != 1u) {
            return (size_t)-1;
        }
        memcpy(dest, g_blk_scratch, (size_t)remaining);
        total += remaining;
    }

    return (size_t)total;
}

size_t blk_device_write(device_t device, const void *buffer, size_t offset, size_t length)
{
    u64        sector_size;
    u64        sector;
    u64        offset_in_block;
    u64        remaining;
    u64        total = 0;
    const u8  *src   = (const u8 *)buffer;

    g_blk_calls++;

    if (length == 0u) {
        return 0u;
    }
    if (buffer == NULL || device.vdiskid >= DEVICE_TABLE_SIZE) {
        return (size_t)-1;
    }

    if (device.type == DEVICE_STREAM) {
        if (device.write == NULL) {
            return (size_t)-1;
        }
        return device.write(device.vdiskid, (uint8_t *)buffer, length, offset);
    }

    if (device.sector_size == 0u || device.write == NULL ||
        device.sector_size > BLK_SCRATCH_BYTES) {
        return (size_t)-1;
    }

    sector_size     = device.sector_size;
    sector          = (u64)offset / sector_size;
    offset_in_block = (u64)offset % sector_size;
    remaining       = (u64)length;

    /*
     * 头:非对齐 ⇒ 这是**读-改-写**(只改那几个字节,其余保持盘上原样)。
     * ⚠ 上游在这里显式要求 `device.read != NULL` —— 没有读能力的设备
     *   做不了非对齐写(要么拒绝,要么把整扇区当垃圾写回去,那更糟)。
     */
    if (offset_in_block != 0u) {
        u64 head = sector_size - offset_in_block;

        if (head > remaining) {
            head = remaining;
        }
        if (device.read == NULL) {
            return (size_t)-1;
        }
        if (device.read(device.vdiskid, g_blk_scratch, 1u, sector) != 1u) {
            return (size_t)-1;
        }
        memcpy(g_blk_scratch + offset_in_block, src, (size_t)head);
        if (device.write(device.vdiskid, g_blk_scratch, 1u, sector) != 1u) {
            return (size_t)-1;
        }
        src += head;
        remaining -= head;
        total += head;
        sector++;
    }

    while (remaining >= sector_size) {
        u64 secs = remaining / sector_size;

        if (secs > (u64)SECTORS_ONCE) {
            secs = (u64)SECTORS_ONCE;
        }
        if (device.write(device.vdiskid, (uint8_t *)src, (size_t)secs, (size_t)sector) != (size_t)secs) {
            return (size_t)-1;
        }
        src += secs * sector_size;
        remaining -= secs * sector_size;
        total += secs * sector_size;
        sector += secs;
    }

    /* 尾:同样读-改-写 */
    if (remaining > 0u) {
        if (device.read == NULL) {
            return (size_t)-1;
        }
        if (device.read(device.vdiskid, g_blk_scratch, 1u, sector) != 1u) {
            return (size_t)-1;
        }
        memcpy(g_blk_scratch, src, (size_t)remaining);
        if (device.write(device.vdiskid, g_blk_scratch, 1u, sector) != 1u) {
            return (size_t)-1;
        }
        total += remaining;
    }

    return (size_t)total;
}
