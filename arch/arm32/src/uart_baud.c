/*
 * UART 波特率分频计算
 *
 * 纯计算,不含任何 MMIO 或架构相关代码 —— 这样宿主 gcc 就能编译它,
 * tests/test_arm32_uart_baud.py 正是这么做的。
 */

#include <arch/uart_baud.h>

/* AMD 驱动同款上限:相对误差超过 5% 视为不可用 */
#define UART_MAX_BAUD_ERROR_PERCENT 5u

/*
 * BAUDDIV 的合法下界。
 * AMD 的 XUartPs_SetBaudRate 从 4 开始遍历,这里保持一致 ——
 * 见 uart_baud.h 中关于 7 倍偏差事故的说明。
 */
#define UART_BAUDDIV_MIN 4u
#define UART_BAUDDIV_MAX 254u

void uart_baud_search(u32 input_clk, u32 baud, uart_baud_result_t *result)
{
    u32 best_baudgen = 0;
    u32 best_bauddiv = 0;
    u32 best_error   = 0xFFFFFFFFu;
    u32 iter_bauddiv;

    if (result == NULL) {
        return;
    }

    result->requested = baud;
    result->baudgen   = 0;
    result->bauddiv   = 0;
    result->actual    = 0;
    result->error_ppm = 0;
    result->valid     = false;

    if (input_clk == 0 || baud == 0) {
        return;
    }

    for (iter_bauddiv = UART_BAUDDIV_MIN; iter_bauddiv <= UART_BAUDDIV_MAX; iter_bauddiv++) {
        u32 baudgen = input_clk / (baud * (iter_bauddiv + 1u));
        u32 actual;
        u32 error;

        if (baudgen == 0) {
            continue; /* 分频值过小,这一档不可用 */
        }

        actual = input_clk / (baudgen * (iter_bauddiv + 1u));
        error  = (baud > actual) ? (baud - actual) : (actual - baud);

        if (error < best_error) {
            best_error   = error;
            best_baudgen = baudgen;
            best_bauddiv = iter_bauddiv;
        }
    }

    if (best_baudgen == 0) {
        return; /* 输入时钟太低,没有可用组合 */
    }

    result->baudgen   = best_baudgen;
    result->bauddiv   = best_bauddiv;
    result->actual    = input_clk / (best_baudgen * (best_bauddiv + 1u));
    result->error_ppm = (best_error * 1000000u) / baud;
    result->valid     = ((u64)best_error * 100ull) <= ((u64)baud * UART_MAX_BAUD_ERROR_PERCENT);
}
