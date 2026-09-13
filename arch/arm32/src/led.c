/*
 * AC880-CB 的 LED / 开关
 *
 * 硬件事实(原理图 + AXI_GPIO_1_BSP.xdc 交叉确认):
 *   PL 侧 8 个 LED : LED0..LED7 -> N22 P22 R18 T18 P20 P21 R20 R21
 *     走双通道 AXI GPIO @ 0x41200000
 *       ch1 = 拨码开关(输入)  ch2 = LED(输出)
 *   PS 侧 MIO7/MIO8: L3_SEL=0 配置为 GPIO,可作 PS LED
 *
 * ====================================================================
 * M3 之后的职责划分
 * ====================================================================
 *
 * **PL 侧(AXI GPIO)的寄存器操作已经移到 src/axi_gpio.c 的驱动里** ——
 * 基址、通道数、位宽现在全部来自设备描述表,不再硬编码在这里。
 * 本文件保留的 led_pl_set / sw_read 只是转发,好处是主循环与自检代码
 * 一行都不用改。
 *
 * **PS 侧(PS GPIO MIO7/MIO8)留在这里**,因为它不是发现来的设备:
 * MIO 引脚的功能由 SLCR 的 L3_SEL 决定,没有"基址/参数"可描述,
 * 属于板级固有属性,由 platform.h 的常量描述就够了。
 *
 * 这个划分本身是有信息的:**不是所有硬件都适合走设备描述层**。
 * 有基址、有参数、可能缺席的,适合;板级固有的(引脚复用、
 * 时钟树),硬编码在板级头文件里反而更清楚。
 */

#include <arch/axi_gpio.h>
#include <arch/io.h>
#include <arch/led.h>
#include <arch/platform.h>

/* 候选 PS LED:MIO7 与 MIO8 */
#define PS_LED_MASK ((1u << 7) | (1u << 8))

void led_init(void)
{
    /*
     * 这里只初始化 PS 侧。
     *
     * ⚠ PL 侧(AXI GPIO)不在这里初始化 —— 它的方向寄存器要按描述层给出的
     *   位宽来配,而那个信息要等 board_probe_all() 跑完才有。
     *   所以 led_init() 现在可以**早于描述层**调用,这对启动顺序很重要:
     *   它是"串口还没起来时唯一的反馈手段",必须足够早。
     *   PL LED 会在 AXI GPIO 驱动 probe 时被初始化。
     */
    mmio_set_bits32(PLAT_GPIO_BASE + GPIO_DIRM_0, PS_LED_MASK);
    mmio_set_bits32(PLAT_GPIO_BASE + GPIO_OEN_0, PS_LED_MASK);

    /*
     * 用 MASK_DATA_0_LSW 做掩码写,只改 bit7/bit8,
     * 不影响 MIO0-15 上的其它引脚。
     * 寄存器高 16 位是掩码(1 = 写入),低 16 位是数据。
     */
    mmio_write32(PLAT_GPIO_BASE + GPIO_MASK_DATA_0_LSW, (PS_LED_MASK << 16));
}

void led_pl_set(u8 value)
{
    /* 转发给描述层找出来的那个 AXI GPIO 驱动 */
    axi_gpio_led_write(value);
}

u8 sw_read(void)
{
    return axi_gpio_switch_read();
}

void led_ps_set(bool on)
{
    u32 value = on ? PS_LED_MASK : 0u;

    mmio_write32(PLAT_GPIO_BASE + GPIO_MASK_DATA_0_LSW, (PS_LED_MASK << 16) | value);
}

u32 led_get_dirm0(void)
{
    return mmio_read32(PLAT_GPIO_BASE + GPIO_DIRM_0);
}

u32 led_get_oen0(void)
{
    return mmio_read32(PLAT_GPIO_BASE + GPIO_OEN_0);
}
