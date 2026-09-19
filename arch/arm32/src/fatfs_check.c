/*
 * FATFS 验收(M4A-1.4)
 *
 * 判据为什么是这几条、RAM 盘与 FAT16 这两个取舍的理由、
 * 以及"与主机侧比对"要怎么做,都写在 <arch/fatfs_check.h> 的文件头上。
 * 这里只写实现上的取舍。
 */

#include <arch/fatfs_check.h>

#include <arch/console.h>
#include <krlibc.h>

/* ------------------------------------------------------------------ */
/* 落地层转发(手写声明;理由见 upstream_api.cpp 的 FATFS 那一节)      */
/* ------------------------------------------------------------------ */

extern int           arm_fatfs_init(void);
extern int           arm_fatfs_make_ramdisk(const char *path, unsigned int bytes);
extern unsigned long arm_fatfs_node_size(const char *path);
extern int           arm_fatfs_format(const char *path);
extern int           arm_fatfs_mount(const char *src, const char *mnt);
extern int           arm_fatfs_read_raw(const char *path, void *buf, unsigned int len);
extern unsigned int  arm_fatfs_x86_path_calls(void);

/* VFS 那几个转发在 M4A-1.2b 就已经有了(vfs_check.c),这里复用同一批 */
extern int arm_vfs_create(const char *path);
extern int arm_vfs_write(const char *path, const void *data, unsigned int len);
extern int arm_vfs_read(const char *path, void *buf, unsigned int len);
extern int arm_vfs_child_count(const char *path);
extern int arm_vfs_fsid(const char *path);

/* ------------------------------------------------------------------ */

#define RAMDISK_PATH "/tmp/ramdisk.img"
#define MOUNT_POINT  "/mnt"
#define FILE_PATH    "/mnt/xj380.txt"

/* 载荷长度与 tmpfs 那条保持一致(32 = 一个指针宽、非全 0 模式)*/
#define FATFS_PAYLOAD_LEN 32

/* 引导扇区固定 512 字节(FATFS 的 FF_MIN_SS)*/
#define FATFS_BOOTSECTOR_LEN 512

/* BPB 里我们关心的字段偏移(FAT 规范)*/
#define BPB_BYTES_PER_SECTOR 11u
#define BPB_TOTAL_SECTORS16  19u
#define BPB_TOTAL_SECTORS32  32u
#define BPB_FS_TYPE          54u
#define BPB_SIGNATURE        510u

static fatfs_check_t g_result;
static u8            g_payload[FATFS_PAYLOAD_LEN];
static u8            g_readback[FATFS_PAYLOAD_LEN];
static u8            g_bootsector[FATFS_BOOTSECTOR_LEN];
static bool          g_ran;

static u32 rd16(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8);
}

static u32 rd32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

const u8 *fatfs_check_bootsector(void)
{
    return g_bootsector;
}

