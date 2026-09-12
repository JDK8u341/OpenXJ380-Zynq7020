/*
 * Cadence UARTPS 驱动实现
 *
 * 寄存器偏移与位定义取自 AMD xuartps_v3_17/xuartps_hw.h。
 */

#include <arch/io.h>
#include <arch/platform.h>
#include <arch/timer.h>
#include <arch/uart_ps.h>

/* 寄存器偏移 */
#define UART_CR       0x00u /* Control Register      [8:0]  */
#define UART_MR       0x04u /* Mode Register         [9:0]  */
#define UART_IDR      0x0Cu /* Interrupt Disable     [12:0] */
#define UART_BAUDGEN  0x18u /* Baud Rate Generator   [15:0] */
#define UART_SR       0x2Cu /* Channel Status        [14:0] */
#define UART_RX_FIFO  0x30u /* 接收 FIFO */
#define UART_TX_FIFO  0x30u /* 发送 FIFO(与 RX 同址,读=收 写=发) */
#define UART_BAUDDIV  0x34u /* Baud Rate Divider     [7:0]  */

/* 控制寄存器位 */
#define UART_CR_RXRST 0x00000001u
#define UART_CR_TXRST 0x00000002u
#define UART_CR_RXEN  0x00000004u
#define UART_CR_TXEN  0x00000010u

/* 模式寄存器:8 位数据 / 无校验 / 1 停止位 */
#define UART_MR_PARITY_NONE 0x00000020u

/* 通道状态位 */
#define UART_SR_RXEMPTY 0x00000002u
#define UART_SR_TXEMPTY 0x00000008u
#define UART_SR_TXFULL  0x00000010u

/* AMD 驱动同款上限:相对误差超过 5% 视为不可用 */
#define UART_MAX_BAUD_ERROR_PERCENT 5u

/*
 * 所有轮询循环都设自旋上限。
 *
 * 这不是防御性编程,而是本板实测踩到的真实故障:
 * 当前 PS7 配置下 UART 外设不响应,寄存器读回恒为 0,
 * 于是 `while (!(SR & TXEMPTY))` 永远不成立,内核在标定阶段直接死循环。
 * 有无超时决定了"没有串口"是表现为可诊断的降级,还是表现为整机挂死。
 */
#define UART_POLL_LIMIT 2000000u

/* 打开指定 UART 的 AMBA 外设时钟。SLCR 需要先解锁 */
static void uart_enable_aper_clock(uintptr_t base)
{
    u32 bit = (base == PLAT_UART1_BASE) ? APER_CLK_UART1 : APER_CLK_UART0;

    mmio_write32(SLCR_UNLOCK, SLCR_UNLOCK_KEY);
    mmio_set_bits32(SLCR_APER_CLK_CTRL, bit);
    mmio_write32(SLCR_LOCK, SLCR_LOCK_KEY);
}

/* 等待 TX FIFO 出现空位;超时返回 false */
static bool uart_wait_tx_space(uintptr_t base)
{
    u32 spins = UART_POLL_LIMIT;

    while ((mmio_read32(base + UART_SR) & UART_SR_TXFULL) != 0) {
        if (--spins == 0) {
            return false;
        }
    }
    return true;
}

/* 等待发送器完全空闲;超时返回 false */
static bool uart_wait_tx_empty(uintptr_t base)
{
    u32 spins = UART_POLL_LIMIT;

    while ((mmio_read32(base + UART_SR) & UART_SR_TXEMPTY) == 0) {
        if (--spins == 0) {
            return false;
        }
    }
    return true;
}

/*
 * 探测 UART 是否真的在响应:未时钟使能时寄存器读回恒为 0。
 * 会先尝试打开 APER 外设时钟,再写一个已知值读回。
 */
bool uart_probe(uintptr_t base)
{
    uart_enable_aper_clock(base);

    mmio_write32(base + UART_MR, UART_MR_PARITY_NONE);

    return mmio_read32(base + UART_MR) != 0;
}

