#pragma once

/*
 * 设备描述表 —— **本文件由 tools/gen_board_desc.py 生成,不要手改**
 *
 * 重新生成:
 *     python tools/gen_board_desc.py arch/arm32/board/xparameters.h \
 *         --out-c arch/arm32/src/board_devices.c --out-h arch/arm32/include/arch/board_devices.h
 *
 * 输入是 Vitis 从 XSA 导出的 xparameters.h —— 它本身就是一份已经扁平化的
 * DTS 表示。改硬件之后重新导出并重跑生成器,不要手改本文件。
 *
 * 模型定义与设计理由见 arch/arm32/include/arch/plat_device.h。
 */

#include <arch/plat_device.h>

/* 全部设备节点,按 xparameters.h 的实例名排序 */
extern const plat_device_t g_board_devices[10];
extern const u32            g_board_device_count;

/*
 * 驱动汇总入口。
 *
 * 具体驱动在各自的源文件里,这里只声明一个聚合点 ——
 * 描述表与驱动表都由调用方交给 plat_probe_all(),
 * 本层不依赖具体驱动。(当前尚无驱动接入,定义见 kmain 的登记处)
 */
extern const plat_driver_t g_board_drivers[];
extern const u32            g_board_driver_count;
