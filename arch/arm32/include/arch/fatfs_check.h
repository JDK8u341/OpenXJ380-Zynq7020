#pragma once

/*
 * FATFS 验收判据(M4A-1.4)
 *
 * ====================================================================
 * 验的是什么
 * ====================================================================
 *
 * 计划 §4.6 里 M4A-1.4 的验收写法是"**挂载 + 读写文件 + 与主机侧比对**"。
 * 三条对应三种不同强度的证据:
 *
 *   1. **挂载 + 读写**:在 RAM 盘上格式化一个 FAT16 卷、挂到 `/mnt`、
 *      建文件、写 32 字节、读回来逐字节相同(与 tmpfs 那条同一个套路);
 *   2. **卷真的是 FAT**:`/mnt` 的 `fsid` 必须与 `/`、`/tmp` **两两不同**
 *      —— 三个不同的文件系统实例(根 / tmpfs / fatfs)同时在场;
 *   3. ★ **与主机侧比对** ★:把卷的**引导扇区原样**打印出来,由宿主机用
 *      **自己的**解析去核(BPB 的 bytes/sector、total sectors 是否等于
 *      我们设的 8MB、FAT16 类型串、0x55AA 结束标志)。
 *      板子自说自话不算数 —— 这一条是"让另一个系统去读同一个产物"。
 *
 * ====================================================================
 * RAM 盘为什么是 tmpfs 文件、为什么是 FAT16
 * ====================================================================
 *
 * 两条理由都写在 `arch/arm32/src/upstream_api.cpp` 的 FATFS 那一节,
 * 一句话版:
 *   - **tmpfs 文件**:这个 OS 的块层**就是 VFS**(`diskio.cpp` 把
 *     `disk_read` 落成 `vfs_read(node, buff, sector*512, count*512)`),
 *     没有独立的块设备 API。tmpfs 文件本来就能按偏移读写(M4A-1.2b 已验)
 *     ⇒ 零新增块驱动代码,且走的正是决策 3 说的那条同路径;
 *   - **`FM_FAT`(FAT12/16 族)**:上游的格式化辅助函数写死 FAT32,
 *     而 FAT32 要求卷 ≥ ~34MB,内核堆只有正好 32MB。
 *
 * ⚠ **子类型由 FatFs 自己按簇数选,不由我们指定**。8MB 的卷上实测选到的是
 *   **FAT12**:8 扇区/簇(4KB)⇒ 2043 簇 ⇒ FAT 占 7 扇区
 *   (2043 簇的 FAT12 需要 ceil(2043*1.5/512) = 6 扇区、FAT16 需要 8 扇区)。
 *   所以板子这一侧**只判族标签**(`'FAT'` 开头,这是 `f_mkfs` 真正写下的),
 *   **子类型交给宿主机用算术独立推**(`tmp-test/verify_fat_bootsector.py`)——
 *   板子那边写死"必须是 FAT16"曾经报过一次假 FAIL(见坑表)。
 *
 * ⚠ 这两个都是**本阶段的取舍**,不是终局:M4A-1.3 的真实块设备(SD/arasan)
 *   接上之后,RAM 盘会换成真实卷,`diskio` 一行都不用改。
 */

#include <arch/types.h>

#define FATFS_CHECK_VOLUME_BYTES (8u * 1024u * 1024u)

typedef struct
{
    int           init;             /* 起搏(mutex + 注册 fatfs) */
    int           ramdisk;          /* 建 RAM 盘 + 撑到 8MB(0 = 成功)*/
    unsigned long ramdisk_size;     /* **读回来**的 node->size(证据,不是假设)*/
    int           format;           /* `f_mkfs()` 的返回值(0 = FR_OK)*/
    int           mount;            /* 挂到 /mnt */
    int           create;           /* 在 /mnt 下建文件 */
    int           write;            /* 写入字节数 */
    int           read;             /* 读回字节数 */
    int           roundtrip;        /* 1 = 逐字节相同 */
    int           list_after;       /* /mnt 下的子项数 */
    int           fsid_root;        /* 三个 fsid 必须两两不同 */
    int           fsid_tmp;
    int           fsid_mnt;
    int           x86_calls;        /* 必须 0:x86 预读路径(alloc_frames/phys_to_virt)没被走到 */
    int           bs_len;           /* 引导扇区读回字节数 */
    int           bs_sig_ok;        /* 1 = 偏移 510/511 是 0x55/0xAA */
    int           bs_bytes_per_sector;
    unsigned long bs_total_sectors; /* BPB 里的总扇区数(期望 = 8MB/512 = 16384)*/
    int           bs_fat_family_ok; /* 1 = 偏移 54 的标签以 "FAT" 开头 */
} fatfs_check_t;

/*
 * 跑一次 FATFS 起搏 + 验收。**幂等**(第二次直接返回上次结果)。
 * ⚠ 必须在**堆可用**且 **VFS 已起搏**(`vfs_check_run()`)之后调用。
 */
const fatfs_check_t *fatfs_check_run(void);

/* 引导扇区的原始 512 字节(验收 3 要用它,宿主机侧脚本读串口日志解析)*/
const u8 *fatfs_check_bootsector(void);
