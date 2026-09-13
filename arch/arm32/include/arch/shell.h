#pragma once

/*
 * 串口命令通道 —— 纯逻辑部分
 *
 * ====================================================================
 * 为什么需要它
 * ====================================================================
 *
 * 在此之前,板上出问题时唯一的交互手段是"重新烧一遍固件,加一行打印"。
 * 而每次烧写都要走 JTAG(几十秒),更要紧的是**烧写会清掉故障现场**——
 * 想看的东西往往在那一刻就没了。
 *
 * 有了命令通道,就能在系统跑着的时候现场查:
 *
 *     > dump          看设备描述表
 *     > probe         重跑驱动匹配并看统计
 *     > selftest      重跑自检报告
 *     > peek 41200000 读一个物理地址
 *
 * ====================================================================
 * 与本文件的分工
 * ====================================================================
 *
 *   本文件     行编辑状态机与命令解析。**不含任何 MMIO**,
 *              所以宿主编译器能直接编译,单测见 tests/test_arm32_shell.py。
 *   src/shell.c UART 轮询、回显、命令表与具体命令实现。
 *
 * 行编辑值得单测的原因和别处一样:它是"看起来显然"的逻辑,
 * 但有一条**安全性**上的硬要求容易被忽略(见下)。
 */

#include <arch/types.h>

/* ------------------------------------------------------------------ */
/* 行缓冲                                                              */
/* ------------------------------------------------------------------ */

/*
 * 64 字符足够本阶段的命令用,而且小到能打印出来排错。
 * 等有传参需求(如往内存写数据)再考虑加长。
 */
#define SHELL_LINE_MAX 64

typedef struct
{
    char buf[SHELL_LINE_MAX];
    u32  len;

    /*
     * ⚠ 溢出标志。这是本文件存在的主要理由。
     *
     * 缓冲区满之后到达的字符不能再被静默丢掉 —— 那样提交的就是一条
     * **被截断的命令**。而"截断"比"拒绝"危险得多:一条长命令的前缀
     * 很可能恰好是另一条**合法**命令。
     *
     * 举个具体的:用户想敲 `peek2 0x1000`(如果将来有 peek2),
     * 缓冲区只装得下 `peek`,于是执行了完全不同的操作 ——
     * 而且不会有任何报错。
     *
     * 所以溢出过的行在提交时必须被拒绝,不是执行截断版。
     */
    bool overflow;
} shell_line_t;

/* 喂进一个字符后应当做什么 */
typedef enum
{
    SHELL_ACT_IGNORED = 0, /* 不产生任何输出 */
    SHELL_ACT_ECHO = 1,    /* 回显这个字符 */
    SHELL_ACT_ERASE = 2,   /* 回显退格序列 "\b \b" */
    SHELL_ACT_SUBMIT = 3,  /* 收到回车,行已就绪(可能带 overflow) */
    SHELL_ACT_CANCEL = 4,  /* 丢弃当前行 */
} shell_act_t;

/* 终端用 "\r\n" 换行,所以回显时要把收到的 '\n' 也当作回车 */
#define SHELL_CHAR_CR 0x0D
#define SHELL_CHAR_LF 0x0A
#define SHELL_CHAR_BS 0x08
#define SHELL_CHAR_DEL 0x7F
#define SHELL_CHAR_CTRL_C 0x03
#define SHELL_CHAR_CTRL_U 0x15

static inline void shell_line_reset(shell_line_t *line)
{
    line->len = 0u;
    line->overflow = false;
    line->buf[0] = '\0';
}

/*
 * 喂一个字符进行编辑器,返回它引起的动作。
 *
 * 只负责行编辑,不认识任何命令 —— 命令解析在 shell_split / shell_lookup。
 * 这样拆分之后,行编辑的行为(退格、取消、溢出)可以完全脱离命令集测试。
 */
