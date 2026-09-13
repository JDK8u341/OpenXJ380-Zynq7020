/*
 * SMP:引导第二个核(AM3-2 / AM3-3)
 *
 * 步骤与依据见 include/arch/smp.h 的说明。
 */

#include <arch/cache.h>
#include <arch/cpu.h>
#include <arch/heartbeat.h>
#include <arch/mmu.h>
#include <arch/percpu.h>
#include <arch/platform.h>
#include <arch/smp.h>
#include <arch/timer.h>

/*
 * CPU1 上线的等待上限。
 *
 * 取 200ms:CPU1 要做的事(MMU、缓存、SCU)在 666MHz 上是微秒级的,
 * 200ms 已经宽松三个数量级。超过就说明真的没起来,不该再等。
 */
#define SMP_ONLINE_TIMEOUT_US 200000u

/*
 * 链接脚本里 CPU1 栈区的顶部(boot/kernel.ld)。
 *
 * 只有 CPU0 知道这个地址 —— CPU1 是被 start.S 尾调用进来的,没有参数通道。
 * 所以由 CPU0 在放它起来之前先把这块信息填进 percpu 表。
 */
extern char __stack1_top[];

void smp_release_cpu1(void)
{
    /*
     * 先确保 CPU1 的表项是干净的。
     * CPU0 在放它起来之前就把这步做完,CPU1 才能一上来就找到自己的结构体。
     */
    g_percpu[1].cpu_id     = 1u;
    g_percpu[1].online     = 0u;
    g_percpu[1].last_intid = 0xFFFFFFFFu;
    g_percpu[1].stack_top  = (uintptr_t)__stack1_top;

    /*
     * ⚠⚠ 必须把这张表从 CPU0 的缓存里**清洗并失效**出去。⚠⚠
     *
     * 上面那几行写入现在只躺在 CPU0 的 L1 里(脏行)。而 CPU1 起来时
     * **缓存是关的**,它的写入直通 DDR、不会使 CPU0 的脏副本失效。
     * 于是 CPU0 那条脏行后来被换出时,会把 CPU1 早先写进去的值覆盖掉。
     *
     * 这个 bug 真实发生过:CPU1 写的 mpidr 被 CPU0 的旧值 0 盖掉,
     * 而同一行里 CPU0 写的字段和 CPU1 开缓存之后写的字段全都正常 ——
     * 症状只丢一个字段,极难联想到是缓存一致性问题。
     *
     * 清洗 + 失效之后:CPU0 下次读这张表会从 DDR 重新取,
     * CPU1 的写入再也覆盖不掉;等 CPU1 开了缓存,两边就走 SCU 一致性了。
     */
    cache_clean_invalidate_range((uintptr_t)&g_percpu[0], sizeof(g_percpu));

    /*
     * DSB 不可省:它保证上面这些写入(以及内核镜像本身)在
     * 下面那条 SEV 之前对内存系统可见。少了它,CPU1 可能在
     * 字段还没落地时就开始读 percpu。
     */
    arch_dsb();

    *SMP_CPU1_START_ADDR = (u32)(uintptr_t)&cpu1_entry;

    /*
     * 写入口地址之后必须再来一次 DSB 才能 SEV。
     *
     * 顺序反了(先 SEV 再写地址)的后果很隐蔽:CPU1 醒来读到的是
     * BootROM 预置的"安全网"地址,于是又回到 WFE —— 表现为
     * "CPU1 永远不上线",而 0xFFFFFFF0 里看上去值是对的(我们后来写的)。
     */
    arch_dsb();

    cpu_sev();
}

bool smp_wait_online(u32 cpu_id, u32 timeout_us)
{
    percpu_t *pc = percpu_for(cpu_id);
    u64       start;

    if (pc == NULL) {
        return false;
    }

    start = timer_read_us();
    while (pc->online == 0u) {
        if ((u32)(timer_read_us() - start) > timeout_us) {
            return false;
        }
        cpu_relax();
    }

    /*
     * 读到 online 之后再来一次 DMB:保证 CPU1 在置位之前写的
     * 其它字段(irq_count、mpidr 等)也一并可见 ——
     * 否则会出现"online 是 1 但旁边的字段还是 0"的诡异现象。
     */
    arch_dmb();

    return true;
}

