#pragma once

/*
 * Zynq-7020 (AC850-CORE + AC880-CB) 板级常量
 *
 * 对应移植计划 §2.2 的「静态板级描述符」——用它替代 x86 侧的 ACPI
 * (FADT/MADT/HPET/MCFG)。Zynq 上没有 ACPI,所有资源都来自板级事实。
 *
 * ⚠ 标注 [实测] 的值是在本板 JTAG 上读寄存器实测得到的;
 *    标注 [BSP] 的来自 AMD 导出平台的 xparameters.h / 设备树。
 *    两者一致的项可以放心使用。
 */

#include <arch/types.h>

/* ================================================================== */
/* 频率                                                                */
/* ================================================================== */

/*
 * CPU 主频 666.666687 MHz。
 * [BSP] XPAR_CPU_CORE_CLOCK_FREQ_HZ = 666666687
 * [实测] 全局定时器 2 秒递增 689,673,023 次 -> 333.3 MHz = CPU/2,吻合
 */
#define PLAT_CPU_FREQ_HZ 666666687u

/* Cortex-A9 全局定时器递增频率 = CPU_3x2x = CPU/2 */
#define PLAT_GLOBAL_TIMER_FREQ_HZ (PLAT_CPU_FREQ_HZ / 2u)

/* PS_CLK 参考时钟 [BSP] 设备树 ps-clk-frequency = <33333333> */
#define PLAT_PS_CLK_FREQ_HZ 33333333u

/*
 * UART 参考时钟。
 * ⚠ 未定:SLCR UART_CLK_CTRL 实测为 0x00003F03(DIVISOR=63 -> ÷64,
 *   SRCSEL=0 -> IO PLL),但 IO PLL 的实际频率尚未标定。
 *   写 UART 驱动前必须先用实测方式确定(见 arch/arm32/README.md)。
 */
#define PLAT_UART_REF_CLK_HZ 0u /* TODO(M1): 标定后填入 */

/* ================================================================== */
/* 内存布局                                                            */
/* ================================================================== */

#define PLAT_OCM_BASE     0x00000000u /* 片上 RAM,256KB,低地址映射 */
#define PLAT_OCM_SIZE     0x00040000u
#define PLAT_OCM_HIGH     0xFFFF0000u /* 高地址镜像 */

#define PLAT_DDR_BASE     0x00100000u /* 设备树 memory@00100000 */
#define PLAT_DDR_SIZE     0x3FF00000u /* 1GB 减去低端保留区 */

/* 内核加载地址:ps7_init 完成 DDR 初始化后由 JTAG 直接下载至此 */
#define PLAT_KERNEL_LOAD  0x00100000u

/* ================================================================== */
/* 中断控制器 PL390 GIC                                                */
/* ================================================================== */

#define PLAT_GIC_DIST_BASE 0xF8F01000u
#define PLAT_GIC_CPU_BASE  0xF8F00100u

/* 每 CPU 私有中断号 */
#define PLAT_IRQ_SCU_TIMER   29u /* Cortex-A9 私有定时器 */
#define PLAT_IRQ_GLOBAL_TIMER 30u /* Cortex-A9 全局定时器 */
#define PLAT_IRQ_SCU_WDT     30u /* 同上,watchdog 复用该号 */

/* 常见外设 SPI 中断号(TRM 通用值,需按板确认) */
#define PLAT_IRQ_UART0 59u
#define PLAT_IRQ_UART1 82u
#define PLAT_IRQ_GEM0  54u
#define PLAT_IRQ_GEM1  61u
#define PLAT_IRQ_SD0   56u
#define PLAT_IRQ_SD1   45u
#define PLAT_IRQ_GPIO  52u

/* ================================================================== */
/* SCU / 定时器 / L2                                                   */
/* ================================================================== */

#define PLAT_SCU_BASE          0xF8F00000u
#define PLAT_GLOBAL_TIMER_BASE 0xF8F00200u
#define PLAT_PRIVATE_TIMER_BASE 0xF8F00600u
#define PLAT_L2CC_BASE         0xF8F02000u /* PL310 */

