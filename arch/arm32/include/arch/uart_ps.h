#pragma once

/*
 * Cadence UARTPS 驱动(Zynq PS 串口)
 *
 * 替换 x86_64 侧的 driver/serial/serial_port.cpp —— 那个驱动写死了
 * COM1 = I/O 端口 0x3F8,并依赖 port I/O 指令(inb/outb)。
 * Zynq 的 UART 是 MMIO 外设,基址 0xE0000000 (UART0) / 0xE0001000 (UART1)。
 *
 * 波特率公式取自 AMD 官方 xuartps 驱动(XUartPs_SetBaudRate):
 *     BaudRate = InputClk / (BAUDGEN × (BAUDDIV + 1))
 * 其中 BAUDDIV 在 4..254 之间遍历取误差最小者。
 */

#include <arch/types.h>

/* 波特率协商结果,便于诊断 */
typedef struct
{
    u32 requested;   /* 请求的波特率 */
    u32 actual;      /* 实际可达成的波特率 */
    u32 baudgen;     /* BAUDGEN 寄存器值 */
    u32 bauddiv;     /* BAUDDIV 寄存器值 */
    u32 error_ppm;   /* 相对误差,单位 ppm */
    bool valid;      /* 误差是否在可接受范围内 */
} uart_baud_result_t;

/*
 * 初始化 UART 并设置波特率。
 *
 * @param base       UART 基址(PLAT_UART0_BASE / PLAT_UART1_BASE)
 * @param uart_clk   UART 参考时钟频率(Hz) —— 见下方说明
 * @param baud       目标波特率
 * @param result     可选,回填实际协商结果(可为 NULL)
 *
 * ⚠ uart_clk 是这套驱动唯一的外部依赖。Zynq 的 UART 参考时钟来自
 *   SLCR 的 UART_CLK_CTRL(0xF8000154)分频,而分频器的输入 PLL 频率
 *   在板上尚未标定(见 arch/arm32/README.md)。
 *   填错不会导致挂死,只会输出乱码 —— 因此调用方可以扫描候选值。
 */
void uart_init(uintptr_t base, u32 uart_clk, u32 baud, uart_baud_result_t *result);

/* 发送一个字节(轮询等待 TX FIFO 有空间;外设无响应时丢弃) */
void uart_putc(uintptr_t base, char c);

/* 发送字符串 */
void uart_puts(uintptr_t base, const char *str);

/* 发送定长缓冲 */
void uart_write(uintptr_t base, const char *data, size_t len);

/* 查询是否有接收数据 */
bool uart_rx_ready(uintptr_t base);

/* 读一个字节(调用前应确认 uart_rx_ready) */
char uart_getc(uintptr_t base);

/*
 * 探测 UART 是否真的在响应。
 *
 * 必要时会先打开该 UART 的 AMBA 外设时钟(APER_CLK_CTRL)。
 * 本板实测:AXI_GPIO_1 设计的 ps7_init 把 UART0/UART1 的 APER 门控都关着,
 * 此时 UART 寄存器读回恒为 0 —— 若不先探测就直接轮询 TX FIFO,
 * 内核会在等待中死循环。调用方应先探测再决定是否使用控制台。
 */
bool uart_probe(uintptr_t base);

/*
 * 自标定 UART 参考时钟。
 *
 * 原理:UART 的发送速率完全由 "参考时钟 ÷ (BAUDGEN×(BAUDDIV+1))" 决定。
 * 把 BAUDGEN/BAUDDIV 设为已知值,再测量发送固定字节数所耗的时间,
 * 就能反推出参考时钟:
 *
 *     t = N × 10bit × BAUDGEN × (BAUDDIV+1) / f_ref
 *  => f_ref = N × 10 × BAUDGEN × (BAUDDIV+1) / t
 *
 * 时间基准用 Cortex-A9 全局定时器(频率已知)。
 * 这样就不需要示波器,也不需要人工试错波特率。
 *
 * @param base  UART 基址
 * @return      推算出的参考时钟(Hz);测量失败返回 0
 *
 * ⚠ 已废弃,不要在新代码里使用 —— 改用 uart_converge_ref_clk()。
 *
 * 本板实测:本函数返回 14.3 MHz,而真值是 100 MHz,偏低 7.033 倍。
 * 原因是它用 BAUDDIV=0 作为标定点,而 AMD 官方驱动遍历的 BAUDDIV 范围是
 * 4..254 —— 0 不在有效区间,该点的实际分频不等于 (BAUDDIV+1)=1,
 * 于是整条外推链按固定倍数偏掉。保留此实现仅为记录这个陷阱。
 */
u32 uart_calibrate_ref_clk(uintptr_t base);

/*
 * 在当前已配置的设定下实测真实波特率(不做外推)。
 *
 * 与 uart_calibrate_ref_clk 的区别:后者在 BAUDGEN=256 处测参考时钟再外推,
 * 前者直接在工作点上量。当外推假设不成立时,两者会给出不同的答案 ——
 * 而 uart_measure_baud 才是线上实际发生的事。
 */
u32 uart_measure_baud(uintptr_t base);

/*
 * 闭环收敛出参考时钟:反复"设定 -> 实测 -> 按比例修正",直到实际波特率
 * 与目标相符(相对误差 ≤ 0.5%)。
 *
 * 这是获知 UART_REF_CLK 的**首选**手段。uart_calibrate_ref_clk 依赖
 * "baud ∝ 1/(BAUDGEN×(BAUDDIV+1))" 从单点外推,而该假设在本板上不成立
 * (外推值偏低 7.033 倍),因此不应再作为主路径使用。
 *
 * @return 收敛后的参考时钟(Hz);失败返回 0
 */
u32 uart_converge_ref_clk(uintptr_t base, u32 target_baud);
