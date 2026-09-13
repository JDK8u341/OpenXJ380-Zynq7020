"""设备描述层匹配逻辑的单元测试。

被测代码是 arch/arm32/src/plat_device.c —— 完全不含 MMIO/内联汇编,
所以能直接用宿主编译器编译运行。

**为什么这套逻辑值得测**:匹配循环里全是"看起来显然"的比较与遍历,
但它们的**顺序**是有语义的:

  - 两个驱动声明同一个 compatible 时谁赢?
  - 排在前面的驱动 probe 返回非 0 之后,要不要继续试下一个?
  - enabled = false 的节点算不算"没人认领"?
  - compatible 为空(NULL)的驱动会不会认领 compatible 为空的设备?

这些一旦写反**都不会报错**,只表现为"某个驱动莫名其妙没起来"或者
"某个完全无关的驱动初始化了"。在没有枚举的 ARM 上,这两种症状都极难定位 ——
因为不存在"总线扫描"这个可以对照的参照物。

参考:
  arch/arm32/include/arch/plat_device.h
  docs/ZYNQ7020_PORT_PLAN.md §2.16
"""

from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

HARNESS = r"""
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <arch/plat_device.h>

static int failures;

static void check(int condition, const char *what)
{
    if (!condition) {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

static void check_u32(unsigned int got, unsigned int want, const char *what)
{
    if (got != want) {
        printf("FAIL: %s (got %u, want %u)\n", what, got, want);
        failures++;
    }
}

/* ================= 调用记录器 ================= */
/* 用来断言"谁按什么顺序被 probe 了" —— 这是本文件的核心 */
#define TRACE_MAX 16

static char        trace_dev[TRACE_MAX][32];
static char        trace_drv[TRACE_MAX][32];
static unsigned    trace_len;

static void trace_reset(void)
{
    trace_len = 0;
}

static void trace_add(const char *dev, const char *drv)
{
    if (trace_len < TRACE_MAX) {
        snprintf(trace_dev[trace_len], 32, "%s", dev ? dev : "(null)");
        snprintf(trace_drv[trace_len], 32, "%s", drv ? drv : "(null)");
        trace_len++;
    }
}

static int trace_count(void) { return (int)trace_len; }

static int trace_is(int idx, const char *dev, const char *drv)
{
    if (idx < 0 || idx >= (int)trace_len) {
        return 0;
    }
    return strcmp(trace_dev[idx], dev) == 0 && strcmp(trace_drv[idx], drv) == 0;
}

/* ================= 假驱动 ================= */

/* 认领一切 */
static int probe_accept(const plat_device_t *dev, void *ctx)
{
    (void)ctx;
    trace_add(dev->name, dev->compatible);
    return PLAT_PROBE_OK;
}

/* 拒绝一切 */
static int probe_reject(const plat_device_t *dev, void *ctx)
{
    (void)ctx;
    trace_add(dev->name, dev->compatible);
    return -1;
}

/* 按设备的某个属性决定认不认领 —— 用来测"同 compatible 多驱动"的情形 */
static int probe_want_dual(const plat_device_t *dev, void *ctx)
{
    u32 dual;

    (void)ctx;
    trace_add(dev->name, dev->compatible);

    dual = plat_prop_get_or(dev, "xlnx,is-dual", 0u);
    return (dual != 0u) ? PLAT_PROBE_OK : -1;
}

/* 注意:驱动汇总表是**指针数组** —— C 不允许用结构体变量初始化结构体数组,
 * 而驱动要能各自住在自己的翻译单元里。见 plat_device.h。 */
static const plat_driver_t drv_uart_ok = {.compatible = "xlnx,ps7-uart", .probe = probe_accept};
static const plat_driver_t drv_gpio_ok = {.compatible = "xlnx,axi-gpio-2.0", .probe = probe_accept};
static const plat_driver_t drv_gpio_dual = {.compatible = "xlnx,axi-gpio-2.0", .probe = probe_want_dual};
static const plat_driver_t drv_gpio_rej = {.compatible = "xlnx,axi-gpio-2.0", .probe = probe_reject};
static const plat_driver_t drv_uart_noprobe = {.compatible = "xlnx,ps7-uart", .probe = NULL};
static const plat_driver_t drv_other = {.compatible = "acme,other", .probe = probe_accept};

/* ================= 测试数据 ================= */

static const plat_prop_t gpio_props[] = {
    {"xlnx,is-dual", 1u},
    {"xlnx,gpio-width", 8u},
};

#define DEV_UART   {.name = "uart1", .compatible = "xlnx,ps7-uart", \
                    .reg_base = 0xE0001000u, .reg_size = 0x1000u, \
                    .irq = 26, .irq_flags = 1u, .clocks = "uart_clk", \
                    .bus = PLAT_BUS_APB, .enabled = true}

#define DEV_GPIO   {.name = "axi_gpio_0", .compatible = "xlnx,axi-gpio-2.0", \
                    .reg_base = 0x41200000u, .reg_size = 0x10000u, \
                    .irq = PLAT_IRQ_NONE, \
                    .props = gpio_props, .prop_count = 2u, \
                    .bus = PLAT_BUS_AXI, .enabled = true}

int main(void)
{
    /* ============================================================== */
    /* 1. compatible 匹配:精确相等,且 NULL 绝不匹配                  */
    /* ============================================================== */
    check(plat_compatible_match("xlnx,ps7-uart", "xlnx,ps7-uart"), "identical strings match");
    check(!plat_compatible_match("xlnx,ps7-uart", "xlnx,ps7-uart-1.00.a"),
          "a different revision string must NOT match");
    check(!plat_compatible_match("xlnx,ps7-uart", "xlnx,ps7-uar"),
          "a prefix must NOT match (exact equality only)");
    check(!plat_compatible_match("xlnx,ps7-uart", "xlnx,ps7-uartx"),
          "a longer string must NOT match");

    /*
     * NULL 与空串。
     *
     * "NULL 匹配 NULL" 如果被判为相等,一个没写 compatible 的驱动
     * 就会认领所有没写 compatible 的设备 —— 症状是驱动乱入。
     * 这一条是本次单测里最值得钉住的边界。
     */
    check(!plat_compatible_match(NULL, NULL), "NULL must not match NULL");
    check(!plat_compatible_match("xlnx,ps7-uart", NULL), "NULL device must not match");
    check(!plat_compatible_match(NULL, "xlnx,ps7-uart"), "NULL driver must not match");

    /* ============================================================== */
    /* 2. 节点是否参与匹配:enabled 与 compatible 缺一不可             */
    /* ============================================================== */
    {
        plat_device_t ok = DEV_UART;
        plat_device_t disabled = DEV_UART;
        plat_device_t no_compat = DEV_UART;

        disabled.enabled = false;
        no_compat.compatible = NULL;

        check(plat_device_is_active(&ok), "an enabled node with compatible is active");
        check(!plat_device_is_active(&disabled), "disabled node is not active");
        check(!plat_device_is_active(&no_compat), "node without compatible is not active");
        check(!plat_device_is_active(NULL), "NULL node is not active");
    }

    /* ============================================================== */
    /* 3. 属性查询                                                    */
    /* ============================================================== */
    {
        plat_device_t gpio = DEV_GPIO;
        u32           v = 0xDEADBEEFu;

        check(plat_prop_get(&gpio, "xlnx,is-dual", &v), "is-dual must be found");
        check_u32(v, 1u, "is-dual value");

        check(plat_prop_get(&gpio, "xlnx,gpio-width", &v), "gpio-width must be found");
        check_u32(v, 8u, "gpio-width value");

        /* 查不到时不得改写输出参数 */
        v = 0xDEADBEEFu;
        check(!plat_prop_get(&gpio, "xlnx,nonexistent", &v), "missing prop must return false");
        check_u32(v, 0xDEADBEEFu, "missing prop must not touch the out parameter");

        check_u32(plat_prop_get_or(&gpio, "xlnx,gpio-width", 32u), 8u, "found -> value");
        check_u32(plat_prop_get_or(&gpio, "xlnx,missing", 32u), 32u, "missing -> default");

        /* 没有 props 数组的节点不能崩 */
        {
            plat_device_t uart = DEV_UART;

            check(!plat_prop_get(&uart, "anything", &v), "node without props returns false");
            check_u32(plat_prop_get_or(&uart, "anything", 7u), 7u, "node without props -> default");
        }
    }

    /* ============================================================== */
    /* 4. probe 遍历:四种归宿                                        */
    /* ============================================================== */
    {
        /* uart 有驱动, gpio 有驱动, orphan 没有驱动 */
        static const plat_device_t devs[] = {DEV_UART, DEV_GPIO,
            {.name = "orphan", .compatible = "acme,no-driver", .enabled = true},
            {.name = "pl_thing", .compatible = "xlnx,axi-gpio-2.0", .enabled = false}};

        static const plat_driver_t *const drvs[] = {&drv_uart_ok, &drv_gpio_ok};

        plat_probe_stats_t st;

        trace_reset();
        plat_probe_all(devs, 4u, drvs, 2u, NULL, &st);

        check_u32(st.total, 4u, "total counts every node, including disabled");
        check_u32(st.disabled, 1u, "the disabled node is counted as disabled");
        check_u32(st.probed, 2u, "uart and gpio are probed");
        check_u32(st.unclaimed, 1u, "orphan has no matching driver");
        check_u32(st.failed, 0u, "nobody declined");

        /*
         * total 必须等于四项之和 —— 这是调用方交叉验证
         * "是不是每个节点都有了归宿"的依据。
         */
        check_u32(st.total, st.disabled + st.probed + st.unclaimed + st.failed,
                  "the four outcomes must add up to total");

        /* 只有活跃且有人认领的设备才被 probe */
        check_u32((unsigned int)trace_count(), 2u, "exactly two probes happened");
        check(trace_is(0, "uart1", "xlnx,ps7-uart"), "uart probed first (table order)");
        check(trace_is(1, "axi_gpio_0", "xlnx,axi-gpio-2.0"), "gpio probed second");
    }

    /* ============================================================== */
    /* 5. probe 返回非 0 时必须继续尝试下一个驱动                     */
    /* ============================================================== */
    {
        static const plat_device_t devs[] = {DEV_GPIO};

        /*
         * 两个驱动声明同一个 compatible:排在前面的只认双通道版本,
         * 排在后面的通吃。is-dual = 1 所以第一个就该认领。
         */
        static const plat_driver_t *const drvs_dual_first[] = {&drv_gpio_dual, &drv_gpio_ok};

        {
            plat_probe_stats_t st;

            trace_reset();
            plat_probe_all(devs, 1u, drvs_dual_first, 2u, NULL, &st);

            check_u32(st.probed, 1u, "specialised driver claims it");
            check_u32((unsigned int)trace_count(), 1u, "the generic driver must NOT be tried");
            check(trace_is(0, "axi_gpio_0", "xlnx,axi-gpio-2.0"), "the first matching driver wins");
        }

        /*
         * 反过来:第一个驱动拒绝(null 指针之外的理由),第二个必须接住。
         * 这正是 Linux 的语义 —— 一个驱动可以因为"硬件版本不对"而放弃。
         */
        {
            static const plat_driver_t *const drvs_reject_first[] = {&drv_gpio_rej, &drv_gpio_ok};
            plat_probe_stats_t st;

            trace_reset();
            plat_probe_all(devs, 1u, drvs_reject_first, 2u, NULL, &st);

            check_u32(st.probed, 1u, "the second driver claims it after the first declines");
            check_u32(st.failed, 0u, "a declined-then-claimed device is not 'failed'");
            check_u32((unsigned int)trace_count(), 2u, "both drivers were tried");
            check(trace_is(0, "axi_gpio_0", "xlnx,axi-gpio-2.0"), "first attempt recorded");
            check(trace_is(1, "axi_gpio_0", "xlnx,axi-gpio-2.0"), "second attempt recorded");
        }

        /*
         * 全部拒绝 -> failed,而不是 unclaimed。
         * 两者要分开:unclaimed 是"描述表漏写了驱动",
         * failed 是"驱动写了但不认这个硬件" —— 排查方向完全不同。
         */
        {
            static const plat_driver_t *const drvs_all_reject[] = {&drv_gpio_rej};
            plat_probe_stats_t st;

            trace_reset();
            plat_probe_all(devs, 1u, drvs_all_reject, 1u, NULL, &st);

            check_u32(st.failed, 1u, "all drivers declining -> failed");
            check_u32(st.unclaimed, 0u, "must not be reported as unclaimed");
            check_u32(st.probed, 0u, "nothing claimed it");
        }
    }

    /* ============================================================== */
    /* 6. 驱动声明认识但 probe 为 NULL:算 unclaimed 还是 failed?      */
    /* ============================================================== */
    {
        static const plat_device_t devs[] = {DEV_UART};
        static const plat_driver_t *const drvs[] = {&drv_uart_noprobe};
        plat_probe_stats_t st;

        trace_reset();
        plat_probe_all(devs, 1u, drvs, 1u, NULL, &st);

        /*
         * 声明了 compatible 却没有 probe 函数 —— 算 failed。
         * 把这种半成品驱动报成 unclaimed 会掩盖真正的问题:
         * 排查的时候会去翻描述表,而错误其实在驱动表里。
         */
        check_u32(st.failed, 1u, "a driver with no probe function counts as failed");
        check_u32((unsigned int)trace_count(), 0u, "nothing was called");
    }

    /* ============================================================== */
    /* 7. 空表与 NULL:不得崩溃                                        */
    /* ============================================================== */
    {
        static const plat_driver_t *const drvs[] = {&drv_other};
        plat_probe_stats_t st;

        plat_probe_all(NULL, 0u, drvs, 1u, NULL, &st);
        check_u32(st.total, 0u, "NULL device table -> zero total");

        plat_probe_all(NULL, 0u, NULL, 0u, NULL, &st);
        check_u32(st.total, 0u, "all NULL -> zero total");

        /* stats 传 NULL 也不能崩 */
        plat_probe_all(NULL, 0u, NULL, 0u, NULL, NULL);
        check(1, "NULL stats is tolerated");
    }

    if (failures == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", failures);
    return 1;
}
"""


