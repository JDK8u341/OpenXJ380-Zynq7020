/*
 * id 分配器 —— `kernel/id_alloc.cpp`(57 行)的 C 版,逐行对应。
 *
 * 为什么要有一份副本、以及它什么时候该被删掉:见 <arch/id_alloc.h>。
 * 改动这份之前请先读那一节 —— 两份实现的行为必须一致,
 * tests/test_arm32_id_alloc.py 把语义逐条钉住了。
 */

#include <arch/id_alloc.h>

id_allocator_t *id_allocator_create(uint32_t max_ids)
{
    id_allocator_t *alloc;
    uint32_t        bitmap_size;

    if (max_ids == 0u) {
        return NULL;
    }

    alloc = (id_allocator_t *)malloc(sizeof(id_allocator_t));
    if (alloc == NULL) {
        return NULL;
    }

    bitmap_size = (max_ids + BITS_PER_WORD - 1u) / BITS_PER_WORD;

    alloc->bitmap = (uint32_t *)calloc(bitmap_size, sizeof(uint32_t));
    if (alloc->bitmap == NULL) {
        free((void *)alloc);
        return NULL;
    }

    alloc->max_ids    = max_ids;
    alloc->free_count = max_ids;
    alloc->next_id    = 0u;

    return alloc;
}

int32_t id_alloc(id_allocator_t *allocator)
{
    uint32_t start;
    uint32_t id;

    if (allocator == NULL || allocator->free_count == 0u) {
        return -1;
    }

    start = allocator->next_id;
    id    = start;

    do {
        uint32_t word_index = id / BITS_PER_WORD;
        uint32_t bit_index  = id % BITS_PER_WORD;
        uint32_t mask       = 1u << bit_index;

        if ((allocator->bitmap[word_index] & mask) == 0u) {
            allocator->bitmap[word_index] |= mask;
            allocator->free_count--;
            allocator->next_id = (id + 1u) % allocator->max_ids;
            return (int32_t)id;
        }

        id = (id + 1u) % allocator->max_ids;
    } while (id != start);

    return -1;
}

bool id_free(id_allocator_t *allocator, uint32_t id)
{
    uint32_t word_index;
    uint32_t bit_index;
    uint32_t mask;

    if (allocator == NULL || id >= allocator->max_ids) {
        return false;
    }

    word_index = id / BITS_PER_WORD;
    bit_index  = id % BITS_PER_WORD;
    mask       = 1u << bit_index;

    if ((allocator->bitmap[word_index] & mask) != 0u) {
        allocator->bitmap[word_index] &= ~mask;
        allocator->free_count++;
        return true;
    }

    return false;
}
