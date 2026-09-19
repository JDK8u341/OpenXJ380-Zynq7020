#pragma once

/*
 * id 分配器 —— **C 侧的声明**。定义来自上游 `kernel/id_alloc.cpp`。
 *
 * ====================================================================
 * 为什么这里要有一份"声明副本",而不是直接 include 上游的头
 * ====================================================================
 *
 * 上游的 `include/id_alloc.h` 是 **C++ 专属**的:它 `#include "krlibc.h"`,
 * 而那份 krlibc.h 拉进来的东西在 C 里根本编不过(实测,逐条):
 *
 *   include/stdint.h:20   `typedef char int8_t` 与移植侧的 `signed char` 冲突
 *                         (ARM 上 char 默认**无符号** —— 上游那条在 ARM 上本身就是错的)
 *   include/stdint.h:38   `#define NULL 0` 与移植侧的 `((void *)0)` 冲突
 *   include/krlibc.h:22   `typedef typeof(nullptr) nullptr_t` —— C++ 专属语法
 *   include/krlibc.h:217  `static memmove` 与移植侧的非 static 声明冲突
 *
 * ⇒ 于是本项目立了一条边界规矩(见 tools/gen_ninja.py 的
 *   ARM32_UPSTREAM_INCLUDE 旁边那段):
 *
 *     **移植侧的 C 文件看不到上游头文件;上游的 C++ 文件看不到移植侧头文件。**
 *     两边唯一的交界 = "上游 C++ 导出 C 链接符号 + 移植侧给出 C 声明"。
 *
 * 这条交界不是靠自觉维持的:结构体与三个原型的**逐字段比对**在
 * `tests/test_arm32_device.py` 里(与 device_t 对着上游 include/device.h
 * 的比对同一套路)。上游改了签名而这里没跟上,测试会报。
 *
 * ====================================================================
 * 链接约定
 * ====================================================================
 *
 * `include/id_alloc.h` 已经加了 `extern "C"`(平台中立的小改动,理由写在
 * 那份文件里),所以 `kernel/id_alloc.cpp` 编出来的符号是**未修饰**的
 * `id_alloc` / `id_free` / `id_allocator_create`,C 这边能直接链上。
 */

#include <krlibc.h>

typedef struct
{
    uint32_t *bitmap;     /* 位图数组 */
    uint32_t  max_ids;    /* 最大 ID 数量 */
    uint32_t  free_count; /* 空闲 ID 计数 */
    uint32_t  next_id;    /* 下一个尝试分配的 ID */
} id_allocator_t;

/* 失败返回 NULL */
id_allocator_t *id_allocator_create(uint32_t max_ids);

/* 表满返回 -1 */
int32_t id_alloc(id_allocator_t *allocator);

/* 释放一个已占用的 id;越界或本来就空闲 ⇒ false */
bool id_free(id_allocator_t *allocator, uint32_t id);
