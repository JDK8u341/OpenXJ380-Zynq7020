#pragma once

#define SECTORS_ONCE 8

/*
 * 平台中立重构(2026-09-18):这一行原先是 `#include "proto.hpp"`。
 *
 * 这是**最后一处**把整个 x86 内核主干拖进文件系统接口链的过度包含 ——
 * `proto.hpp` 拉 `<ps2/keyboard.h>`、`<efi/fbc.h>`、mm 下的头文件、
 * `<smp/smp.h>`,还在里面留了一个 `static inline uint64_t rdtsc();`
 * 的**空声明**(GCC 报 `-Werror=unused-function`)。
 *
 * 本文件真正用到的基础类型只有 size_t / uint8_t / uint64_t / bool / errno_t
 * (逐个查过:唯一的其它复合类型 `struct vecbuf` 来自下面那个 vbuffer.h),
 * 而它们全在 `<krlibc.h>`(它再带 `<stdint.h>`)里。
 *
 * 为什么必须改:上游 `driver/fs/vfs/vfs.cpp` 包含本文件,而 VFS 是要在
 * **两个架构上共用**的 —— 它不该因为"设备表里有个 uint8_t"就把键盘和
 * 显存的头拖进来。
 *
 * 按计划 §7.2,上游文件只在平台中立重构时改动、每处都有理由。
 */
#include "krlibc.h" /* size_t / uint8_t / uint64_t / bool / errno_t */
#include "vbuffer.h"
#include <fs/vfs/vfs.h>

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
    size_t        size;        // 大小
    size_t        sector_size; // 扇区大小
    device_flag_t type;
    char          drive_name[50];
    size_t        vdiskid; // 设备号
    char         *path;    // 设备注册路径
    uint64_t      max_size;
} device_t; // 设备句柄

size_t disk_size(int drive);
int    device_manager_init();

/**
 * 注册一个设备 (会被自动映射进 devfs)
 * @param vd 设备
 * @param path 注册路径(为NULL注册进 /dev 根)
 * @return 注册编号
 */
int regist_device(const char *path, device_t vd);

/**
 * 卸载一个设备 (devfs也会删除设备节点)
 * @param vdiskid 设备ID
 */
void delete_device(int vdiskid);

/**
 * 根据指定设备id查找设备
 * @param id 设备id
 * @return 为 NULL 则找不到设备
 */
device_t *get_device(size_t id);

errno_t devfs_register(const char *path, size_t id);
errno_t devfs_delete(const char *path);

bool   have_vdisk(int drive);
size_t device_read(size_t lba, size_t number, void *buffer, int drive);
size_t device_write(size_t lba, size_t number, const void *buffer, int drive);
size_t blk_device_read(int drive, void *buffer, size_t offset, size_t length);
size_t blk_device_write(device_t device, const void *buffer, size_t offset, size_t length);
void  *device_mmap(int drive, void *addr, size_t len);
