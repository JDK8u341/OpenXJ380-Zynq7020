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

#include <arch/board_devices.h>

static const plat_prop_t xpar_axi_gpio_0_props[] = {
    {"xlnx,interrupt-present", 0u},
    {"xlnx,is-dual", 1u},
    {"xlnx,gpio-width", 8u},
};

static const plat_prop_t xpar_intc_props[] = {
    {"arm,baseaddr-1", 4176478464u},
};

static const plat_prop_t xpar_qspi_props[] = {
    {"xlnx,clock-freq", 200000000u},
    {"xlnx,connection-mode", 0u},
    {"xlnx,qspi-mode", 0u},
    {"xlnx,qspi-bus-width", 2u},
};

static const plat_prop_t xpar_sdhci0_props[] = {
    {"arasan,sdio-clk-freq-hz", 100000000u},
    {"arasan,has-cd", 0u},
    {"arasan,has-wp", 0u},
    {"arasan,clocks", 21u},
};

const plat_device_t g_board_devices[10] = {
    {
        .name       = "adc",
        .compatible = "xlnx,zynq-xadc-1.00.a",
        .reg_base   = 0xF8007100u,
        .reg_size   = 0x00000020u,
        .irq        = PLAT_IRQ_NONE,
        .irq_flags  = 0u,
        .clocks     = NULL,  /* xparameters.h 无对应概念,见文件头 */
        .props      = NULL,
        .prop_count = 0u,
        .parent     = NULL,
        .bus        = PLAT_BUS_APB,
        .enabled    = true,
    },
    {
        .name       = "axi_gpio_0",
        .compatible = "xlnx,axi-gpio-2.0",
        .reg_base   = 0x41200000u,
        .reg_size   = 0x00010000u,
        .irq        = PLAT_IRQ_NONE,
        .irq_flags  = 0u,
        .clocks     = NULL,  /* xparameters.h 无对应概念,见文件头 */
        .props      = xpar_axi_gpio_0_props,
        .prop_count = 3u,
        .parent     = NULL,
        .bus        = PLAT_BUS_AXI,
        .enabled    = false,
    },
    {
        .name       = "coresight",
        .compatible = "xlnx,ps7-coresight-comp-1.00.a",
        .reg_base   = 0xF8800000u,
        .reg_size   = 0x00100000u,
        .irq        = PLAT_IRQ_NONE,
        .irq_flags  = 0u,
        .clocks     = NULL,  /* xparameters.h 无对应概念,见文件头 */
        .props      = NULL,
        .prop_count = 0u,
        .parent     = NULL,
        .bus        = PLAT_BUS_APB,
        .enabled    = true,
    },
    {
        .name       = "devcfg",
        .compatible = "xlnx,zynq-devcfg-1.0",
        .reg_base   = 0xF8007000u,
        .reg_size   = 0x00000100u,
        .irq        = 40,  /* SPI rel=8 +32 */
        .irq_flags  = 4u,
        .clocks     = NULL,  /* xparameters.h 无对应概念,见文件头 */
        .props      = NULL,
        .prop_count = 0u,
        .parent     = NULL,
        .bus        = PLAT_BUS_APB,
        .enabled    = true,
    },
    {
        .name       = "dmac_s",
        .compatible = "arm,pl330",
        .reg_base   = 0xF8003000u,
        .reg_size   = 0x00001000u,
        .irq        = 45,  /* SPI rel=13 +32 */
        .irq_flags  = 4u,
        .clocks     = NULL,  /* xparameters.h 无对应概念,见文件头 */
        .props      = NULL,
        .prop_count = 0u,
        .parent     = NULL,
        .bus        = PLAT_BUS_APB,
        .enabled    = true,
    },
    {
        .name       = "intc",
        .compatible = "arm,cortex-a9-gic",
        .reg_base   = 0xF8F01000u,
        .reg_size   = 0x00001000u,
        .irq        = PLAT_IRQ_NONE,
        .irq_flags  = 0u,
        .clocks     = NULL,  /* xparameters.h 无对应概念,见文件头 */
        .props      = xpar_intc_props,
        .prop_count = 1u,
        .parent     = NULL,
        .bus        = PLAT_BUS_APB,
        .enabled    = true,
    },
    {
        .name       = "qspi",
        .compatible = "xlnx,zynq-qspi-1.0",
        .reg_base   = 0xE000D000u,
        .reg_size   = 0x00001000u,
        .irq        = 51,  /* SPI rel=19 +32 */
        .irq_flags  = 4u,
        .clocks     = NULL,  /* xparameters.h 无对应概念,见文件头 */
        .props      = xpar_qspi_props,
        .prop_count = 4u,
        .parent     = NULL,
        .bus        = PLAT_BUS_APB,
        .enabled    = true,
    },
    {
        .name       = "scutimer",
        .compatible = "arm,cortex-a9-twd-timer",
        .reg_base   = 0xF8F00600u,
        .reg_size   = 0x00000020u,
        .irq        = 29,  /* PPI rel=13 +16 */
        .irq_flags  = 1u,
        .clocks     = NULL,  /* xparameters.h 无对应概念,见文件头 */
        .props      = NULL,
        .prop_count = 0u,
        .parent     = NULL,
        .bus        = PLAT_BUS_APB,
        .enabled    = true,
    },
    {
        .name       = "scuwdt",
        .compatible = "xlnx,ps7-scuwdt-1.00.a",
        .reg_base   = 0xF8F00620u,
        .reg_size   = 0x000000E0u,
        .irq        = 30,  /* PPI rel=14 +16 */
        .irq_flags  = 4u,
        .clocks     = NULL,  /* xparameters.h 无对应概念,见文件头 */
        .props      = NULL,
        .prop_count = 0u,
        .parent     = NULL,
        .bus        = PLAT_BUS_APB,
        .enabled    = true,
    },
    {
        .name       = "sdhci0",
        .compatible = "arasan,sdhci-8.9a",
        .reg_base   = 0xE0100000u,
        .reg_size   = 0x00001000u,
        .irq        = PLAT_IRQ_NONE,
        .irq_flags  = 0u,
        .clocks     = NULL,  /* xparameters.h 无对应概念,见文件头 */
        .props      = xpar_sdhci0_props,
        .prop_count = 4u,
        .parent     = NULL,
        .bus        = PLAT_BUS_APB,
        .enabled    = true,
    },
};

const u32 g_board_device_count = 10u;
