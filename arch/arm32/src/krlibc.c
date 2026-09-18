/*
 * krlibc 子集 —— 纯函数实现(M4-1)
 *
 * 本文件里**没有任何 MMIO、CP15 或内联汇编**,所以宿主编译器能直接编译它,
 * tests/test_arm32_krlibc.py 就是这么验证的。
 *
 * 搬运范围是数出来的(见 include/krlibc.h 的说明),不是拍脑袋定的。
 *
 * 实现上刻意选择"朴素但无歧义":
 *   - 不为了几个周期去写字长对齐的复杂版本 —— 这些函数目前还不是热点,
 *     而错一个字节的 memcpy 是最难查的一类 bug;
 *   - 边界行为按 C 标准,不"顺手加保护"。比如 strlen(NULL) 不检查 ——
 *     C 标准里它就是未定义,加个静默返回 0 只会把错误藏起来。
 */

#include <arch/errno.h>
#include <krlibc.h>

/*
 * errno 的存储。
 *
 * ⚠ 全局,不是每任务 —— 见 include/krlibc.h 的说明与 README 的退化实现清单。
 * 初值 0(EOK),这样没人赋值时读出来是"成功"而不是垃圾。
 */
int errno = EOK;

/* ------------------------------------------------------------------ */
/* 内存                                                                */
/* ------------------------------------------------------------------ */

void *memcpy(void *dest, const void *src, size_t n)
{
    unsigned char       *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    size_t               i;

    /*
     * ⚠ 标准规定 memcpy 的区间**不得重叠**。这里不做重叠检测 ——
     *   加了检测就变成 memmove,而"该用 memmove 的地方误用了 memcpy"
     *   这类 bug 会被永远掩盖。需要重叠安全时请显式用 memmove。
     */
    for (i = 0; i < n; i++) {
        d[i] = s[i];
    }

    return dest;
}

void *memmove(void *dest, const void *src, size_t n)
{
    unsigned char       *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;

    if (d == s || n == 0) {
        return dest;
    }

    /*
     * 只有"目标在源之后且区间重叠"时才需要反向拷贝。
     * 其余情况正向拷贝即可 —— 正向拷贝对不重叠区间也总是对的。
     */
    if (d < s || d >= s + n) {
        size_t i;
        for (i = 0; i < n; i++) {
            d[i] = s[i];
        }
    } else {
        size_t i = n;
        while (i > 0) {
            i--;
            d[i] = s[i];
        }
    }

    return dest;
}

void *memset(void *dst, int val, size_t size)
{
    unsigned char *d = (unsigned char *)dst;
    size_t         i;

    for (i = 0; i < size; i++) {
        d[i] = (unsigned char)val;
    }

    return dst;
}

