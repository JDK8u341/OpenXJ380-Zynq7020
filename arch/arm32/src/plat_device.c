/*
 * 设备描述层 —— 纯逻辑实现
 *
 * 本文件**不含任何 MMIO / 内联汇编**,所以宿主编译器能直接编译它,
 * 单测见 tests/test_arm32_plat_device.py。
 *
 * 值得单测的原因:匹配逻辑里全是"看起来显然"的循环与比较,
 * 但它们的**顺序**是有语义的 —— 两个驱动声明同一个 compatible 时谁赢、
 * probe 失败后要不要继续试下一个、enabled=false 的节点是否参与统计,
 * 这些一旦写反都不会报错,只表现为"某个驱动莫名其妙没起来"。
 */

#include <arch/plat_device.h>

/* ------------------------------------------------------------------ */
/* 字符串比较                                                           */
/* ------------------------------------------------------------------ */

/*
 * freestanding 构建没有 libc,所以这里自带一个最小的字符串比较。
 * 不复用 console.c 里的任何东西:那个文件属于控制台,本文件要保持纯净
 * (能被宿主单测直接编译)。
 */
static int plat_str_eq(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return 0;
    }

    while (*a != '\0' && *b != '\0') {
        if (*a != *b) {
            return 0;
        }
        a++;
        b++;
    }

    /* 两者必须同时结束,否则是前缀关系而不是相等 */
    return (*a == '\0' && *b == '\0') ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* 匹配与查询                                                           */
/* ------------------------------------------------------------------ */

bool plat_compatible_match(const char *driver_compatible, const char *device_compatible)
{
    /*
     * 任意一边为空都不匹配。
     *
     * 这一条不是防御性编程:生成器在 IP 没有 compatible 属性时会写出 NULL,
     * 而"NULL 匹配 NULL"如果被判为相等,一个没写 compatible 的驱动
     * 就会认领所有没写 compatible 的设备 —— 症状是驱动乱入,
     * 而表象是"某个完全无关的驱动初始化了"。
     */
    if (driver_compatible == NULL || device_compatible == NULL) {
        return false;
    }

    return plat_str_eq(driver_compatible, device_compatible) != 0;
}

bool plat_device_is_active(const plat_device_t *dev)
{
    if (dev == NULL) {
        return false;
    }

    /* compatible 为空说明这个节点没有任何驱动能认领,不必参与匹配 */
    if (dev->compatible == NULL || dev->compatible[0] == '\0') {
        return false;
    }

    return dev->enabled;
}

bool plat_prop_get(const plat_device_t *dev, const char *name, u32 *out)
{
    u32 i;

    if (dev == NULL || name == NULL || dev->props == NULL) {
        return false;
    }

    for (i = 0; i < dev->prop_count; i++) {
        if (plat_str_eq(dev->props[i].name, name) != 0) {
            if (out != NULL) {
                *out = dev->props[i].value;
            }
            return true;
        }
    }

    return false;
}

u32 plat_prop_get_or(const plat_device_t *dev, const char *name, u32 default_value)
{
    u32 value = default_value;

    (void)plat_prop_get(dev, name, &value);

    return value;
}

/* ------------------------------------------------------------------ */
/* probe 遍历                                                           */
/* ------------------------------------------------------------------ */

void plat_probe_all(const plat_device_t *devices, u32 device_count,
                    const plat_driver_t *drivers, u32 driver_count,
                    void *ctx, plat_probe_stats_t *stats)
{
    plat_probe_stats_t local = {0, 0, 0, 0, 0};
    u32                d;

    if (devices == NULL || drivers == NULL) {
        if (stats != NULL) {
            *stats = local;
        }
        return;
    }

    for (d = 0; d < device_count; d++) {
        const plat_device_t *dev = &devices[d];
        u32                  r;
        int                  claimed = 0;
        int                  any_matched = 0;

        local.total++;

        if (!plat_device_is_active(dev)) {
            /*
             * enabled == false 的节点计入 disabled,不参与匹配。
             * 仍要计数:调用方要靠 total / disabled / probed / unclaimed
             * 这四个数交叉验证"是不是每个节点都有了归宿"。
             */
            local.disabled++;
            continue;
        }

        /*
         * 内层按驱动表顺序找第一个能认领的驱动。
         *
         * probe 返回非 0 **不是终止条件** —— 继续试下一个匹配的驱动。
         * 这与 Linux 的语义一致:一个驱动可以因为"硬件版本不对"而主动放弃,
         * 把机会留给排在后面的通用驱动。
         */
        for (r = 0; r < driver_count; r++) {
            const plat_driver_t *drv = &drivers[r];

            if (!plat_compatible_match(drv->compatible, dev->compatible)) {
                continue;
            }

            any_matched = 1;

            if (drv->probe == NULL) {
                continue;
            }

            if (drv->probe(dev, ctx) == PLAT_PROBE_OK) {
                claimed = 1;
                break;
            }
        }

        if (claimed) {
            local.probed++;
        } else if (any_matched) {
            /* 有驱动声明认识它,但全都放弃了 —— 与"没人认领"要分开统计 */
            local.failed++;
        } else {
            local.unclaimed++;
        }
    }

    if (stats != NULL) {
        *stats = local;
    }
}