/*
 * CPU1 的 C 侧主函数。
 *
 * 到达这里时的 CPU 状态(由 start.S 的 cpu1_entry 建立):
 *   - ARM 态、SVC 模式、IRQ/FIQ 屏蔽
 *   - 各模式栈已就位
 *   - VBAR 已指向共享向量表
 *   - **MMU 关、缓存关**(这些都是每核寄存器,CPU1 的初值是复位值)
 *   - BSS 已由 CPU0 清零,不需要也不能再清一遍
 */
void cpu1_main(void)
{
    percpu_t *pc;

    /*
     * 阶段号由 CPU1 自己写。挂死时 JTAG 读回的就是它最后进入的阶段 ——
     * 串口在 CPU1 起来之前是帮不上忙的(控制台归 CPU0 管)。
     */
    HB[HB_SLOT_CPU1_STAGE] = HB_CPU1_STAGE_ENTERED;

    pc = percpu_init_self(0);
    if (pc == NULL) {
        /* 核号越界 —— 本板不该发生。停下来等调试器,而不是跑飞 */
        HB[HB_SLOT_CPU1_STAGE] = HB_CPU1_STAGE_FAILED;
        for (;;) {
            cpu_wfe();
        }
    }

    HB[HB_SLOT_CPU1_STAGE] = HB_CPU1_STAGE_PERCPU;

    /*
     * ---- 1. MMU ----
     * 复用 CPU0 已经建好的页表,只写本核的 TTBR0/DACR/SCTLR。
     * 不重建表:那会与 CPU0 的写入竞争,而且完全没有必要。
     */
    mmu_enable_secondary();
    HB[HB_SLOT_CPU1_STAGE] = HB_CPU1_STAGE_MMU;

    /*
     * ---- 2. 一致性 ----
     * SCU 是共享的(CPU0 已经开过,这里置位自己的位即可),
     * 但 ACTLR 的 SMP 位是**每核**的 —— CPU1 不置的话,
     * 它的 L1 不会缓存 Shareable 内存,表现为"缓存开了但没加速"。
     */
    cortexa9_coherency_init();
    HB[HB_SLOT_CPU1_STAGE] = HB_CPU1_STAGE_COHERENT;

    /*
     * ---- 3. 本核 L1 ----
     * 只失效/使能本核的 L1。L1 是每核私有的,按 set/way 全清是安全的;
     * **L2 是共享的,绝不能在这里重来一遍**(那会丢掉 CPU0 的脏数据)。
     */
    cache_invalidate_before_enable();
    cache_enable_l1();
    HB[HB_SLOT_CPU1_STAGE] = HB_CPU1_STAGE_CACHE;

    /*
     * ---- 4. 补写本核身份,然后报到 ----
     *
     * ⚠ 这些共享内存的写入**必须在缓存使能之后**。
     *   在这之前本核的写入走的是非缓存路径,不会让 CPU0 的缓存副本失效,
     *   于是 CPU0 的旧脏行换出时会把它们覆盖掉 —— 这个坑真实踩过,
     *   详见 percpu_hw.c 的 percpu_init_self() 注释。
     *   现在两边都开了缓存、都在 SCU 一致性域里,写入是互见的。
     *
     * ⚠ online 也必须由本核自己置。由 CPU0 代写是错的:
     *   那只能证明"CPU0 觉得 CPU1 该起来了"。
     */
    percpu_publish_self();

    HB[HB_SLOT_CPU1_ID]     = pc->cpu_id;
    HB[HB_SLOT_CPU1_MPIDR]  = pc->mpidr;
    HB[HB_SLOT_CPU1_ONLINE] = 1u;
    HB[HB_SLOT_CPU1_STAGE]  = HB_CPU1_STAGE_ONLINE;
    arch_dsb();
    cpu_sev(); /* 通知可能正在 WFE 等它的 CPU0 */
    /*
     * ---- 5. 本核主循环 ----
     * 这一轮(AM3-1/2/3)还没有本核中断,所以只是计数 + 让出。
     * per-CPU 中断与 SGI 在 AM3-4/5 接上。
     *
     * 心跳里的 loops 只在本核内递增,CPU0 通过读 percpu 表拿到它 ——
     * 这是"CPU1 真的在独立推进"而不是"CPU0 打印了一个数字"的证据。
     */
    for (;;) {
        pc->loops++;
        cpu_relax();
    }
}