class Arm32PlatDeviceTests(unittest.TestCase):
    COMPILER_CANDIDATES = ("gcc", "cc", "clang")

    def _compile_and_run(self, source: str, extra_sources: list[Path]) -> str:
        capture = {"capture_output": True, "text": True, "encoding": "utf-8", "errors": "replace"}

        with tempfile.TemporaryDirectory() as tmp:
            harness = Path(tmp) / "plat_device_test.c"
            binary = Path(tmp) / "plat_device_test"
            harness.write_text(source, encoding="utf-8")

            attempts: list[str] = []
            for compiler in self.COMPILER_CANDIDATES:
                command = [
                    compiler,
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "arch/arm32/include"),
                    str(harness),
                    *[str(path) for path in extra_sources],
                    "-o",
                    str(binary),
                ]
                try:
                    result = subprocess.run(command, **capture)
                except FileNotFoundError:
                    attempts.append(f"{compiler}: not found")
                    continue

                if result.returncode == 0:
                    break

                detail = (result.stderr or "").strip().replace("\n", " ")[:300]
                attempts.append(f"{compiler}: rc={result.returncode} {detail}")
            else:
                self.fail("no working host C compiler found:\n  " + "\n  ".join(attempts))

            run_result = subprocess.run([str(binary)], **capture)
            self.assertEqual(
                run_result.returncode,
                0,
                f"platform device checks failed:\n{run_result.stdout}{run_result.stderr}",
            )
            return run_result.stdout

    def test_match_and_probe(self) -> None:
        output = self._compile_and_run(HARNESS, [ROOT / "arch/arm32/src/plat_device.c"])
        self.assertIn("ALL PASS", output)


if __name__ == "__main__":
    unittest.main()
