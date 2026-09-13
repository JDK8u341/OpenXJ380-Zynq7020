#pragma once

/*
 * AXI GPIO 驱动 —— 设备描述层的第一个用户
 *
 * 注册方式:它自己不被"注册",而是通过 src/board.c 的 g_board_drivers[]
 * 参与统一匹配。驱动只声明"我认识 compatible = xlnx,axi-gpio-2.0",
 * 基址与参数(通道数、位宽)由描述层在 probe 时交给它。
 *
 * 与 led.c 的分工:
 *   axi_gpio.c —— PL 侧 AXI GPIO,基址来自设备描述层
 *   led.c      —— PS 侧 MIO7/MIO8,板级固有属性,不走描述层
 */

#include <arch/types.h>

/* 驱动是否已经成功 probe(即描述层找到了它并接受了参数) */
bool axi_gpio_ready(void);

/* 写 LED 通道。值会按描述里的位宽截断 */
void axi_gpio_led_write(u8 value);

/* 读回 LED 通道 —— 用于证明写真的到了硬件,而不是只有代码跑过 */
u8 axi_gpio_led_read(void);

/* 读拨码开关通道 */
u8 axi_gpio_switch_read(void);

/* 诊断:probe 之后拿到的基址与位宽(未 probe 时基址为 0) */
uintptr_t axi_gpio_get_base(void);
u32       axi_gpio_get_width(void);
