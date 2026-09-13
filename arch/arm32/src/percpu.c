/*
 * 每 CPU 数据的**纯逻辑**部分 —— 不碰任何 CP15,所以宿主编译器能直接编译,
 * 可以上单元测试。CP15 相关在 percpu_hw.c。
 *
 * 这样做不是洁癖:本模块最容易出错的地方是"越界核号把别人的结构体写坏",
 * 而那种错误在板上的表现是"另一个核莫名奇妙状态不对",极难定位。
 * 放在宿主机上测,一秒就能钉住。
 */

#include <arch/percpu.h>

percpu_t g_percpu[PERCPU_MAX_CPUS];

/*
 * 静态锁死容量与核数的关系。
 *
 * 本板是双核,但数组大小写成了 PERCPU_MAX_CPUS。如果哪天核数被改大
 * 而这里忘了同步,percpu_for() 会静默返回 NULL,表现为"某个核起不来"
 * 而没有任何编译期提示。让它在编译期就报错。
 */
_Static_assert(PERCPU_MAX_CPUS >= 1u, "至少要有一个 CPU 的表项");
_Static_assert(PERCPU_MAX_CPUS <= 4u, "Cortex-A9 MPCore 最多 4 核,MPIDR 只取低 2 位");

bool percpu_id_valid(u32 cpu_id)
{
    return cpu_id < PERCPU_MAX_CPUS;
}

percpu_t *percpu_for(u32 cpu_id)
{
    if (!percpu_id_valid(cpu_id)) {
        return NULL;
    }
    return &g_percpu[cpu_id];
}

void percpu_table_reset(void)
{
    u32 i;

    for (i = 0; i < PERCPU_MAX_CPUS; i++) {
        percpu_t *pc = &g_percpu[i];

        pc->cpu_id    = i;
        pc->online    = 0;
        pc->mpidr     = 0;
        pc->loops     = 0;
        pc->irq_count = 0;
        pc->last_intid = 0xFFFFFFFFu;
        pc->spin_retry = 0;
        pc->stack_top  = 0;
    }
}

u32 percpu_online_count(void)
{
    u32 i;
    u32 count = 0;

    for (i = 0; i < PERCPU_MAX_CPUS; i++) {
        if (g_percpu[i].online != 0u) {
            count++;
        }
    }

    return count;
}
