/*
 * AC880-CB 的 LED / 开关驱动
 *
 * 本文件是 M0 阶段的可视输出手段。它同时覆盖了两条不同的
 * 硬件访问路径,这正是后续驱动要复用的两种模式:
 *   1. PL 外设经 AXI 互联访问(AXI GPIO @ 0x41200000)
 *   2. PS 硬核寄存器直接访问(PS GPIO @ 0xE000A000)
 */

#include <arch/io.h>
#include <arch/led.h>
#include <arch/platform.h>

/* 候选 PS LED:MIO7 与 MIO8 */
#define PS_LED_MASK ((1u << 7) | (1u << 8))

void led_init(void)
{
    /* ---- PL 侧:双通道 AXI GPIO ---- */

    /*
     * ch1 接拨码开关,必须保持输入。
     * 上电 TRI 默认 0xFFFFFFFF(输入),这里显式写回以防万一 ——
     * 若误设为输出,会和拨码开关驱动对顶。
     */
    mmio_write32(PLAT_AXI_GPIO_BASE + AXI_GPIO_CH1_TRI, 0xFFFFFFFFu);

    /* ch2 接 LED,设为输出并清零 */
    mmio_write32(PLAT_AXI_GPIO_BASE + AXI_GPIO_CH2_TRI, 0x00000000u);
    mmio_write32(PLAT_AXI_GPIO_BASE + AXI_GPIO_CH2_DATA, 0x00000000u);

    /* ---- PS 侧:MIO7 / MIO8 ---- */
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
    mmio_write32(PLAT_AXI_GPIO_BASE + AXI_GPIO_CH2_DATA, (u32)value);
}

u8 sw_read(void)
{
    return (u8)(mmio_read32(PLAT_AXI_GPIO_BASE + AXI_GPIO_CH1_DATA) & 0xFFu);
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
