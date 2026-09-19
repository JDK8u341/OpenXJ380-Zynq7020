#pragma once

#define BITS_PER_WORD (sizeof(uint32_t) * 8)

#include "krlibc.h"

typedef struct {
    uint32_t *bitmap;     // 位图数组
    uint32_t  max_ids;    // 最大 ID 数量
    uint32_t  free_count; // 空闲 ID 计数
    uint32_t  next_id;    // 下一个尝试分配的 ID
} id_allocator_t;

/*
 * extern "C" —— 平台中立的小改动,理由是 C 调用方要能链到这三个函数。
 *
 * 背景:ARM 侧(arch/arm32)是 C,而本文件原先没有 extern "C",
 * 于是 kernel/id_alloc.cpp 编成 C++ 之后符号是 _Z8id_allocP14id_allocator_t,
 * C 那边根本链不上。加了这个块之后两边符号一致。
 *
 * 对 x86 侧**没有任何行为影响**:kernel/id_alloc.cpp 与 driver/device.cpp
 * 都 include 本文件,所以定义方与调用方的链接约定一起变,名字变了但配对不变。
 *
 * 本项目的规矩(计划 §7.2)是"上游文件只在平台中立重构时改动,且每处都有理由"
 * —— 这一处符合。
 */
#ifdef __cplusplus
extern "C" {
#endif

id_allocator_t *id_allocator_create(uint32_t max_ids);
int32_t         id_alloc(id_allocator_t *);
bool            id_free(id_allocator_t *, uint32_t id);

#ifdef __cplusplus
}
#endif
