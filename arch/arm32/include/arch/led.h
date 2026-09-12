#pragma once

/*
 * AC880-CB 底板 LED 与拨码开关
 *
 * 硬件事实(原理图 + AXI_GPIO_1_BSP.xdc 交叉确认):
 *   PL 侧 8 个 LED    : LED0..LED7 -> N22 P22 R18 T18 P20 P21 R20 R21
 *   走双通道 AXI GPIO @ 0x41200000
 *     ch1 = 拨码开关(输入)  ch2 = LED(输出)
 *   PS 侧 MIO7 / MIO8 : L3_SEL=0 配置为 GPIO,可作 PS LED
 *
 * ⚠ AXI GPIO 的基址与通道分配来自具体 PL 设计,不是板级固有属性。
 *   换一个比特流就可能完全不同(见 arch/arm32/README.md)。
 *   这里之所以能用,是因为当前加载的是 AXI_GPIO_1 设计的比特流。
 */

#include <arch/types.h>

/* 配置 GPIO 方向:PL 的 ch1 输入 / ch2 输出,PS 的 MIO7/MIO8 输出 */
void led_init(void);

/* 写 8 个 PL LED,bit0 -> LED0 */
void led_pl_set(u8 value);

/* 读 8 位拨码开关,bit0 -> SW0 */
u8 sw_read(void);

/* 控制两个候选 PS LED(MIO7 / MIO8) */
void led_ps_set(bool on);

/* 诊断:读回 GPIO 方向寄存器,用于确认配置生效 */
u32 led_get_dirm0(void);
u32 led_get_oen0(void);
