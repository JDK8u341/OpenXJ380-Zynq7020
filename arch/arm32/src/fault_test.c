/*
 * 故障注入实现
 *
 * 四个异常各有一条触发路径,每次触发前先把说明打到串口 ——
 * 这样即便处理函数没跑起来,也能从"最后一行输出"判断走到哪一步。
 *
 * 其中前三个是致命异常(处理完停在 wfe 自旋),SVC 会返回到下一条指令。
 */

#include <arch/console.h>
#include <arch/fault_test.h>
#include <arch/io.h>
#include <arch/platform.h>
#include <arch/timer.h>

#define FAULT_SEL ((volatile u32 *)PLAT_FAULT_SEL_ADDR)

/*
 * 刻意选一个 Zynq 地址映射里没有从设备的位置。
 *
 * 参考 Zynq 地址映射:
 *   0x00000000-0x3FFFFFFF  DDR
 *   0x40000000-0x7FFFFFFF  PL (AXI GP)
 *   0xE0000000-0xE02FFFFF  PS 外设
 *   0xF8000000-0xF8FFFFFF  SLCR/SCU/GIC
 *   0xFFF00000-0xFFFFFFFF  OCM 高位
 * 当前比特流只在 0x41200000 挂了 AXI GPIO,所以 0x50000000 无从设备,
 * AXI 事务会失败 -> 外部异常 -> Data/Prefetch Abort。
 */
#define UNMAPPED_ADDR 0x50000000u

void fault_test_trigger(u32 selector)
{
    console_printf("\n>>> FAULT INJECTION: selector=%u\n", selector);

    /* 留出时间把上面这行推出去,否则异常一旦发生串口可能来不及发完 */
    timer_delay_ms(100);

    switch (selector) {
    case FAULT_SEL_DATA_ABORT: {
        volatile u32 value;

        console_puts("    reading unmapped 0x50000000 -> expect Data Abort\n");
        timer_delay_ms(50);

        /*
         * volatile 保证这条读真的发出去。若处理函数正确,这里之后
         * 不会再有输出 —— 串口上会出现 Data Abort 现场。
         */
        value = *(volatile u32 *)UNMAPPED_ADDR;
        (void)value;
        break;
    }

    case FAULT_SEL_UNDEF: {
        console_puts("    executing UDF -> expect Undefined Instruction\n");
        timer_delay_ms(50);

        /*
         * 0xE7F000F0 是 ARM 架构永久未定义的编码(UDF)。
         * 用 .word 而不是助记符,避免不同汇编器对 UDF 的支持差异。
         */
        __asm__ volatile(".word 0xE7F000F0" ::: "memory");
        break;
    }

    case FAULT_SEL_PREFETCH: {
        console_puts("    branching to unmapped 0x50000000 -> expect Prefetch Abort\n");
        timer_delay_ms(50);

        /*
         * 用寄存器间接跳转,避免编译器把目标当常量做优化。
         * 0x50000000 的 bit0 = 0,所以是 ARM 状态跳转。
         */
        __asm__ volatile("ldr r0, =0x50000000\n\t"
                         "blx r0\n\t"
                         :
                         :
                         : "r0", "lr", "memory");
        break;
    }

    case FAULT_SEL_SVC: {
        console_puts("    svc #0 -> expect SVC handler (diagnostic only, will return)\n");
        timer_delay_ms(50);

        /*
         * SVC 与前三个不同:它的向量(_vec_svc)没有 wfe 自旋,处理完就返回。
         * 所以这里提前 return,避免落到下面那句"异常没有触发"的提示上 ——
         * 对 SVC 来说"返回了"恰恰是正确结果。
         */
        __asm__ volatile("svc #0" ::: "memory");

        console_puts("    SVC returned normally -> handler ran and returned as designed\n");
        return;
    }

    case FAULT_SEL_XN_FETCH: {
        console_puts("    branching into the XN-marked PS peripheral region\n");
        console_puts("      -> expect Prefetch Abort with IFSR=0x0D (permission fault)\n");
        timer_delay_ms(50);

        /*
         * 挑 UART1 的地址:它在页表里是 Device 且带 XN。
         *
         * 这里刻意不复用 FAULT_SEL_PREFETCH 用的 0x50000000 ——
         * 那个地址没有映射(XN 与否都会报外部异常),区分不出 XN 是否生效。
         * 只有"已映射 + 明确标了 XN"的地址才能验证 XN 这条路径。
         */
        __asm__ volatile("ldr r0, =0xE0001000\n\t"
                         "blx r0\n\t"
                         :
                         :
                         : "r0", "lr", "memory");
        break;
    }

    default:
        console_puts("    unknown selector, nothing triggered\n");
        break;
    }

    /*
     * 走到这里说明异常没有发生(或处理函数返回了)。
     * 对 data abort / prefetch abort 来说,这本身就是失败信号。
     */
    console_puts("    !!! NOT REACHED in a successful test - exception did not fire\n");
}

void fault_test_poll(void)
{
    u32 selector = *FAULT_SEL;

    if (selector != FAULT_SEL_NONE) {
        /*
         * 先清零再触发。否则异常处理停机后,若被调试器恢复运行,
         * 会立刻再次进入同一个故障,掩盖现场。
         */
        *FAULT_SEL = FAULT_SEL_NONE;
        fault_test_trigger(selector);
    }
}