static inline shell_act_t shell_line_feed(shell_line_t *line, char c)
{
    switch (c) {
    case SHELL_CHAR_CR:
    case SHELL_CHAR_LF:
        /*
         * 回车。行标志不清空 —— 由调用方读走之后调 shell_line_reset,
         * 这样它有机会先检查 overflow 再决定执不执行。
         * 在这里清空会让 overflow 的检查永远读到 false。
         */
        line->buf[line->len] = '\0';
        return SHELL_ACT_SUBMIT;

    case SHELL_CHAR_BS:
    case SHELL_CHAR_DEL:
        if (line->len == 0u) {
            return SHELL_ACT_IGNORED; /* 空行上按退格:不回显,否则终端会吃掉提示符 */
        }
        line->len--;
        line->buf[line->len] = '\0';
        return SHELL_ACT_ERASE;

    case SHELL_CHAR_CTRL_C:
    case SHELL_CHAR_CTRL_U:
        shell_line_reset(line);
        return SHELL_ACT_CANCEL;

    default:
        break;
    }

    /* 只接受可打印 ASCII。控制字符一律忽略,避免把终端搞乱 */
    if (c < 0x20 || c > 0x7E) {
        return SHELL_ACT_IGNORED;
    }

    if (line->len >= (SHELL_LINE_MAX - 1u)) {
        /* 满了:记下溢出,但不接受这个字符 */
        line->overflow = true;
        return SHELL_ACT_IGNORED;
    }

    line->buf[line->len] = c;
    line->len++;
    line->buf[line->len] = '\0';
    return SHELL_ACT_ECHO;
}

/* ------------------------------------------------------------------ */
/* 命令解析                                                            */
/* ------------------------------------------------------------------ */

typedef struct
{
    const char *name;
    const char *help;
    void (*run)(const char *args);
} shell_cmd_t;

/*
 * 就地拆出一行的命令名。
 *
 * **会修改 line**:命令名之后写入 '\0'。
 * 返回参数起始位置(跳过命令名与其后的空白);行尾则返回指向 '\0' 的位置。
 * 行首空白会被跳过;**空行返回的 name 指向 '\0'**,调用方据此忽略。
 *
 * 只按空白拆分,不做引号/转义 —— 这是调试通道,不是 shell。
 * 真需要传带空格的参数时再加,而不是现在猜。
 */
static inline const char *shell_split(char *line, const char **name_out)
{
    char *p = line;
    char *name;

    while (*p == ' ' || *p == '\t') {
        p++;
    }

    name = p;

    while (*p != '\0' && *p != ' ' && *p != '\t') {
        p++;
    }

    if (*p != '\0') {
        *p = '\0';
        p++;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
    }

    *name_out = name;
    return p;
}

/*
 * 按名字**精确**查找命令。
 *
 * 不做前缀匹配:调试通道里"敲少一个字母却执行了另一条命令"
 * 比"命令不存在"危险得多 —— 后者至少会报错。
 */
static inline const shell_cmd_t *shell_lookup(const shell_cmd_t *cmds, u32 count, const char *name)
{
    u32 i;

    if (cmds == NULL || name == NULL || name[0] == '\0') {
        return NULL;
    }

    for (i = 0; i < count; i++) {
        const char *a = cmds[i].name;
        const char *b = name;

        if (a == NULL) {
            continue;
        }

        while (*a != '\0' && *a == *b) {
            a++;
            b++;
        }

        if (*a == '\0' && *b == '\0') {
            return &cmds[i];
        }
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/* 硬件侧(实现在 src/shell.c)                                        */
/* ------------------------------------------------------------------ */

/* 复位行缓冲。应当在启动流程里调用一次 */
void shell_init(void);

/*
 * 从主循环里调用。**非阻塞**:没有字符立刻返回。
 *
 * 单次最多处理固定数量的字符,防止对端持续灌数据把主循环饿死
 * (那会表现为 LED 卡住、tick 计数变慢,而"串口在收数据"本身看着正常)。
 */
void shell_poll(void);

/* 打印欢迎语与提示符 */
void shell_banner(void);
