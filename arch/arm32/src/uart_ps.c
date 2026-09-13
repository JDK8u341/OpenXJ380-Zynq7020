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

/*
 * MR 的位布局(取自 AMD/Xilinx 随这颗芯片出货的 xuartps_hw.h,**不是**通用
 * Cadence 手册 —— 两者在通道模式的位置上不一致,而写错不会报错):
 *
 *   bit0     CLKSEL
 *   bit2:1   CHARLEN     00 = 8 位
 *   bit5:3   PARITY      0b100 = 无校验
 *   bit7:6   STOPMODE    00 = 1 位
 *   bit9:8   CHMODE      00 正常 / 01 回声 / 10 本地环回 / 11 远端环回
 *
 * ⚠ CHMODE 在 **bits[9:8]**。通用 Cadence 手册把它写作 [7:6],
 *   我一开始就是按 [7:6] 写的 —— 结果 0x80 落到了 STOPMODE[1] 上,
 *   把模式悄悄改成"2 个停止位",环回测试自然永远失败,
 *   而寄存器读回来看着完全正常(MR=0xA0,不像有任何问题)。
 *   这个错误把排查方向指向了"线缆没接好",浪费了一轮。
 */
#define UART_MR_CHM_MASK           0x00000300u
#define UART_MR_CHM_LOCAL_LOOPBACK 0x00000200u

/* 通道状态位 */
#define UART_SR_RXEMPTY 0x00000002u
#define UART_SR_TXEMPTY 0x00000008u
#define UART_SR_TXFULL  0x00000010u

/*
 * 所有轮询循环都设自旋上限。
 *
 * 这不是防御性编程,而是本板实测踩到的真实故障:
 * 当前 PS7 配置下 UART 外设不响应,寄存器读回恒为 0,
 * 于是 `while (!(SR & TXEMPTY))` 永远不成立,内核在标定阶段直接死循环。
 * 有无超时决定了"没有串口"是表现为可诊断的降级,还是表现为整机挂死。
 */
#define UART_POLL_LIMIT 2000000u

/*
 * 上一次闭环收敛实际用掉的迭代次数。
 *
 * 之所以要把它暴露出来:收敛"返回 100.5MHz"和"失败后用 100.5MHz 兜底"
 * 在心跳里长得一模一样,而这两者的含义完全相反 ——
 * 一个是"实测验证过",一个是"猜的"。没有这个计数就无法区分。
 */
static u32 g_converge_iters;

u32 uart_converge_last_iters(void)
{
    return g_converge_iters;
}

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
    /*
     * result 允许为 NULL(调用方只想要副作用,不关心协商结果)。
     * 内部统一改用局部变量,避免后面解引用空指针。
     *
     * 这里踩过一次:重构时把分频搜索挪进 uart_baud_search() 之后,
     * 忘了 result 可能是 NULL,于是 uart_converge_ref_clk() 传 NULL 进来时
     * 直接往地址 0 写。ARM 关闭 MMU 时地址 0 是 OCM 且可写,
     * 所以既不崩溃也不报错,只是静默配上错误的波特率 ——
     * 串口只剩一堆 0x00,极难定位。
     */
    uart_baud_result_t local;
    uart_baud_result_t *out = (result != NULL) ? result : &local;

    out->requested = baud;
    out->baudgen   = 0;
    out->bauddiv   = 0;
    out->actual    = 0;
    out->error_ppm = 0;
    out->valid     = false;

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
     * 分频搜索本身是纯计算,挪到 uart_baud.c 以便在宿主机上单测
     * (这段数学曾出过 7 倍偏差的 bug,见 uart_baud.h)。
     */
    uart_baud_search(uart_clk, baud, out);

    if (out->baudgen == 0) {
        return; /* 没有可用组合 */
    }

    mmio_write32(base + UART_BAUDGEN, out->baudgen);
    mmio_write32(base + UART_BAUDDIV, out->bauddiv);

    /* 再次复位收发逻辑,然后使能 */
    mmio_write32(base + UART_CR, UART_CR_TXRST | UART_CR_RXRST);
    mmio_write32(base + UART_CR, UART_CR_TXEN | UART_CR_RXEN);
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

