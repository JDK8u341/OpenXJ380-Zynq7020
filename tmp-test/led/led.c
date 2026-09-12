/*
 * AC880 底板 LED 测试（Zynq-7020 / Cortex-A9，裸机）v3
 *
 * v1 -> v2 修正：
 *   phase = (gt>>20)^(tick>>3) 把高速定时器与循环计数异或，相位实际随机
 *   -> LED 乱闪。v2 起改为单一时间基准。
 *
 * v2 -> v3 新增 PS 侧 LED：
 *   实测 MIO7 / MIO8 的 L3_SEL=0（配置为 GPIO），但 DIRM_0=0 且 OEN_0=0，
 *   即全为输入、输出使能关闭 -> PS LED 不会亮。
 *   v3 把它们配成输出并驱动。
 *   （MIO12 / MIO47 的 L3_SEL=2，被外设占用，本次不动，避免总线上顶。）
 *
 * === 硬件事实 ===
 * PL 侧：双通道 AXI GPIO @ 0x41200000（各 8 位）
 *   ch1 = gpio_rtl_0  拨码开关 H15 N18 N17 N20 N19 M20 M19 M16 -> 输入
 *   ch2 = gpio_rtl_1  8 个 LED N22 P22 R18 T18 P20 P21 R20 R21 -> 输出
 * PS 侧：MIO7 / MIO8 候选 PS LED，经 PS GPIO 控制器 @ 0xE000A000 驱动
 *
 * === 显示约定 ===
 *   PL 8 灯：跑马灯，约 5 Hz 一步（全局定时器 333.33MHz >> 26）
 *            某位拨码开关闭合 -> 该位取反，占空比 1/8 -> 7/8（明显变亮）
 *   PS 2 灯：MIO7/MIO8 同步慢闪，约 1.2 Hz（>> 28）
 *
 * === 心跳（OCM 0x00020000，可用 xsdb mrd 读回）===
 *   [0]=magic [1]=步进 [2]=PL图案 [3]=开关 [4]=全局定时器
 *   [5]=PS LED 状态 [6]=DIRM_0 [7]=OEN_0
 */

typedef unsigned int u32;

/* --- PL 侧：双通道 AXI GPIO --- */
#define AXI_GPIO_BASE   0x41200000u
#define GPIO_CH1_DATA   (*(volatile u32 *)(AXI_GPIO_BASE + 0x00))
#define GPIO_CH1_TRI    (*(volatile u32 *)(AXI_GPIO_BASE + 0x04))
#define GPIO_CH2_DATA   (*(volatile u32 *)(AXI_GPIO_BASE + 0x08))
#define GPIO_CH2_TRI    (*(volatile u32 *)(AXI_GPIO_BASE + 0x0C))

/* --- PS 侧：Zynq GPIO 控制器 --- */
#define PS_GPIO_BASE    0xE000A000u
#define PS_MASK_DATA_LSW (*(volatile u32 *)(PS_GPIO_BASE + 0x0000))
#define PS_DATA_LSW      (*(volatile u32 *)(PS_GPIO_BASE + 0x0040))
#define PS_DIRM_0        (*(volatile u32 *)(PS_GPIO_BASE + 0x0204))
#define PS_OEN_0         (*(volatile u32 *)(PS_GPIO_BASE + 0x0208))

/* 候选 PS LED：MIO7 与 MIO8 */
#define PS_LED_MASK      ((1u << 7) | (1u << 8))

#define GT_COUNTER_LO    (*(volatile u32 *)0xF8F00200u)

#define HEARTBEAT        ((volatile u32 *)0x00020000u)
#define HB_MAGIC         0x4C454433u /* "LED3" */

static void busy_delay(volatile u32 n)
{
    while (n--) {
        __asm__ volatile("nop");
    }
}

/* 用掩码方式写 PS GPIO，避免影响 MIO0-15 上的其它位 */
static void ps_leds(u32 on)
{
    u32 v = on ? PS_LED_MASK : 0u;
    PS_MASK_DATA_LSW = (PS_LED_MASK << 16) | (v & PS_LED_MASK);
}

static void pl_leds(u32 v)
{
    GPIO_CH2_DATA = v & 0xFFu;
}

int main(void)
{
    u32 last_step = 0xFFFFFFFFu;
    u32 last_ps   = 0xFFFFFFFFu;
    u32 steps     = 0;

    /* 1) PL 方向：ch1 输入（开关），ch2 输出（LED） */
    GPIO_CH1_TRI = 0xFFFFFFFFu;
    GPIO_CH2_TRI = 0x00000000u;
    pl_leds(0x00u);

    /* 2) PS 方向：把 MIO7 / MIO8 设为输出并使能 */
    PS_DIRM_0 |= PS_LED_MASK;
    PS_OEN_0  |= PS_LED_MASK;
    ps_leds(0u);

    /* 3) 上电自检 */
    pl_leds(0xFFu); ps_leds(1u); busy_delay(2000000u);
    pl_leds(0x00u); ps_leds(0u); busy_delay(2000000u);
    pl_leds(0xAAu); ps_leds(1u); busy_delay(2000000u);
    pl_leds(0x55u); ps_leds(0u); busy_delay(2000000u);
    pl_leds(0x00u);

    HEARTBEAT[0] = HB_MAGIC;

    /* 4) 主循环：完全由全局定时器定节奏 */
    for (;;) {
        u32 gt = GT_COUNTER_LO;

        /* PS 侧：约 1.2 Hz 同步慢闪 */
        u32 ps_phase = (gt >> 28) & 1u;
        if (ps_phase != last_ps) {
            ps_leds(ps_phase);
            last_ps = ps_phase;
            HEARTBEAT[5] = ps_phase;
            HEARTBEAT[6] = PS_DIRM_0;
            HEARTBEAT[7] = PS_OEN_0;
        }

        /* PL 侧：约 5 Hz 跑马灯，叠加拨码开关 */
        {
            u32 step = gt >> 26;
            if (step != last_step) {
                u32 sw  = GPIO_CH1_DATA & 0xFFu;
                u32 bar = 1u << (step & 7u);

                pl_leds(bar ^ sw);

                last_step = step;
                steps++;

                HEARTBEAT[1] = steps;
                HEARTBEAT[2] = bar ^ sw;
                HEARTBEAT[3] = sw;
                HEARTBEAT[4] = gt;
            }
        }
    }

    return 0;
}
