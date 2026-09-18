#pragma once

/*
 * krlibc 子集 —— M4-1
 *
 * ====================================================================
 * 为什么是"子集"而不是把 krlibc 整个搬过来
 * ====================================================================
 *
 * 搬运范围不是拍脑袋定的,是**数出来的**:扫描 driver/fs + include/fs +
 * driver/device.cpp(排除 ffunicode.cpp 那张 2MB 的生成表,它不调 libc),
 * 得到 23 个函数、699 个调用点。其中 malloc/calloc/realloc/free
 * 共 316 处属于**堆**(M4-3),不算本阶段。
 *
 * ★ 2026-09-18 补记:那一组**现在补上了**(M4A-1.1a)。堆本身早在 M4-3
 *   就做完(heap.c),缺的只是"名字"这一层接线 —— 实现见 src/kmalloc.c,
 *   语义与**刻意没提供**的两个函数见 <arch/kmalloc.h>。
 *   上游文件写的 #include <mm/alloc/alloc.h> 那层路径映射还没做。
 *
 * 本阶段只做**纯函数**:不依赖堆、不依赖调度器、不看硬件。
 * 好处是它们能在宿主机上直接编译运行 —— 而这个项目的规矩是
 * "没被调用、没法验证的代码不搬"。
 *
 * ====================================================================
 * 与 x86 侧的关系(B5 的前置)
 * ====================================================================
 *
 * 头文件路径拼写刻意与旧 XJ380 一致(`<krlibc.h>`、`<errno.h>`),
 * 这样将来驱动源码可以两边共用而不必改 include。
 *
 * ⚠ 实现放在 arch/arm32/src/ 而不是仓库根的 lib/,有一个具体原因:
 *   `lib/` 已经是 **x86 的源根**(gen_ninja.py 的 root_source_roots),
 *   而 kernel/krlibc.cpp 已经定义了 memcpy 等 —— 把这份实现放进去
 *   会让 x86 链接出现**重复符号**。为了 ARM 的整洁去弄坏 x86,不划算。
 *   提升为共享实现应当是 B5 里一个单独的、可评审的步骤。
 *
 * 这些函数全部是**架构无关的纯 C11**,所以那时只是移动文件 + 改 include 路径。
 */

#include <arch/types.h>

/* ---- 内存 ---- */
void *memcpy(void *dest, const void *src, size_t n);
void *memmove(void *dest, const void *src, size_t n);
void *memset(void *dst, int val, size_t size);
int   memcmp(const void *a, const void *b, size_t size);

/*
 * ---- 堆(M4A-1.1a)----
 *
 * 拼写与旧 XJ380 的 <mm/alloc/alloc.h> 一致,这样上游文件能直接编进 ARM 图。
 * 实现在 src/kmalloc.c,分配策略全在 heap.c;语义(含 free(NULL) 与
 * 未绑定时返回 NULL)见 <arch/kmalloc.h>。
 *
 * ⚠ 上游 alloc.h 还声明了 aligned_alloc 与 usable_size —— 这两个**没有**提供,
 *   理由写在 <arch/kmalloc.h> 的末尾,别以为它们存在。
 */
void *malloc(size_t size);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void  free(void *ptr);

/* ---- 字符串 ---- */
size_t strlen(const char *str);
int    strcmp(const char *s1, const char *s2);
int    strncmp(const char *s1, const char *s2, size_t n);
char  *strcpy(char *dest, const char *src);
char  *strncpy(char *dest, const char *src, size_t n);
char  *strcat(char *dest, const char *src);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);

/*
 * strtok 是**有状态**的:第一次传串,之后传 NULL 继续切分。
 *
 * ⚠ 状态是全局的 —— 两个任务同时 strtok 会互相踩。
 *   旧 XJ380 的实现也是这样,所以这不是移植引入的问题,但必须记下来:
 *   等 M4-11 有了可睡眠原语与任务,这里应当改成 strtok_r 或按任务保存。
 *   fs 层只有 vfs.cpp 的 2 处调用,风险暂时可控。
 */
char *strtok(char *str, const char *delim);

/* ---- 字符与数字 ---- */
int isdigit(int c);
int atoi(const char *pstr);

/*
 * errno 的存储。
 *
 * ⚠ 当前是**全局变量**,不是每任务。POSIX 的 errno 是 per-thread,
 *   但在 M4-1 阶段还没有线程可谈。等 M4-6 有了 TCB 之后,
 *   这里应当换成 per-task(与 Linux 的 current->errno 同构)。
 *   已登记进 README 的「退化实现清单」。
 */
extern int errno;