/*
 * 收发通路自检:用 UART 自己的**内部环回**判定 RX 路径通不通。
 *
 * ====================================================================
 * 为什么值得单独做一个自检
 * ====================================================================
 *
 * "板上收不到数据"有两类完全不同的原因,而它们在现象上一模一样:
 *   (a) 驱动/寄存器配置有问题 —— 该开没开、读错了寄存器;
 *   (b) 外部通路有问题 —— 线序、对端没在发、MIO 引脚没接对。
 *
 * 光看"没回显"分不出这两类,而排查方向完全相反。
 *
 * Cadence UART 的 MR[7:6] 是通道模式(CHM),写成 0b10 就是**本地环回**:
 * 发送端在芯片内部直接接到接收端,完全不经过外部引脚。于是:
 *
 *   环回能收到 -> UART 的收发逻辑与驱动全部正常,问题在 (b) 外部通路;
 *   环回收不到 -> 问题在 (a),与线缆无关。
 *
 * 这一步把"要不要去查线"从一个猜测变成一个有结论的问题。
 *
 * ⚠ 自检会临时改 MR 再改回来。调用时应当确保没有别的代码正在用这个串口
 *   (启动阶段调用即可)。
 */
bool uart_loopback_selftest(uintptr_t base)
{
    const char probe = 'U';
    u32        saved_mr;
    u32        spins;
    char       got;

    if (!uart_probe(base)) {
        return false;
    }

    saved_mr = mmio_read32(base + UART_MR);

    /* CHM = 0b10 (本地环回);其余位保持原样,尤其是 8N1 那几位 */
    mmio_write32(base + UART_MR, (saved_mr & ~UART_MR_CHM_MASK) | UART_MR_CHM_LOCAL_LOOPBACK);

    /* 清掉 FIFO 里可能残留的东西,避免把旧数据当成环回结果 */
    mmio_write32(base + UART_CR, UART_CR_TXRST | UART_CR_RXRST);
    mmio_write32(base + UART_CR, UART_CR_TXEN | UART_CR_RXEN);

    /* 发一个字节 */
    spins = UART_POLL_LIMIT;
    while ((mmio_read32(base + UART_SR) & UART_SR_TXFULL) != 0) {
        if (--spins == 0) {
            mmio_write32(base + UART_MR, saved_mr);
            return false;
        }
    }
    mmio_write32(base + UART_TX_FIFO, (u32)probe);

    /* 等它从内部绕回来。有超时 —— 否则通路断了就是死循环 */
    spins = UART_POLL_LIMIT;
    while ((mmio_read32(base + UART_SR) & UART_SR_RXEMPTY) != 0) {
        if (--spins == 0) {
            mmio_write32(base + UART_MR, saved_mr);
            return false;
        }
    }

    got = (char)(mmio_read32(base + UART_RX_FIFO) & 0xFFu);

    /* 恢复通道模式。这一步必须做,否则串口就再也发不出去了 */
    mmio_write32(base + UART_MR, saved_mr);

    return got == probe;
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

/*
 * 测量时发送的字符数。
 *
 * ⚠ 不要为了"减少串口噪声"把它调小 —— 这会直接破坏测量精度。
 *
 * 这些字符会真的出现在串口上(测量靠计时发送才能反映线上真实速率),
 * 看着像噪声,但数量受一个硬约束:
 *
 *   UART_SR 的 TXEMPTY 表示 TX FIFO 为空,而最后一个字节此时还在
 *   移位寄存器里没发完。所以"从空到空"的计时窗口实际只覆盖了
 *   MEASURE_COUNT-1 个字符,算出来的波特率被高估 N/(N-1) 倍。
 *
 *   N=32  -> 高估 3.2%,闭环被这个偏差带偏,收敛到 103.17MHz(实测);
 *   N=200 -> 高估 0.5%,落在闭环 0.5% 的收敛判据内,一次迭代收敛,
 *            实测精确得到 100000000Hz / BAUDGEN=1736 / 0ppm。
 *
 * 换句话说:N 越小噪声越少,但系统误差越大,而闭环只会把
 * "测出来的值"拉到目标,不会察觉这个偏差是测量本身的。
 * 200 是实测验证过能收敛到正确参考时钟的最小档位。
 */
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

    g_converge_iters = 0;

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

        g_converge_iters = iter + 1u;

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
