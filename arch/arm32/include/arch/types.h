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

/*
 * 固定宽度的那套别名(stdint 的名字)。
 *
 * 为什么 ARM 侧也要有:上游的头文件到处写的是 `uint8_t` / `uint64_t`
 * (include/device.h、include/mm 下、driver/fs 下),而移植侧一直只用 u8/u32。
 * 两套拼写混着用,将来共享源码时每一处都要改 —— 那是纯粹的摩擦。
 *
 * 为什么不直接 #include <stdint.h>:ARM 图是 -nostdinc,系统头文件不可见;
 * 而仓库根目录的 include/stdint.h 属于上游头文件树,把它拉进 ARM 的
 * -I 会让**所有** ARM 源文件都暴露在上游头文件下(实测过一个坑:
 * include/id_alloc.h 用引号写 #include "krlibc.h",会命中上游那份 219 行的)。
 * ⇒ 显式在这里给别名,不开新的 include 路径。
 *
 * 与 hosted 分支的 <stdint.h> 定义重复也不要紧:C11 允许相同的 typedef 重复。
 */

typedef unsigned char      uint8_t;
typedef signed char        int8_t;
typedef unsigned short     uint16_t;
typedef signed short       int16_t;
typedef unsigned int       uint32_t;
typedef signed int         int32_t;
typedef unsigned long long uint64_t;
typedef signed long long   int64_t;

/*
 * bool —— 用**宏**而不是 typedef,而且必须与上游 include/stdint.h 的一致。
 *
 * ★ 为什么不能是 `typedef u8 bool`:仓库自己的 `include/stdint.h:41` 有
 *     #define bool _Bool
 *   于是同一个翻译单元里只要同时出现移植侧与上游的头文件,`bool` 就变成
 *   **两种不同的类型**(`unsigned char` vs `_Bool`),编译器报
 *     "conflicting types for 'have_vdisk'; have '_Bool(int)'"
 *   这不是偶发事件 —— 合流之后每一个混用两边的 TU 都会撞上
 *   (实测:device.c 引用上游 <id_alloc.h> 时当场就撞了)。
 *
 * 换成宏之后两边是同一件事(而且宏的重复定义只要字面相同就是合法的,
 * 上游那一条不会再冲突),`#ifndef` 让先后顺序无关。
 *
 * 语义差别只有一个:把非 0/1 的值赋给 `bool` 会被规范化成 1。
 * 全树 120 处 `bool` 用法已经扫过 —— **没有任何一处**依赖"它是个字节"
 * (赋值全是 true/false/比较/返回 bool 的函数)。`_Bool` 在 ARM EABI 上
 * 与 u8 同为 1 字节,结构体布局不变(percpu_t / heap_t 那些 sizeof 断言照旧)。
 *
 * ⚠ C++ 有**内建** bool,绝不能在这里宏掉它 —— 上游 kernel/、driver/ 全是
 *   .cpp,合流(见 docs/ZYNQ7020_INTEGRATION_PLAN.md)会用到。
 */
#    ifndef __cplusplus
#        ifndef bool
#            define bool  _Bool
#            define true  1
#            define false 0
#        endif
#    endif

#    ifndef NULL
/*
 * ⚠ C++ 里必须是 `nullptr`(或 `0`),**不能**是 `((void *)0)`。
 *
 * C++ 不允许 `void*` 隐式转换成别的指针类型,于是
 *   `list_t node = NULL;`   // list_t 是 struct list *
 * 会报 "invalid conversion from 'void*' to 'list_t' [-fpermissive]"。
 * 实测:上游 `driver/fs/vfs/tmpfs.cpp` 在 ARM 上就是这么炸的 ——
 * 而同一份文件在 x86 上是好的,因为上游 `include/stdint.h` 用的是 `#define NULL 0`。
 *
 * 这个头会被**上游的 C++ 翻译单元**看到(架构覆盖层 `upstream/cpu/lock.h`
 * 要 include <arch/cpu.h> 拿 spin_t),所以它必须对 C++ 友好。
 */
#        ifdef __cplusplus
#            define NULL nullptr
#        else
#            define NULL ((void *)0)
#        endif
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