void uart_init(uintptr_t base, u32 uart_clk, u32 baud, uart_baud_result_t *result)
{
    u32 best_baudgen   = 0;
    u32 best_bauddiv   = 0;
    u32 best_error     = 0xFFFFFFFFu;
    u32 iter_bauddiv;
    u32 input_clk = uart_clk;

    if (result != NULL) {
        result->valid = false;
    }

    /* 探测外设是否存在(顺带打开 APER 时钟)。不存在则不做任何后续配置 */
    if (!uart_probe(base)) {
        return;
    }

    /* 先复位收发逻辑,避免配置过程中产生毛刺 */
    mmio_write32(base + UART_CR, UART_CR_TXRST | UART_CR_RXRST);

    /* 8 位数据、无校验、1 停止位 */
    mmio_write32(base + UART_MR, UART_MR_PARITY_NONE);

    /* 屏蔽全部中断 —— 当前是轮询模式 */
    mmio_write32(base + UART_IDR, 0xFFFFFFFFu);

    if (uart_clk == 0 || baud == 0) {
        /* 时钟未知:只做基本复位,不配置波特率 */
        mmio_write32(base + UART_CR, UART_CR_TXEN | UART_CR_RXEN);
        return;
    }

    /*
     * 遍历 BAUDDIV 4..254,找使实际波特率最接近目标的组合。
     * 这是 AMD XUartPs_SetBaudRate 的同一算法。
     */
    for (iter_bauddiv = 4; iter_bauddiv < 255; iter_bauddiv++) {
        u32 baudgen = input_clk / (baud * (iter_bauddiv + 1));
        u32 actual;
        u32 error;

        if (baudgen == 0) {
            continue; /* 分频值过小,跳过 */
        }

        actual = input_clk / (baudgen * (iter_bauddiv + 1));
        error  = (baud > actual) ? (baud - actual) : (actual - baud);

        if (error < best_error) {
            best_error   = error;
            best_baudgen = baudgen;
            best_bauddiv = iter_bauddiv;
        }
    }

    mmio_write32(base + UART_BAUDGEN, best_baudgen);
    mmio_write32(base + UART_BAUDDIV, best_bauddiv);

    /* 再次复位收发逻辑,然后使能 */
    mmio_write32(base + UART_CR, UART_CR_TXRST | UART_CR_RXRST);
    mmio_write32(base + UART_CR, UART_CR_TXEN | UART_CR_RXEN);

    if (result != NULL) {
        result->requested = baud;
        result->baudgen   = best_baudgen;
        result->bauddiv   = best_bauddiv;
        result->actual    = (best_baudgen == 0) ? 0 : input_clk / (best_baudgen * (best_bauddiv + 1));
        result->error_ppm = (baud == 0) ? 0 : (best_error * 1000000u) / baud;
        result->valid     = (best_error * 100u) / baud <= UART_MAX_BAUD_ERROR_PERCENT;
    }
}

void uart_putc(uintptr_t base, char c)
{
    if (!uart_wait_tx_space(base)) {
        return; /* 外设无响应:丢弃而不是挂死 */
    }
    mmio_write32(base + UART_TX_FIFO, (u32)(u8)c);
}

void uart_write(uintptr_t base, const char *data, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        if (data[i] == '\n') {
            uart_putc(base, '\r'); /* 串口终端需要 CRLF */
        }
        uart_putc(base, data[i]);
    }
}

void uart_puts(uintptr_t base, const char *str)
{
    while (*str != '\0') {
        if (*str == '\n') {
            uart_putc(base, '\r');
        }
        uart_putc(base, *str++);
    }
}

bool uart_rx_ready(uintptr_t base)
{
    return (mmio_read32(base + UART_SR) & UART_SR_RXEMPTY) == 0;
}

char uart_getc(uintptr_t base)
{
    return (char)(mmio_read32(base + UART_RX_FIFO) & 0xFFu);
}

/* ------------------------------------------------------------------ */
/* 参考时钟自标定                                                      */
/* ------------------------------------------------------------------ */

/*
 * 标定用的固定参数。
 * BAUDGEN 取 256 是一个折中:
 *   - 参考时钟在 10~200MHz 时,对应波特率约 39k~780k,发送耗时可测且不过长
 *   - 是 2 的幂,便于人工核对寄存器值
 */
#define CAL_BAUDGEN 256u
#define CAL_BAUDDIV 0u
#define CAL_COUNT   1000u

u32 uart_calibrate_ref_clk(uintptr_t base)
{
    u32 i;
    u32 t_start;
    u32 t_end;
    u32 elapsed;

    /* 外设时钟没开时寄存器恒为 0,标定必然失败,提前退出避免空转 */
    if (!uart_probe(base)) {
        return 0;
    }

    /* 用已知的 BAUDGEN/BAUDDIV 重新配置,不复位 FIFO 以免影响计时 */
    mmio_write32(base + UART_BAUDGEN, CAL_BAUDGEN);
    mmio_write32(base + UART_BAUDDIV, CAL_BAUDDIV);
    mmio_write32(base + UART_CR, UART_CR_TXRST | UART_CR_RXRST);
    mmio_write32(base + UART_CR, UART_CR_TXEN | UART_CR_RXEN);

    /* 等待发送器完全空闲,保证计时从干净状态开始 */
    if (!uart_wait_tx_empty(base)) {
        return 0;
    }

    t_start = timer_read_ticks_low();

    /*
     * 全程轮询写,保证 FIFO 不被抽空 —— 中间一旦出现空隙,
     * 总耗时就不等于纯粹的 10bit/字节,标定会偏大。
     */
    for (i = 0; i < CAL_COUNT; i++) {
        if (!uart_wait_tx_space(base)) {
            return 0;
        }
        mmio_write32(base + UART_TX_FIFO, (u32)'A');
    }

    /* 等最后一个字节完全移出移位寄存器,计时才包含它 */
    if (!uart_wait_tx_empty(base)) {
        return 0;
    }

    t_end = timer_read_ticks_low();
    elapsed = t_end - t_start;

    if (elapsed == 0) {
        return 0; /* 时钟未使能或计数器没跑 */
    }

    /*
     * f_ref = N × 10bit × BAUDGEN × (BAUDDIV+1) × f_gt / elapsed
     *
     * N=1000, BAUDGEN=256 -> N×10×BAUDGEN = 2,560,000
     * 再乘 f_gt(约 3.33e8) 得约 8.5e14,必须用 64 位中间量。
     */
    return (u32)(((u64)CAL_COUNT * 10ull * (u64)CAL_BAUDGEN * (u64)(CAL_BAUDDIV + 1u) *
                  (u64)PLAT_GLOBAL_TIMER_FREQ_HZ) /
                 (u64)elapsed);
}

