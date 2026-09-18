/*
 * libc 堆接口的实现 —— 见 <arch/kmalloc.h> 里"为什么不搬上游那一套"。
 *
 * 这个文件刻意只有**转发 + 语义补齐**,没有自己的分配逻辑:
 * 分配策略全在 heap.c(它不含 MMIO/CP15,宿主可穷尽测)。
 * 这里多出来的只有三件 C 标准要求、而 heap_* 不管的事:
 *
 *   1. free(NULL) 必须是 no-op(heap_free 对 NULL 返回 BAD_POINTER);
 *   2. 堆没绑定时必须"分配失败"而不是崩;
 *   3. 释放失败必须**可观测**(free 是 void,所以走计数)。
 */

#include <arch/kmalloc.h>

#include <krlibc.h>

/* 由 kmalloc_set_heap() 绑定。静态存储 ⇒ 启动时是 0(BSS),不必显式初始化 */
static heap_t *g_heap;

/* 见 kmalloc.h:free() 没有返回值,这是失败的唯一出口 */
static u32 g_bad_free;

void kmalloc_set_heap(heap_t *h)
{
    g_heap = h;
}

u32 kmalloc_bad_free_count(void)
{
    return g_bad_free;
}

void *malloc(size_t size)
{
    if (g_heap == NULL) {
        return NULL;
    }

    return heap_alloc(g_heap, size);
}

void *calloc(size_t nmemb, size_t size)
{
    if (g_heap == NULL) {
        return NULL;
    }

    return heap_calloc(g_heap, nmemb, size);
}

void *realloc(void *ptr, size_t size)
{
    if (g_heap == NULL) {
        return NULL;
    }

    return heap_realloc(g_heap, ptr, size);
}

void free(void *ptr)
{
    /*
     * C 标准:free(NULL) 是合法的 no-op。
     * 这一条不能省 —— 上游代码里"可能为 NULL 的指针直接 free"到处都是
     * (driver/device.cpp 的 delete_device 就是那个形状),少了它每次都会
     * 记一次假错误,那个计数就没人信了。
     */
    if (ptr == NULL) {
        return;
    }

    if (g_heap == NULL) {
        /* 堆还没绑定却有人来释放 —— 这个指针不可能有合法来源 */
        g_bad_free++;
        return;
    }

    if (heap_free(g_heap, ptr) != HEAP_OK) {
        g_bad_free++;
    }
}