const fatfs_check_t *fatfs_check_run(void)
{
    int i;

    if (g_ran) {
        return &g_result;
    }
    g_ran = true;

    memset(&g_result, 0, sizeof(g_result));
    g_result.fsid_root = -1;
    g_result.fsid_tmp  = -1;
    g_result.fsid_mnt  = -1;
    g_result.list_after = -1;

    /* ① 起搏:注册 fatfs + 建那把操作锁 */
    g_result.init = arm_fatfs_init();

    /*
     * ② RAM 盘:建文件 + 撑到 8MB。
     *
     * ⚠ 判据取的是**读回来的** `node->size`(`ramdisk_size`),不是"我们要求了 8MB"
     *   —— `vfs_resize()` 会把 resize 回调的失败吞掉(回调是 `void`),
     *   所以"要求了"与"生效了"是两件事,这里只认后者。
     */
    g_result.ramdisk      = arm_fatfs_make_ramdisk(RAMDISK_PATH, FATFS_CHECK_VOLUME_BYTES);
    g_result.ramdisk_size = arm_fatfs_node_size(RAMDISK_PATH);

    /* ③ 格式化(FAT16)。0 = FR_OK */
    g_result.format = arm_fatfs_format(RAMDISK_PATH);

    /* ④ 挂载到 /mnt */
    g_result.mount = arm_fatfs_mount(RAMDISK_PATH, MOUNT_POINT);

    /* ⑤ 建文件 + 写 + 读回比对(与 tmpfs 那条同一个套路) */
    g_result.create = arm_vfs_create(FILE_PATH);

    for (i = 0; i < FATFS_PAYLOAD_LEN; i++) {
        g_payload[i] = (u8)((i * 11u) + 5u);
    }
    memset(g_readback, 0xAA, sizeof(g_readback));

    g_result.write = arm_vfs_write(FILE_PATH, g_payload, FATFS_PAYLOAD_LEN);
    g_result.read  = arm_vfs_read(FILE_PATH, g_readback, FATFS_PAYLOAD_LEN);

    g_result.roundtrip = 0;
    if (g_result.read == FATFS_PAYLOAD_LEN &&
        memcmp(g_payload, g_readback, FATFS_PAYLOAD_LEN) == 0) {
        g_result.roundtrip = 1;
    }

    /* ⑥ 列目录 + 三个 fsid(根 / tmpfs / fatfs 必须两两不同)*/
    g_result.list_after = arm_vfs_child_count(MOUNT_POINT);
    g_result.fsid_root  = arm_vfs_fsid("/");
    g_result.fsid_tmp   = arm_vfs_fsid("/tmp");
    g_result.fsid_mnt   = arm_vfs_fsid(MOUNT_POINT);

    /* ⑦ x86 预读路径的次数。**必须 0** —— 这是"那条路没跑"的证据 */
    g_result.x86_calls = (int)arm_fatfs_x86_path_calls();

    /*
     * ⑧ ★ 与主机侧比对的那一半:把引导扇区原样读出来并解析。
     *
     * 板子这里只做**最便宜的**自检(签名 + bytes/sector + 总扇区数 + 类型串),
     * 真正的"另一个系统去读它"由 `tmp-test/verify_fat_bootsector.py` 做:
     * 它拿串口日志里那串十六进制,用自己的解析重新核一遍。
     */
    g_result.bs_len = arm_fatfs_read_raw(RAMDISK_PATH, g_bootsector, FATFS_BOOTSECTOR_LEN);
    if (g_result.bs_len == FATFS_BOOTSECTOR_LEN) {
        if (g_bootsector[BPB_SIGNATURE] == 0x55u && g_bootsector[BPB_SIGNATURE + 1u] == 0xAAu) {
            g_result.bs_sig_ok = 1;
        }
        g_result.bs_bytes_per_sector = (int)rd16(&g_bootsector[BPB_BYTES_PER_SECTOR]);

        {
            u32 total16 = rd16(&g_bootsector[BPB_TOTAL_SECTORS16]);
            u32 total32 = rd32(&g_bootsector[BPB_TOTAL_SECTORS32]);

            g_result.bs_total_sectors = (total16 != 0u) ? (unsigned long)total16
                                                        : (unsigned long)total32;
        }

        /*
         * 族标签。⚠ **只判族,不判子类型**:
         *   `f_mkfs()` 在 FAT12/16 卷上写下的就是 "FAT" 加空格(实测偏移 54
         *   是 `'FAT     '`),而**子类型是它自己按簇数选的**(8MB 的卷上
         *   实测选到 FAT12)。第一版这里写死"必须是 FAT16",于是对一个
         *   **正确**的卷报了假 FAIL —— 判据自己错了,不是被测代码错。
         *   子类型由宿主机用算术独立推(`tmp-test/verify_fat_bootsector.py`),
         *   那才是"与主机侧比对"的意义。
         */
        if (g_bootsector[BPB_FS_TYPE + 0u] == 'F' && g_bootsector[BPB_FS_TYPE + 1u] == 'A' &&
            g_bootsector[BPB_FS_TYPE + 2u] == 'T') {
            g_result.bs_fat_family_ok = 1;
        }
    }

    console_printf(" FATFS check : format=%d mount=%d roundtrip=%s list=%d size=%lu\n",
                   g_result.format, g_result.mount,
                   g_result.roundtrip ? "PASS" : "FAIL",
                   g_result.list_after, g_result.ramdisk_size);

    /*
     * ★ 引导扇区的**原始字节**打给宿主机。
     *
     * ⚠ 这一行必须打在这里(报告区间**之外**):`verify_board.py` 对报告区间里
     *   的非 CHECK 行会报警告 —— 那是有意的护栏(见 verify_board.py 的说明)。
     *   本函数在 `selftest_begin()` 之前跑,所以这里是安全的。
     *
     * 512 字节 = 1024 个十六进制字符 ≈ 1.1KB,9600 波特下约 1.2 秒。
     */
    console_printf("FATBS:");
    for (i = 0; i < FATFS_BOOTSECTOR_LEN; i++) {
        console_printf("%02x", g_bootsector[i]);
    }
    console_printf("\n");

    return &g_result;
}
