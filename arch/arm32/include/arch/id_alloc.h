#pragma once

/*
 * id 分配器 —— 上游 `include/id_alloc.h` + `kernel/id_alloc.cpp` 的 C 版
 *
 * ====================================================================
 * 为什么是"照抄一份"而不是"在 device.c 里写个私有位图"
 * ====================================================================
 *
 * 规程是**源 OS 优先**(计划 §0.5.9):"我觉得这样更省事"不是理由。
 * 上游的设备管理器用的就是这个分配器(`device.cpp` 的 `dev_allocator`),
 * 而它**不止一个用户** —— `kernel/dlinker.cpp` 用它分配模块 id。
 * 两份语义不同的 id 分配器并存,将来合流时要对账的正是这种东西。
 *
 * ⚠ **这是一份有意的临时副本**,不是长期方案:
 *   上游那份是 `.cpp`,编进 ARM 图需要 C++ 规则;而 1.1b 定的是"先走 C"
 *   (合流决策里"移植侧写骨架"),C++ 规则的前置是 M4A-1.2(搬上游 VFS)。
 *   ⇒ 那条规则到位后,这份应当**删掉**,直接共用 `kernel/id_alloc.cpp`。
 *   本项目的先例:`include/errno.h` 也是复制一份 + 逐值比对测试。
 *
 * ====================================================================
 * 语义(与上游逐条一致)
 * ====================================================================
 *
 *   O(1) 位图分配;`next_id` 轮转 ⇒ 刚释放的 id 不会被立刻重用
 *   free_count == 0 时 `id_alloc` 直接返回 -1,不做全表扫描
 *   `id_free` 对未占用的 id 返回 false(不静默成功)
 */

#include <krlibc.h>

#define BITS_PER_WORD ((u32)(sizeof(uint32_t) * 8u))

typedef struct
{
    uint32_t *bitmap;     /* 位图数组 */
    uint32_t  max_ids;    /* 最大 ID 数量 */
    uint32_t  free_count; /* 空闲 ID 计数 */
    uint32_t  next_id;    /* 下一个尝试分配的 ID */
} id_allocator_t;

/* 失败返回 NULL(上游对 malloc 失败没有额外处理,这里保持一致) */
id_allocator_t *id_allocator_create(uint32_t max_ids);

/* 表满返回 -1 */
int32_t id_alloc(id_allocator_t *allocator);

/* 释放一个已占用的 id;id 越界或本来就空闲 ⇒ false */
bool id_free(id_allocator_t *allocator, uint32_t id);