/* ------------------------------------------------------------------ */
/* 实测当前配置下的真实波特率                                          */
/* ------------------------------------------------------------------ */

#define MEASURE_COUNT 200u

/*
 * 在"已经配置好的当前设定"下直接测量真实波特率,不做任何外推。
 *
 * 为什么需要它:uart_calibrate_ref_clk() 是在 BAUDGEN=256/BAUDDIV=0 这一点
 * 测出参考时钟,再按 "baud ∝ 1/(BAUDGEN×(BAUDDIV+1))" 外推到工作点。
 * 一旦这个线性关系在实际器件上不严格成立(不同 BAUDDIV 区段行为不同),
 * 外推结果就会整体偏掉,而且从寄存器上完全看不出来 ——
 * 表现为"配置写着 9600,线上却不是 9600"。
 *
 * 这个函数在工作点上直接量,是唯一能证伪的手段。
 */
u32 uart_measure_baud(uintptr_t base)
{
    u32 i;
    u32 t_start;
    u32 t_end;
    u32 elapsed;

    if (!uart_probe(base)) {
        return 0;
    }

    /* 不改波特率寄存器,完全按当前设定测量 */
    mmio_write32(base + UART_CR, UART_CR_TXRST | UART_CR_RXRST);
    mmio_write32(base + UART_CR, UART_CR_TXEN | UART_CR_RXEN);

    if (!uart_wait_tx_empty(base)) {
        return 0;
    }

    t_start = timer_read_ticks_low();

    for (i = 0; i < MEASURE_COUNT; i++) {
        if (!uart_wait_tx_space(base)) {
            return 0;
        }
        mmio_write32(base + UART_TX_FIFO, (u32)'A');
    }

    if (!uart_wait_tx_empty(base)) {
        return 0;
    }

    t_end = timer_read_ticks_low();
    elapsed = t_end - t_start;

    if (elapsed == 0) {
        return 0;
    }

    /* baud = N × 10bit / 秒;秒 = elapsed / f_gt */
    return (u32)(((u64)MEASURE_COUNT * 10ull * (u64)PLAT_GLOBAL_TIMER_FREQ_HZ) / (u64)elapsed);
}

/* ------------------------------------------------------------------ */
/* 闭环收敛参考时钟                                                    */
/* ------------------------------------------------------------------ */

u32 uart_converge_ref_clk(uintptr_t base, u32 target_baud)
{
    u32 f_ref = 100000000u; /* 初值取 Xilinx 常见默认;闭环会把它拉到位 */
    u32 iter;

    if (!uart_probe(base) || target_baud == 0) {
        return 0;
    }

    /*
     * 为什么不用 uart_calibrate_ref_clk 的外推:
     * 那个函数在 BAUDGEN=256/BAUDDIV=0 处测参考时钟。但 AMD 官方驱动
     * 遍历 BAUDDIV 的范围是 4..254 —— BAUDDIV=0 不在有效区间,
     * 该点的实际分频不等于 (BAUDDIV+1)=1,于是整条外推链按固定倍数偏掉。
     * 本板实测偏移 7.033 倍(真值约 100.5MHz,外推给出 14.3MHz)。
     *
     * 闭环不依赖任何模型:选一个工作点实测波特率,再按
     * "参考时钟与实际波特率成正比" 修正,迭代到收敛。
     */
    for (iter = 0; iter < 12; iter++) {
        u32 measured;
        u32 diff;

        uart_init(base, f_ref, target_baud, NULL);
        measured = uart_measure_baud(base);
        if (measured == 0) {
            return 0;
        }

        /* 相对误差 ≤ 0.5% 即认为收敛 */
        diff = (measured > target_baud) ? (measured - target_baud) : (target_baud - measured);
        if (((u64)diff * 200ull) <= (u64)target_baud) {
            break;
        }

        /* f_ref 与实测波特率成正比,按比例修正;用 64 位避免中间溢出 */
        f_ref = (u32)(((u64)f_ref * (u64)measured) / (u64)target_baud);

        /* 防止比例失控 */
        if (f_ref < 1000000u || f_ref > 400000000u) {
            return 0;
        }
    }

    return f_ref;
}
