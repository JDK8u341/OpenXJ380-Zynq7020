// 版权所有©XINGJI Studios 2017-2026 保留所有权利。
// XJ380 类型定义头文件
#ifndef _STDINT_H_
#define _STDINT_H_

// 规范类型
// 无符号整型
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef uint64_t           u64;
typedef uint16_t           u16;
typedef uint32_t           u32;
typedef uint8_t            u8;
#ifdef __UINTPTR_TYPE__
typedef __UINTPTR_TYPE__ uintptr_t;
#endif
// 有符号整型
/*
 * 平台中立修复(2026-09-18):`int8_t` 原先是 `typedef char int8_t;`。
 *
 * ★ 那在 ARM 上是**错的**:ARM EABI 的 `char` 默认**无符号**,于是
 *   `int8_t` 会变成无符号类型 —— 一个叫 int 的类型其实不 signed,
 *   而它的失败方式是"负数字符串比较/差值算错",不会编译报错。
 *   x86 的 `char` 默认有符号,所以这条 bug 在 x86 上一直看不出来。
 *
 * 改成显式 `signed char` 之后:
 *   - x86 上 `signed char` 与 `char` 是**同一个类型**,行为零变化;
 *   - ARM 上才是它字面上承诺的东西;
 *   - 而且与移植侧 `arch/arm32/include/arch/types.h` 的
 *     `typedef signed char int8_t;` 一致 —— 两边同处一个 TU 时不再报
 *     "conflicting types for 'int8_t'"(实测撞过)。
 */
typedef signed char int8_t;
typedef short     int16_t;
typedef int       int32_t;
typedef long long int64_t;
typedef char      CHAR8;
#ifdef __INTPTR_TYPE__
typedef __INTPTR_TYPE__ intptr_t;
#endif
// 浮点型
typedef float  float32_t;
typedef double float64_t;

// 自定义类型
// 其他的在efi.h里
// 浮点型
typedef float32_t f32;
typedef float64_t f64;

/*
 * `NULL` 加守卫(平台中立修复,2026-09-18)。
 *
 * 移植侧的 `arch/arm32/include/arch/types.h` 也会定义 `NULL`,用的是
 * `((void *)0)` 而不是 `0` —— 两个宏字面不同,谁后到谁就触发
 * "warning: 'NULL' redefined"(-Werror 下直接是错误)。
 *
 * 加上 `#ifndef` 之后**先到先得**:单个架构内仍然是原来那个定义,
 * 行为零变化;两边同处一个 TU 时也不会再冲突。移植侧那一份本来就有守卫。
 */
#ifndef NULL
#    define NULL 0
#endif

#ifndef __cplusplus
#    define bool  _Bool
#    define true  1
#    define false 0
#endif

typedef __SIZE_TYPE__ size_t; // compatible to ELF toolchain
typedef int           ssize_t;

typedef union ptr_cast
{
    void     *ptr;
    uintptr_t val;
} ptr_cast_t;

#endif