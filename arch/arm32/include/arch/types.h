#pragma once

/*
 * ARMv7-A 基础类型定义
 *
 * 独占使用:不代表完整 stdint.h,只提供内核需要的定宽类型。
 * 与 x86_64 侧的差异:ARM 是 32 位,long/指针宽度为 4。
 */

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

typedef u8  bool;
#define true  1
#define false 0

#define NULL ((void *)0)

/* 常用边界 */
#define U32_MAX 0xFFFFFFFFu
#define U64_MAX 0xFFFFFFFFFFFFFFFFull
