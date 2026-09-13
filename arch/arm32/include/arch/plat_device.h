#pragma once

/*
 * 设备描述层 —— 类 DTS 的硬件描述与驱动匹配
 *
 * ====================================================================
 * 为什么 ARM 侧必须有这一层
 * ====================================================================
 *
 * x86 是**枚举**:驱动问总线"你是谁",硬件必须回答(PCI 配置空间 / ACPI 表)。
 * ARM 这边连枚举这个概念都不成立:
 *
 *   |          | x86_64                  | Zynq-7020              |
 *   |----------|-------------------------|------------------------|
 *   | 发现方式 | PCI 枚举 / ACPI         | **只能被告知**          |
 *   | 地址     | BAR,运行期分配          | Vivado 地址编辑器写死   |
 *   | 中断号   | MSI / ACPI _PRT         | 从 xparameters.h 抄     |
 *   | IP 参数  | 标准 capability 寄存器  | **读不出来**            |
 *
 * 最后一行是关键:AXI GPIO 的数据寄存器宽度恒为 32 位、通道数不体现在任何
 * ID 寄存器里 —— **硬件本身不告诉你它是什么**。
 * 所以描述不是可选优化,是唯一的信息来源。
 *
 * 没有枚举还有一个后果:x86 上漏配一个设备,枚举照样能把它找出来;
 * 这里**描述表就是全部** —— 少写一个节点,驱动静默不加载且不报错。
 * 所以本层的调用方**必须**提供"启动时打印全部节点"的能力,
 * 那是唯一能在"驱动没起来"时区分"驱动写错了"和"节点漏写"的手段。
 *
 * ====================================================================
 * 描述模型与落地形式是两个独立的轴
 * ====================================================================
 *
 *   描述模型  ← 照 DTS:节点 / compatible / reg / interrupts / 属性 / status
 *   落地形式  ← C 表 · DTB · 从 xparameters.h 生成   ← 三者可换
 *
 * 本文件只定义**模型**。当前的落地形式是"从 xparameters.h 生成的 C 表"
 * (tools/gen_board_desc.py),换 DTB 时驱动代码一行都不用动。
 */

#include <arch/types.h>

/* ------------------------------------------------------------------ */
/* 总线类型                                                             */
/* ------------------------------------------------------------------ */

/*
 * 当前 Zynq 是"一条 APB + 一条 AXI GP"的扁平结构,所以这里只是个分类标签,
 * 不参与地址翻译。等 PL 里挂上 AXI 互联,parent/bus 才会真正起作用。
 */
typedef enum
{
    PLAT_BUS_NONE = 0,
    PLAT_BUS_APB,
    PLAT_BUS_AXI,
} plat_bus_t;

/* 表示"该设备没有中断" */
#define PLAT_IRQ_NONE ((i32)-1)

/* ------------------------------------------------------------------ */
/* 属性(对应 DTS 里的自定义属性)                                       */
/* ------------------------------------------------------------------ */

/*
 * 这类属性是**硬件读不出来**的,只能由描述提供。
 * 例:`xlnx,is-dual`、`xlnx,gpio-width`。
 *
 * 目前只支持 32 位整数。字符串属性(如时钟名)先用独立字段
 * (plat_device_t.clocks)表达,等真需要时再扩成带类型的联合。
 */
typedef struct
{
    const char *name; /* 如 "xlnx,gpio-width" */
    u32         value; /* 如 8 */
} plat_prop_t;

/* ------------------------------------------------------------------ */
/* 设备节点(对应 DTS 里的一个节点)                                     */
/* ------------------------------------------------------------------ */

