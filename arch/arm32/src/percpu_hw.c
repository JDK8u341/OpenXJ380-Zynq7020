/*
 * 每 CPU 数据的 CP15 部分 —— 只能在目标板上跑。
 *
 * 纯逻辑见 percpu.c(宿主可测)。
 */

#include <arch/cpu.h>
#include <arch/percpu.h>

percpu_t *percpu_init_self(u32 stack_top)
{
    u32       mpidr = arch_read_mpidr();
    u32       id    = mpidr & 0x3u;
    percpu_t *pc    = percpu_for(id);

    if (pc == NULL) {
        return NULL;
    }

    /*
     * ⚠⚠ 这里**只写 CP15,不写任何共享内存**。⚠⚠
     *
     * 原因是一个真实踩到的 SMP 一致性 bug:本函数在 CPU1 上执行时,
     * CPU1 的缓存还是关的 —— 它的内存写入直通 DDR,而且**不会让
     * CPU0 缓存里的副本失效**。于是:
     *
     *   1. CPU0 的 percpu_table_reset() 把整行读进自己的 L1 并置脏;
     *   2. CPU1 开缓存之前往这一行写 mpidr,值进了 DDR;
     *   3. CPU0 那条脏行后来被换出,**把 mpidr 又覆盖回复位值 0**。
     *
     * 实测症状正是如此:CPU1 自己写心跳时 mpidr=0x80000001 是对的,
     * 而结构体里最终留下的是 0 —— 同一行里的 cpu_id/stack_top(CPU0 写的)
     * 和 online/loops(CPU1 开缓存之后写的)全都正常,唯独这一个字段丢了。
     *
     * 规矩:**CPU1 在缓存使能之前不得写任何共享内存**。
     * 本函数只碰 TPIDRPRW(每核寄存器,不涉及缓存),是安全的;
     * 其余字段由 CPU1 在 cache_enable_l1() 之后补写(见 smp.c)。
     *
     * stack_top 是例外:它只有 CPU0 知道(链接符号 __stack1_top),
     * 而 CPU1 是被 start.S 尾调用进来的、没有参数通道。所以 CPU0 先填好,
     * 这里只在传入非 0 时才覆盖(CPU0 自己那条路径)。
     */
    if (stack_top != 0u) {
        pc->stack_top = stack_top;
    }

    arch_write_percpu((u32)(uintptr_t)pc);
    arch_dsb();

    return pc;
}

percpu_t *percpu_self(void)
{
    u32 raw = arch_read_percpu();

    /* 还没初始化过。返回 NULL 而不是 (percpu_t *)0 —— 让调用方炸得明显些 */
    if (raw == 0u) {
        return NULL;
    }

    return (percpu_t *)(uintptr_t)raw;
}

void percpu_publish_self(void)
{
    percpu_t *pc = percpu_self();

    if (pc == NULL) {
        return;
    }

    /*
     * ⚠ 只能在**本核缓存已使能**之后调用。
     *   缓存没开时的写入不参与一致性,会被别的核的旧脏行覆盖掉 ——
     *   这个坑在 percpu_init_self() 的注释里有完整记录。
     */
    pc->cpu_id = arch_cpu_id();
    pc->mpidr  = arch_read_mpidr();
    pc->online = 1u;

    arch_dsb();
}