/* 全局定时器寄存器偏移 */
#define GT_COUNTER_LOW   0x00u
#define GT_COUNTER_HIGH  0x04u
#define GT_CONTROL       0x08u
#define GT_COMPARE_LOW   0x10u
#define GT_COMPARE_HIGH  0x14u

/* ================================================================== */
/* 系统级控制寄存器 SLCR                                               */
/* ================================================================== */

#define PLAT_SLCR_BASE       0xF8000000u
#define SLCR_LOCK            0xF8000004u
#define SLCR_UNLOCK          0xF8000008u
#define SLCR_UNLOCK_KEY      0x0000DF0Du
#define SLCR_LOCK_KEY        0x0000767Bu
#define SLCR_LVL_SHFTR_EN    0xF8000900u /* PL 电平转换器,访问 PL 前必须使能 */
#define SLCR_FPGA0_CLK_CTRL  0xF8000170u
#define SLCR_UART_CLK_CTRL   0xF8000154u
#define SLCR_APER_CLK_CTRL   0xF800012Cu /* AMBA 外设时钟门控 */
#define SLCR_OCM_CFG         0xF8000910u

/*
 * APER_CLK_CTRL 位:外设的 AMBA 总线时钟门控。
 *
 * ⚠ 本板实测结论(与直觉相反,记录以免后人重复踩坑):
 *   现有 AXI_GPIO_1 设计的 ps7_init 把 UART0/UART1 的 APER 位都关着
 *   (APER_CLK_CTRL = 0x01CC040D,bit12/bit13 = 0)。
 *   但**打开它们并不能让 UART 工作** —— 已实测:把 APER 全部置 1 后
 *   UART 寄存器仍读回 0;而 GPIO 的 APER 位同样为 0 却工作正常。
 *   真正的原因是该外设在当前 PS7 配置下不响应(见 README「已知问题」)。
 *   下面两个位定义保留,供其它 PS7 配置参考。
 */
#define APER_CLK_UART0 (1u << 12)
#define APER_CLK_UART1 (1u << 13)
#define APER_CLK_GPIO  (1u << 14)

/* ================================================================== */
/* 外设基址                                                            */
/* ================================================================== */

/* Cadence UARTPS */
#define PLAT_UART0_BASE 0xE0000000u
#define PLAT_UART1_BASE 0xE0001000u
#define UARTPS_RX_TX_FIFO 0x30u /* 收发 FIFO */
#define UARTPS_CHANNEL_STS 0x2Cu /* bit4 = TX FIFO 满 */
#define UARTPS_TX_FIFO_FULL (1u << 4)

/*
 * 控制台 UART。
 *
 * 本板 PS 调试口接的是 **UART1**,不是 UART0。已从原理图逐脚追踪确认:
 *   AC850 核心板 27 脚 (SoC 球 D11) = PS_MIO48 -> 底板 P2/27 = PS_UART_TXD
 *   AC850 核心板 45 脚 (SoC 球 C14) = PS_MIO49 -> 底板 P2/45 = PS_UART_RXD
 * 与 Zynq 标准的 UART1 = MIO48(TX)/MIO49(RX) 一致。
 */
#define PLAT_CONSOLE_UART_BASE PLAT_UART1_BASE
#define PLAT_CONSOLE_UART_MIO_TX 48u
#define PLAT_CONSOLE_UART_MIO_RX 49u

/*
 * 控制台波特率。
 *
 * 9600 而不是 115200:本板配套的调试终端默认就是 9600,
 * 发 115200 过去会因帧错误而"看起来什么都没有" —— 排查时容易被误判成
 * 接线问题。改这里即可切换,启动时的波特率扫描会依次尝试
 * 9600/19200/.../230400 各发一遍,便于快速确认终端的实际设置。
 */
#define PLAT_CONSOLE_BAUD 9600u

/* PS GPIO 控制器(MIO + EMIO) */
#define PLAT_GPIO_BASE 0xE000A000u
/* bank0 = MIO0-31 */
#define GPIO_MASK_DATA_0_LSW 0x0000u
#define GPIO_DATA_0          0x0040u
#define GPIO_DIRM_0          0x0204u
#define GPIO_OEN_0           0x0208u
/* bank1 = MIO32-53 + EMIO54-63 */
#define GPIO_DATA_1 0x0044u
#define GPIO_DIRM_1 0x0244u
#define GPIO_OEN_1  0x0248u