typedef struct plat_device
{
    /* --- 标识 --- */
    const char *name;       /* 节点名,如 "axi_gpio_0"。仅用于打印与诊断 */
    const char *compatible; /* 驱动匹配依据,如 "xlnx,axi-gpio-2.0"     */

    /*
     * --- reg = <base size> ---
     * 地址来自 Vivado 地址编辑器,是任意的、不可探测的。
     * reg_base == 0 视为"无寄存器区间"(如纯中断设备)。
     */
    uintptr_t reg_base;
    size_t    reg_size;

    /* --- interrupts --- */
    /*
     * ⚠ 这里是**已经解码好的 GIC INTID**,不是 xparameters.h 里的
     *   XPAR_*_INTERRUPTS 原始值。
     *
     *   原始值是编码过的:
     *       bits[11:0]  = 相对中断号
     *       bits[15:12] = 触发类型
     *       bit20       = 0 = SPI, 1 = PPI
     *       GIC INTID   = 相对号 + (SPI ? 32 : PPI ? 16 : 0)
     *
     *   解码放在生成器里,不放驱动里:差 32 的错会让驱动挂到一个
     *   完全无关的中断上,而这种错误不会在编译期暴露。
     *   本板实例:SCUTIMER 0x13100d -> 29(私有定时器)、QSPI 0x4013 -> 51。
     */
    i32 irq;
    u32 irq_flags; /* 1 = 电平,3 = 边沿(取自同一个编码的 bits[15:12]) */

    /* --- clocks --- */
    const char *clocks; /* 时钟源名,如 "uart_clk";无则 NULL */

    /* --- 自定义属性 --- */
    const plat_prop_t *props;
    u32                prop_count;

    /* --- 层级(预留) --- */
    /*
     * 当前 Zynq 是扁平的,parent 恒为 NULL。
     * 留着是为了将来 PL 里挂 AXI 互联时不用改结构体布局 ——
     * 改结构体意味着所有生成物都要重新生成一遍。
     */
    const char *parent;
    plat_bus_t  bus;

    /* --- status = "okay" / "disabled" --- */
    /*
     * PL 未启用期间,PL 节点一律 false。驱动匹配会跳过它们,
     * 但"打印全部节点"仍然会把它们列出来并标注状态。
     */
    bool enabled;
} plat_device_t;

/* ------------------------------------------------------------------ */
/* 驱动(对应 Linux 的 of_match_table)                                 */
/* ------------------------------------------------------------------ */

/*
 * probe 返回 0 = 认领成功;非 0 = 主动放弃,**匹配循环会继续尝试下一个驱动**。
 * 这样同一个 compatible 可以有多个驱动按优先级排列。
 */
typedef struct plat_driver
{
    const char *compatible;
    int (*probe)(const plat_device_t *dev, void *ctx);
    void (*remove)(const plat_device_t *dev);
} plat_driver_t;

/* probe 的上下文由调用方提供,原样透传给每个 probe */
#define PLAT_PROBE_OK 0

/* ------------------------------------------------------------------ */
/* 纯逻辑接口(实现在 src/plat_device.c,宿主机可单测)                */
/* ------------------------------------------------------------------ */

/*
 * compatible 匹配。
 *
 * 采用**精确字符串相等**,不做前缀/通配匹配 —— 这是 DTS 的语义,
 * 而且可预测:通配匹配会让"为什么这个驱动认领了那个设备"变得难查。
 */
bool plat_compatible_match(const char *driver_compatible, const char *device_compatible);

/* 设备是否参与匹配(enabled 且 compatible 非空) */
bool plat_device_is_active(const plat_device_t *dev);

/*
 * 按名字查属性。
 * 找到返回 true 并写入 *out;未找到返回 false(此时不改 *out)。
 */
bool plat_prop_get(const plat_device_t *dev, const char *name, u32 *out);

/* 带默认值的版本:未找到时返回 default_value */
u32 plat_prop_get_or(const plat_device_t *dev, const char *name, u32 default_value);

/* 一次 probe 遍历的统计结果 */
typedef struct
{
    u32 total;     /* 表里的节点总数 */
    u32 disabled;  /* enabled == false,被跳过 */
    u32 probed;    /* 被某个驱动成功认领 */
    u32 unclaimed; /* 没有任何驱动认领 */
    u32 failed;    /* 有驱动匹配,但所有匹配的 probe 都返回了非 0 */
} plat_probe_stats_t;

/*
 * 遍历设备表,为每个活跃设备找到第一个认领它的驱动。
 *
 * 匹配顺序是**确定的**,便于复现:
 *   外层按设备表顺序,内层按驱动表顺序;
 *   同一设备遇到 probe 失败会继续尝试下一个匹配的驱动。
 *
 * stats 可为 NULL。
 */
void plat_probe_all(const plat_device_t *devices, u32 device_count,
                    const plat_driver_t *drivers, u32 driver_count,
                    void *ctx, plat_probe_stats_t *stats);