int memcmp(const void *a, const void *b, size_t size)
{
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    size_t               i;

    for (i = 0; i < size; i++) {
        if (x[i] != y[i]) {
            return (int)x[i] - (int)y[i];
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* 字符串                                                              */
/* ------------------------------------------------------------------ */

size_t strlen(const char *str)
{
    size_t n = 0;

    while (str[n] != '\0') {
        n++;
    }

    return n;
}

int strcmp(const char *s1, const char *s2)
{
    while (*s1 != '\0' && *s1 == *s2) {
        s1++;
        s2++;
    }

    return (int)(unsigned char)*s1 - (int)(unsigned char)*s2;
}

int strncmp(const char *s1, const char *s2, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (s1[i] != s2[i]) {
            return (int)(unsigned char)s1[i] - (int)(unsigned char)s2[i];
        }
        if (s1[i] == '\0') {
            return 0;
        }
    }

    return 0;
}

char *strcpy(char *dest, const char *src)
{
    char *p = dest;

    while ((*p++ = *src++) != '\0') {
        /* 空循环 */
    }

    return dest;
}

char *strncpy(char *dest, const char *src, size_t n)
{
    size_t i = 0;

    while (i < n && src[i] != '\0') {
        dest[i] = src[i];
        i++;
    }

    /*
     * ⚠ 这是 strncpy 与"直觉"最不一样的地方,也是标准明确规定的:
     *   源串短于 n 时,**剩余部分全部补 0**;源串不短于 n 时,目标**不以 0 结尾**。
     *   漏掉补 0 会让目标尾部留着旧数据 —— 在文件系统里这意味着
     *   "读到上一个文件的内容",是很危险的一类 bug。
     */
    while (i < n) {
        dest[i] = '\0';
        i++;
    }

    return dest;
}

char *strcat(char *dest, const char *src)
{
    char *p = dest;

    while (*p != '\0') {
        p++;
    }

    while ((*p++ = *src++) != '\0') {
        /* 空循环 */
    }

    return dest;
}

char *strchr(const char *s, int c)
{
    char ch = (char)c;

    for (;;) {
        if (*s == ch) {
            return (char *)s;
        }
        if (*s == '\0') {
            return NULL;
        }
        s++;
    }
}

char *strrchr(const char *s, int c)
{
    char  ch   = (char)c;
    char *last = NULL;

    for (;;) {
        if (*s == ch) {
            last = (char *)s;
        }
        if (*s == '\0') {
            return last;
        }
        s++;
    }
}

/*
 * strdup —— 见头文件。M4A-1.1b 起被 device.c 的路径复制用到
 * (上游 device.cpp 的 regist_device 就是这个形状)。
 *
 * ⚠ 分配失败返回 NULL,而不是返回一个空串 —— 让调用方能分辨
 *   "复制成功但内容是空的"与"根本没分配出来"。
 */
char *strdup(const char *s)
{
    size_t len;
    char  *copy;

    if (s == NULL) {
        return NULL;
    }

    len  = strlen(s) + 1u; /* 含结尾的 '\0' */
    copy = (char *)malloc(len);
    if (copy == NULL) {
        return NULL;
    }

    memcpy(copy, s, len);

    return copy;
}

char *strtok(char *str, const char *delim)
{
    /*
     * 有状态实现,与旧 XJ380 一致(它在 vfs.cpp 里被调用 2 次)。
     * ⚠ 状态是全局的:两个任务同时用会互相踩。见头文件的说明。
     *
     * 标准语义:第一次传串;之后传 NULL 表示"接着上次的位置继续"。
     * 连续的分隔符被跳过,不产生空 token。
     */
    static char *next;

    char *start;

    if (str != NULL) {
        next = str;
    }

    if (next == NULL) {
        return NULL;
    }

    /* 跳过前导分隔符 */
    while (*next != '\0' && strchr(delim, *next) != NULL) {
        next++;
    }

    if (*next == '\0') {
        next = NULL;
        return NULL;
    }

    start = next;

    /* 找到 token 结尾 */
    while (*next != '\0' && strchr(delim, *next) == NULL) {
        next++;
    }

    if (*next != '\0') {
        *next = '\0';
        next++;
    } else {
        /* 已经到串尾,下次调用应当返回 NULL */
        next = NULL;
    }

    return start;
}

/* ------------------------------------------------------------------ */
/* 字符与数字                                                          */
/* ------------------------------------------------------------------ */

int isdigit(int c)
{
    return (c >= '0' && c <= '9') ? 1 : 0;
}

int atoi(const char *pstr)
{
    int sign = 1;
    int value = 0;

    /*
     * 与 strtol 不同,atoi **没有错误报告通道**:溢出、非数字都只是
     * 得到一个"某个值"。这里刻意不把非法输入当成 0 之外的任何特殊值 ——
     * 那会让调用方以为可以靠返回值判错。
     */
    while (*pstr == ' ' || *pstr == '\t' || *pstr == '\n' || *pstr == '\r') {
        pstr++;
    }

    if (*pstr == '-') {
        sign = -1;
        pstr++;
    } else if (*pstr == '+') {
        pstr++;
    }

    while (isdigit((int)(unsigned char)*pstr)) {
        value = value * 10 + (*pstr - '0');
        pstr++;
    }

    return sign * value;
}
