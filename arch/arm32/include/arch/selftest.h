#pragma once

/*
 * 启动自检报告
 *
 * ====================================================================
 * 为什么要有这个
 * ====================================================================
 *
 * 在此之前,"上板验证"是靠人读串口日志然后判断"看起来没问题"。
 * 这有两个毛病:
 *
 *   1. **不可自动化** —— 每次都要人肉比对,改一行代码就得重看一遍;
 *   2. **判据模糊** —— "speedup=6x" 算不算通过?人心里有个数,
 *      但那个数没写在任何地方,换个人(或换个时间)判断可能不同。
 *
 * 所以把判据**写成代码**,并让板子以固定格式回传,由脚本判定:
 *
 *     === SELF-TEST ===
 *     CHECK mmu_stage             = 4            (expect == 4)        PASS
 *     CHECK cache_speedup         = 6            (expect >= 2)        PASS
 *     ...
 *     === SELF-TEST SUMMARY: 12 passed, 0 failed ===
 *
 * 退出码即结论。人只需要看那一行 SUMMARY。
 *
 * ====================================================================
 * 与心跳区的关系
 * ====================================================================
 *
 * 两者面向不同的失败场景,不是重复:
 *   - **心跳区**:每项结果单独写一个槽。内核挂死时串口日志会断,
 *     但心跳仍在 OCM 里,JTAG 可以事后读回"最后到哪一步、哪些项通过了";
 *   - **自检报告**:等所有项跑完后一次性回传,给人和脚本一个总账。
 *
 * 所以每项自检既写心跳也进报告 —— 挂死时靠心跳,活着时靠报告。
 */

#include <arch/types.h>

/* 判据类型。名字即语义,不要在调用处写裸的 < > */
typedef enum
{
    SELFTEST_EQ = 0, /* 必须精确相等 */
    SELFTEST_GE = 1, /* 必须 >= 期望值 */
    SELFTEST_LE = 2, /* 必须 <= 期望值 */
    SELFTEST_NE = 3, /* 必须不等(用于"错误码应为非 0"这类) */
} selftest_op_t;

/*
 * 判定单项是否通过。纯函数,宿主可单测 —— 见 tests/test_selftest.py。
 *
 * 单独抽出来是因为判据的方向极易写反:GE 写成 LE 之后,
 * "加速比 >= 2" 会变成"加速比 <= 2",而**两种写法在通过时都打印 PASS**,
 * 只有在真的出问题时才会表现出相反的结论 —— 也就是说,
 * 判据写反的验证工具会在最需要它的时候给错答案。
 */
static inline bool selftest_verdict(u32 actual, u32 expect, selftest_op_t op)
{
    switch (op) {
    case SELFTEST_EQ:
        return actual == expect;
    case SELFTEST_GE:
        return actual >= expect;
    case SELFTEST_LE:
        return actual <= expect;
    case SELFTEST_NE:
        return actual != expect;
    default:
        return false; /* 未知判据一律判失败,不放过 */
    }
}

/* 判据的文本形式,用于报告里写出"expect == 4" */
static inline const char *selftest_op_text(selftest_op_t op)
{
    switch (op) {
    case SELFTEST_EQ:
        return "==";
    case SELFTEST_GE:
        return ">=";
    case SELFTEST_LE:
        return "<=";
    case SELFTEST_NE:
        return "!=";
    default:
        return "?";
    }
}

/* ------------------------------------------------------------------ */
/* 报告输出(实现在 src/selftest.c,依赖 console)                      */
/* ------------------------------------------------------------------ */

/*
 * 报告分隔行。
 *
 * 刻意用不容易在普通日志里出现的形状:解析脚本靠它定位报告区间,
 * 若与普通输出混淆,脚本会把无关的行当成检查项 ——
 * 那种错误比"解析不到"更危险,因为它会静默地少检查几项。
 */
#define SELFTEST_BEGIN "=== SELF-TEST BEGIN ==="
#define SELFTEST_END "=== SELF-TEST END ==="
#define SELFTEST_SUMMARY_PREFIX "=== SELF-TEST SUMMARY:"

/* 打印报告头。之后每项用 selftest_report() */
void selftest_begin(void);

/*
 * 记录并打印一项。
 * 返回该项是否通过,便于调用方按需改变后续行为。
 */
bool selftest_report(const char *name, u32 actual, u32 expect, selftest_op_t op);

/*
 * 打印总账并返回失败项数。
 *
 * 返回 0 表示全通过。调用方应当把它写进心跳(挂死时也能读到结论),
 * 但**不要**因为失败就停机 —— 自检的意义是给出信息,
 * 而不是把一个本来能跑的系统拦在启动阶段。
 */
u32 selftest_summary(void);
