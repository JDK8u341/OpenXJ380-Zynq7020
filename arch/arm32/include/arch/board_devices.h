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
/*
 * ⚠ 非 const:enabled 字段允许运行时修正。
 *
 * 生成器描述的是**设计里有什么**(xparameters.h 的来源),
 * 而"比特流是否真的已加载"是运行时事实 —— 两者不一定一致。
 * 所以 PL 节点默认 enabled = false,由 board.c 按实际加载的
 * 比特流在启动时修正。见 arch/arm32/src/board.c。
 */
extern plat_device_t g_board_devices[10];
extern const u32     g_board_device_count;

/*
 * 驱动汇总入口 —— **指针数组**。
 *
 * 为什么不是结构体数组:C 不允许用结构体变量初始化结构体数组,
 * 而驱动必须能各自住在自己的翻译单元里。见 plat_device.h。
 *
 * 定义在 arch/arm32/src/board.c(手写),因为驱动是代码不是数据 ——
 * 生成器只该产出"硬件是什么",不该产出"谁去驱动它"。
 */
extern const plat_driver_t *const g_board_drivers[];
extern const u32                  g_board_driver_count;
