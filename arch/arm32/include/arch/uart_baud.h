#pragma once

/*
 * UART 波特率分频计算(纯函数,不碰硬件)
 *
 * 单独成模块是为了**可测试**:这段数学曾经出过一个 7 倍的偏差 bug
 * (标定点选了 AMD 驱动遍历范围之外的 BAUDDIV=0,导致外推整体偏掉),
 * 而它夹在 MMIO 代码里时无法在宿主机上单测。
 * 现在它不依赖 arch/io.h,可以直接用宿主 gcc 编译验证。
 */

#include <arch/types.h>

/* 波特率协商结果,便于诊断 */
typedef struct
{
    u32 requested;  /* 请求的波特率 */
    u32 actual;     /* 实际可达成的波特率 */
    u32 baudgen;    /* BAUDGEN 寄存器值 */
    u32 bauddiv;    /* BAUDDIV 寄存器值 */
    u32 error_ppm;  /* 相对误差,单位 ppm */
    bool valid;     /* 误差是否在可接受范围内 */
} uart_baud_result_t;

/*
 * 在 BAUDDIV 的合法区间内搜索使实际波特率最接近目标的组合。
 *
 * 算法取自 AMD 官方 XUartPs_SetBaudRate:
 *     BaudRate = InputClk / (BAUDGEN × (BAUDDIV + 1))
 * 遍历 BAUDDIV = 4..254 取误差最小者。
 *
 * ⚠ BAUDDIV 的下界 4 是这个算法的**硬性约束**,不是随便定的:
 *   区间外的取值(尤其是 0)实际分频语义与公式不符。
 *   本板曾用 BAUDDIV=0 做标定点,结果参考时钟估算偏低 7.033 倍。
 *
 * @param input_clk  UART 参考时钟(Hz)
 * @param baud       目标波特率
 * @param result     输出;可为 NULL
 */
void uart_baud_search(u32 input_clk, u32 baud, uart_baud_result_t *result);
