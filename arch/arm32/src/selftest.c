/*
 * 启动自检报告的输出部分
 *
 * 判定逻辑是纯的,放在 include/arch/selftest.h 里(可宿主单测);
 * 本文件只负责"把它打出来" —— 这一步依赖 console,只能上板验证。
 *
 * 格式设计上只有一个要求:**解析脚本不会把无关的行当成检查项**。
 * 所以每项都以行首的 "CHECK " 开头、以 PASS/FAIL 结尾,
 * 报告区间用两条不易在普通日志里出现的分隔行框住。
 * 这个约束不是洁癖:如果脚本误把别的行当成检查项,
 * 它会静默地少检查几项,而"少检查"和"检查通过"在输出上长得一样。
 */

#include <arch/console.h>
#include <arch/selftest.h>

static u32 g_passed;
static u32 g_failed;

void selftest_begin(void)
{
    g_passed = 0;
    g_failed = 0;

    console_printf("\n%s\n", SELFTEST_BEGIN);
}

bool selftest_report(const char *name, u32 actual, u32 expect, selftest_op_t op)
{
    bool ok = selftest_verdict(actual, expect, op);

    if (ok) {
        g_passed++;
    } else {
        g_failed++;
    }

    /*
     * 名称占 22 字符、数值占 12 字符 —— 用 %-22s 之类的左对齐本来更省事,
     * 但 console.c 是个最小 printf,只有 '0' 与 '-' 两个标志位,
     * 宽度是固定的。所以名称的长度由调用方保证,这里只对齐数值列。
     *
     * 对齐是为了人读;脚本解析靠的是行首 "CHECK " 与行尾 PASS/FAIL,
     * 对空格数量不敏感。
     */
    console_printf("CHECK %s = %u (expect %s %u) %s\n", name, actual, selftest_op_text(op), expect,
                   ok ? "PASS" : "FAIL");

    return ok;
}

u32 selftest_summary(void)
{
    console_printf("%s %u passed, %u failed ===\n", SELFTEST_SUMMARY_PREFIX, g_passed, g_failed);
    console_printf("%s\n", SELFTEST_END);

    return g_failed;
}
