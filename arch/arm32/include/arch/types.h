#pragma once

/*
 * ARMv7-A 基础类型定义
 *
 * 这个头文件要同时服务两种编译环境,靠 __STDC_HOSTED__ 自动区分:
 *
 *   内核构建   -ffreestanding  -> __STDC_HOSTED__ == 0
 *              没有 libc,size_t/bool/NULL 等全部由本文件提供
 *
 *   宿主单测   普通编译        -> __STDC_HOSTED__ == 1
 *              直接用宿主 libc 的定义,避免重复 typedef
 *
 * 为什么必须区分:ARM 是 32 位,size_t 是 unsigned int、uintptr_t 也是
 * unsigned int;而 64 位宿主上是 unsigned long long。
 * 不做区分就会报 "typedef redefinition with different types",
 * 整个架构层都无法在宿主机上编译 —— 而这里面恰恰有值得单测的纯计算
 * (如波特率分频搜索,它曾经出过一个 7 倍偏差的 bug)。
 */

#if defined(__STDC_HOSTED__) && (__STDC_HOSTED__ == 0)

/* ---------------- freestanding:自己提供 ---------------- */

typedef unsigned char      u8;
typedef signed char        i8;
typedef unsigned short     u16;
typedef signed short       i16;
typedef unsigned int       u32;
typedef signed int         i32;
typedef unsigned long long u64;
typedef signed long long   i64;

typedef unsigned int       size_t;
typedef int                ssize_t;
typedef unsigned int       uintptr_t;
typedef int                intptr_t;

typedef u8 bool;

#    define true  1
#    define false 0

#    ifndef NULL
#        define NULL ((void *)0)
#    endif

#else

/* ---------------- hosted:借用 libc ---------------- */

#    include <stdbool.h>
#    include <stddef.h>
#    include <stdint.h>

typedef unsigned char      u8;
typedef signed char        i8;
typedef unsigned short     u16;
typedef signed short       i16;
typedef unsigned int       u32;
typedef signed int         i32;
typedef unsigned long long u64;
typedef signed long long   i64;

#endif

/* 常用边界。不受宿主环境影响,始终由本文件提供 */
#define U32_MAX 0xFFFFFFFFu
#define U64_MAX 0xFFFFFFFFFFFFFFFFull