/* 其它 PS 外设 */
#define PLAT_QSPI_BASE 0xE000D000u
#define PLAT_SD0_BASE  0xE0100000u
#define PLAT_GEM0_BASE 0xE000B000u
#define PLAT_GEM1_BASE 0xE000C000u

/* ================================================================== */
/* 本板 PL 侧外设(来自 AXI_GPIO_1 工程,⚠ 属于具体设计而非板级事实)   */
/* ================================================================== */

/*
 * 双通道 AXI GPIO @ 0x41200000,各 8 位。
 *   ch1 (gpio_rtl_0) = 拨码开关,引脚 H15 N18 N17 N20 N19 M20 M19 M16
 *   ch2 (gpio_rtl_1) = 8 个 LED,  引脚 N22 P22 R18 T18 P20 P21 R20 R21
 * TRI 上电默认 0xFFFFFFFF(输入),必须写 0 才能输出。
 * ch1 必须保持输入,否则与拨码开关对顶。
 *
 * ⚠ 这个地址来自具体 PL 设计。真实产品中 PL 设计会变,
 *   地址应当作为配置传入而不是编译期常量(见移植计划 §2.2)。
 */
#define PLAT_AXI_GPIO_BASE 0x41200000u
#define AXI_GPIO_CH1_DATA  0x00u
#define AXI_GPIO_CH1_TRI   0x04u
#define AXI_GPIO_CH2_DATA  0x08u
#define AXI_GPIO_CH2_TRI   0x0Cu

/* 8 个 PL LED 的引脚(板级事实,来自原理图与约束文件) */
#define BOARD_LED0_PIN "N22"
#define BOARD_LED1_PIN "P22"
#define BOARD_LED2_PIN "R18"
#define BOARD_LED3_PIN "T18"
#define BOARD_LED4_PIN "P20"
#define BOARD_LED5_PIN "P21"
#define BOARD_LED6_PIN "R20"
#define BOARD_LED7_PIN "R21"

/* ================================================================== */
/* 调试用固定地址                                                      */
/* ================================================================== */

/*
 * 心跳区:放在 OCM 内,程序链接位置无关。
 * JTAG 可直接用 mrd 0x00020000 读回,无需串口。
 *
 * 槽位语义见 arch/heartbeat.h —— 那里是唯一定义处,不要在这里重复列举。
 * (这段注释原先列了一份槽位表,在心跳扩展到 16 槽之后就一直是错的:
 *  它把 [6] 写成 DIRM_0,而实际 [6] 是 UART 参考时钟。
 *  过期文档比没有文档更糟,所以改成指向定义处。)
 *
 * ⚠ 心跳区所在的低 1MB 在页表里被映射为**不可缓存**(见 src/mmu.c 的区域表),
 *   这是有意的:JTAG 走 DAP/AXI 直接读物理内存,不经过 CPU 的 L1/L2,
 *   一旦这段是写回可缓存,心跳就只是躺在 cache 里,JTAG 会读到陈旧值。
 */
#define PLAT_HEARTBEAT_BASE 0x00020000u
#define PLAT_HEARTBEAT_MAGIC 0x4F583338u /* "OXJ8" */

/*
 * 故障注入选择器。
 *
 * ⚠ 位置不是随便挑的:它必须**紧接在心跳区预留容量之后**。
 *   心跳区占地 0x00020000..0x0002007F(32 槽),选择器从 0x00020080 开始。
 *
 *   这个边界是踩出来的:最初选择器放在 0x00020040,也就是第 16 槽,
 *   而心跳后来扩展到了第 16 槽(MMU 阶段标记)——
 *   两者一旦撞上,fault_test_poll() 会把 MMU 写的进度号当成注入码,
 *   随机触发异常,而且症状是"内核莫名奇妙进了 Data Abort"。
 *   arch/heartbeat.h 里有静态断言双向锁住这条边界。
 *
 * 由 JTAG 写入以触发指定异常,见 arch/fault_test.h。
 */
#define PLAT_HEARTBEAT_REGION_SLOTS 32u
#define PLAT_FAULT_SEL_ADDR 0x00020080u
