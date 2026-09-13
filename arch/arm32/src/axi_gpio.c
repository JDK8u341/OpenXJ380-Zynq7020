/*
 * AXI GPIO 驱动 —— 设备描述层的第一个用户
 *
 * 它同时是 M3 的验收对象:证明"描述表 → compatible 匹配 → probe →
 * 驱动拿到基址与参数"这条路径真的能跑通。
 *
 * 选它的理由:GPIO **不是设备节点** —— 不需要 device_t / VFS / 堆 / 调度器。
 * 块设备驱动要注册 device_t,而 regist_device() 依赖
 * krlcb + 堆 + mutex/调度器 + fs/partition + mm_uaccess(见计划 §4.1),
 * 那些目前一行都还没搬到 ARM 上。所以只有 GPIO 这类驱动能在当前的最小内核上
 * 把描述层验证完。
 *
 * ====================================================================
 * 这个驱动最能说明"为什么 ARM 需要描述层"
 * ====================================================================
 *
 * AXI GPIO 有两个参数**读硬件根本读不出来**:
 *
 *   xlnx,is-dual     —— 有几个通道。数据寄存器宽度恒为 32 位,
 *                       通道数不体现在任何 ID 寄存器里;
 *   xlnx,gpio-width  —— 每通道多少位。同理,硬件不告诉你。
 *
 * x86 上这类信息由 PCI capability 或 ACPI 提供;ARM 上只能被告知。
 * 本驱动用 plat_prop_get_or() 取它们,并在缺失时**拒绝认领** ——
 * 见 axi_gpio_probe 里的说明。
 */

#include <arch/io.h>
#include <arch/led.h>
#include <arch/plat_device.h>
#include <arch/types.h>

/* AXI GPIO 的寄存器布局(与具体 PL 设计无关,是 IP 自身固定的) */
#define AXI_GPIO_CH1_DATA 0x0000u
#define AXI_GPIO_CH2_DATA 0x0008u
#define AXI_GPIO_CH1_TRI 0x0004u
#define AXI_GPIO_CH2_TRI 0x000Cu

/* 认领之后记下来的基址与参数,供 led_pl_set / sw_read 使用 */
static uintptr_t g_axi_gpio_base;
static u32       g_axi_gpio_width;
static u32       g_axi_gpio_is_dual;

static int axi_gpio_probe(const plat_device_t *dev, void *ctx)
{
    u32 width;
    u32 dual;

    (void)ctx;

    /*
     * 这两个参数是**认领的前提**,不是可选优化。
     *
     * 硬件读不出来它们,所以只能从描述里拿。缺失说明描述不完整,
     * 此时**主动放弃**比用默认值猜要好:
     *   - 用默认值猜,宽度猜错会静默截断高位(LED 看起来"就是不亮");
     *   - 放弃则会被 plat_probe_all 记进 failed,并在启动日志里显示,
     *     一眼能看出"有驱动声明认识它但拒了" —— 与 unclaimed
     *     (驱动没写)明确区分开。
     *
     * 这正是 probe 返回非 0 而不是直接崩的意义。
     */
    if (!plat_prop_get(dev, "xlnx,gpio-width", &width)) {
        return -1;
    }
    if (!plat_prop_get(dev, "xlnx,is-dual", &dual)) {
        return -1;
    }

    /* 板上 LED 走的是双通道器件的 ch2;单通道器件接不了 */
    if (dual == 0u) {
        return -1;
    }

    /* 本板是 8 位;其它位宽不是不能跑,只是没验证过 */
    if (width != 8u) {
        return -1;
    }

    g_axi_gpio_base = dev->reg_base;
    g_axi_gpio_width = width;
    g_axi_gpio_is_dual = dual;

    /* ch1 接拨码开关,必须保持输入 */
    mmio_write32(g_axi_gpio_base + AXI_GPIO_CH1_TRI, 0xFFFFFFFFu);

    /* ch2 接 LED,设为输出并清零 */
    mmio_write32(g_axi_gpio_base + AXI_GPIO_CH2_TRI, 0x00000000u);
    mmio_write32(g_axi_gpio_base + AXI_GPIO_CH2_DATA, 0x00000000u);

    return PLAT_PROBE_OK;
}

const plat_driver_t axi_gpio_driver = {
    .compatible = "xlnx,axi-gpio-2.0",
    .probe = axi_gpio_probe,
    .remove = NULL,
};

/* ------------------------------------------------------------------ */
/* 驱动对外提供的操作                                                   */
/* ------------------------------------------------------------------ */

bool axi_gpio_ready(void)
{
    return g_axi_gpio_base != 0u;
}

void axi_gpio_led_write(u8 value)
{
    if (g_axi_gpio_base == 0u) {
        return;
    }

    /*
     * 按描述里的位宽截断,而不是硬编码 8 位。
     * 这样将来换成 4 位或 16 位的 PL 设计时,这里不用改。
     */
    if (g_axi_gpio_width < 32u) {
        value &= (u8)((1u << g_axi_gpio_width) - 1u);
    }

    mmio_write32(g_axi_gpio_base + AXI_GPIO_CH2_DATA, (u32)value);
}

u8 axi_gpio_switch_read(void)
{
    if (g_axi_gpio_base == 0u) {
        return 0u;
    }

    return (u8)(mmio_read32(g_axi_gpio_base + AXI_GPIO_CH1_DATA) & 0xFFu);
}

/*
 * 读回 LED 通道的值。
 *
 * 存在的意义:**证明写真的到了硬件**,而不只是"代码路径跑过了"。
 *
 * AXI GPIO 的输出通道 DATA 寄存器是可读的,读回的就是寄存器的当前值。
 * 这一点很关键,因为 M3 把 led_pl_set 从"写一个编译期常量基址"改成了
 * "写 probe 填进来的基址" —— 如果 probe 填错了地址,写入会静默落到
 * 某个无关的地方,LED 不亮,而**调用方的代码看起来一切正常**
 * (心跳里那个 LED 图案是算出来的,不是读回来的,所以它骗得过人)。
 *
 * 这与本项目踩过的 SCU/ACTLR 那个坑是同一类:寄存器位说得通,
 * 但硬件到底动没动,只能靠直接观测。
 */
u8 axi_gpio_led_read(void)
{
    if (g_axi_gpio_base == 0u) {
        return 0u;
    }

    return (u8)(mmio_read32(g_axi_gpio_base + AXI_GPIO_CH2_DATA) & 0xFFu);
}

uintptr_t axi_gpio_get_base(void)
{
    return g_axi_gpio_base;
}

u32 axi_gpio_get_width(void)
{
    return g_axi_gpio_width;
}
