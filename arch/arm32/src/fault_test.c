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
#include <arch/kstack.h>
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

    /*
     * ---- 栈溢出进 guard 页(M4-5)----
     *
     * 这三件是一组受控 A/B,单独看任何一件都不构成证据:
     * 6/7 证明"这样访问会报错",8 证明"报错的原因确实是 guard"。
     * 详见 arch/fault_test.h 的说明。
     */
    case FAULT_SEL_STACK_GUARD:
    case FAULT_SEL_STACK_GUARD_AP:
    case FAULT_SEL_STACK_GUARD_OFF: {
        const kstack_t *probe = kstack_probe_stack();

        if (probe == NULL) {
            console_puts("    no stack registered for probing (kstack_probe_register)\n");
            break;
        }

        if (selector == FAULT_SEL_STACK_GUARD_OFF) {
            /*
             * ★ 对照组:把 guard 页变成普通可读写页 ★
             *
             * 之后跑的是**完全相同**的一段代码、完全相同的地址。
             * 唯一的差别就是这一页映射与否 —— 这正是"受控"的含义。
             */
            kstack_err_t e = kstack_probe_guard_disable();

            console_puts("    guard page temporarily MAPPED (control group)\n");
            console_printf("      stack base=0x%08X top=0x%08X guard=0x%08X\n", probe->base, probe->top,
                           probe->guard);
            console_printf("      guard_disable=%u\n", (u32)e);

            timer_delay_ms(50);

            {
                u32 written = kstack_probe_overflow();

                /*
                 * 走到这里就是结果本身:guard 关掉之后,同一段溢出
                 * **不再有异常**,写下去的内容还静默地留在了那里。
                 */
                console_printf("    overflow wrote %u words past the stack bottom\n", written);
                console_printf("      clobbered region readable back = %s\n",
                               kstack_probe_clobbered() ? "YES (silent corruption)" : "NO");
                console_puts("    !!! CONTROL GROUP: no exception, as expected !!!\n");
            }

            /* 恢复,别把池留在被改过的状态里 */
            if (kstack_probe_guard_enable() != KSTACK_OK) {
                console_puts("    WARN: failed to restore the guard page\n");
            } else {
                console_puts("    guard page restored\n");
            }
            return;
        }

        if (selector == FAULT_SEL_STACK_GUARD_AP) {
            /*
             * AP=0b000 的 guard 需要**另一个池实例**(guard_kind 是池级配置),
             * 所以板级建了两个池:大的那个用"不映射",这个小的用 AP=0b000。
             *
             * ★ 这一路的 A/B 比"不映射"那一路更紧 ★
             *   两个阶段里 guard 页**都是映射着的**,唯一的差别就是 AP 是不是
             *   0b000(对照组的 kstack_guard_disable 会把 AP 改回全权限)。
             *   所以它同时回答两件事:
             *     1. KSTACK_GUARD_AP_NONE 这条实现在真硬件上成不成立;
             *     2. ★ DACR = client 模式下 AP 到底有没有被硬件执行 ★ ——
             *        这是 M2-4 欠的账:当时把 DACR 从全 manager 切成全 client,
             *        理由是"所有区域的 AP 都是 0b011,所以行为应当完全不变",
             *        也就是说 AP 有没有被强制执行**从来没有被验证过**。
             */
            const kstack_t *ap = kstack_probe_stack_ap();

            if (ap == NULL) {
                console_puts("    no AP-mode stack registered (kstack_probe_register_ap)\n");
                return;
            }

            console_printf("    stack base=0x%08X top=0x%08X guard=0x%08X\n", ap->base, ap->top,
                           ap->guard);
            console_puts("    guard page is MAPPED but AP=0b000 (no access at PL1)\n");
            console_puts("      -> expect Data Abort with DFAR = base-4\n");
            console_puts("         and FS[4:0]=0x0F (permission fault, level 2)\n");
            console_puts("         (NOT 0x07: this is a permission fault, not a translation one)\n");
            timer_delay_ms(50);

            (void)kstack_probe_overflow_ap();
            return;
        }

        console_printf("    stack base=0x%08X top=0x%08X guard=0x%08X\n", probe->base, probe->top,
                       probe->guard);
        console_puts("    writing downwards past the stack bottom\n");
        console_puts("      -> expect Data Abort with DFAR = base-4\n");
        console_puts("         and FS[4:0]=0x07 (translation fault, level 2)\n");
        console_puts("         (take FS as (dfsr&0xF)|((dfsr>>10)&0x10) - NOT dfsr&0x1F:\n");
        console_puts("          bits[7:4] of DFSR are the Domain field, domain=15 here)\n");
        timer_delay_ms(50);

        /*
         * 正常情况下**不会返回**:第一个字就落在 guard 页里,
         * 而那一页没有映射,于是 Translation fault。
         */
        (void)kstack_probe_overflow();
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
