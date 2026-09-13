#pragma once

/*
 * 板级胶水层 —— 把生成的设备描述表与具体驱动接起来
 *
 * 分工:
 *   arch/arm32/board/xparameters.h      输入(从 XSA 导出,固化进仓库)
 *   arch/arm32/src/board_devices.c      生成物(设备描述表,**不要手改**)
 *   arch/arm32/src/board.c              本文件对应的实现:驱动表 + 诊断输出
 */

#include <arch/plat_device.h>
#include <arch/types.h>

/*
 * 打印全部设备节点。
 *
 * **这不是调试装饰,是描述层的必需件。**
 *
 * x86 上漏配一个设备,PCI 枚举照样能把它找出来;
 * 而 ARM 这边描述表就是全部 —— 少写一个节点,驱动静默不加载
 * 且不报任何错。所以"驱动没起来"时只有两种可能:
 *   (a) 节点在表里,但驱动没写对;
 *   (b) 节点根本不在表里(生成输入里就没有)。
 * 打印这张表是区分两者的唯一手段。
 *
 * 因此**无论驱动是否加载成功都要打印**,包括 enabled = false 的节点 ——
 * "被跳过的节点"同样是重要信息。
 */
void board_dump_devices(void);

/*
 * 跑一遍驱动匹配,然后打印结果。
 *
 * 返回被成功认领的设备数。返回 0 不代表出错:
 * 当前大部分节点(ADC/DEVCFG/DMAC/QSPI/SDHCI…)本来就没有驱动。
 */
u32 board_probe_all(void);
