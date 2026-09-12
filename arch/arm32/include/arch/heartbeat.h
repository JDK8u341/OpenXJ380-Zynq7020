#pragma once

/*
 * OCM 心跳区槽位定义 —— 跨模块契约
 *
 * 心跳的作用是在**串口不可用、甚至内核已经跑飞**的时候仍然能观测状态:
 * JTAG 用 mrd 直接读 OCM 固定地址,不依赖任何驱动,也不需要 CPU 配合。
 * 详见 arch/arm32/README.md 的「调试通道」章节。
 *
 * 为什么单独放一个头文件、而不是各自在 .c 里 define:
 *
 *   MMU 打开过程一旦出错,kmain 就再也执行不到了 ——
 *   所以"进行到哪一步"这个标记必须由**开 MMU 的代码自己**写。
 *   于是写这个区的模块不止一个(kmain 与 mmu_hw)。
 *   两处槽位定义一旦不一致,读到的就是别的字段,
 *   而心跳恰恰是出问题时唯一还能用的观测手段,它自己不能先坏掉。
 *
 *   slot 0..15 是常规状态,由 kmain 维护;
 *   slot 16 起是**分阶段进度标记**,由负责该阶段的模块写 ——
 *   这类模块的共同点是"中途可能再也回不来"。
 */

#include <arch/io.h>
#include <arch/platform.h>
#include <arch/types.h>

#define HB ((volatile u32 *)PLAT_HEARTBEAT_BASE)

/* ---- 常规状态(kmain 维护) ---- */
#define HB_SLOT_MAGIC     0u  /* PLAT_HEARTBEAT_MAGIC,内核活了的第一证据 */
#define HB_SLOT_LOOP      1u  /* 主循环计数 */
#define HB_SLOT_LED       2u  /* PL LED 图案 */
#define HB_SLOT_SW        3u  /* 拨码开关值 */
#define HB_SLOT_GT        4u  /* 全局定时器低 32 位 */
#define HB_SLOT_PSLED     5u  /* PS LED 状态 */
#define HB_SLOT_UARTCLK   6u  /* UART 参考时钟(0 = 未标定) */
#define HB_SLOT_DIRM0     7u  /* PS GPIO DIRM_0 */
#define HB_SLOT_UARTOK    8u  /* UART 探测结果(1 = 存在) */
#define HB_SLOT_MEASBAUD  9u  /* 当前设定下实测的波特率 */
#define HB_SLOT_TICKS     10u /* 周期 tick 计数 */
#define HB_SLOT_IRQCOUNT  11u /* GIC 收到的中断总数 */
#define HB_SLOT_CLKSRC    12u /* 参考时钟来源:1 = 闭环收敛,2 = 兜底常量 */
#define HB_SLOT_CONVITER  13u /* 闭环迭代次数 */
#define HB_SLOT_BAUDGEN   14u /* 最终生效的 BAUDGEN */
#define HB_SLOT_BAUDDIV   15u /* 最终生效的 BAUDDIV */

/*
 * ---- 分阶段进度标记 ----
 *
 * 约定:进入一个可能回不来的阶段前写入阶段号,成功后写入下一个号。
 * 挂死时读回的值就是"最后成功进入的阶段"。
 *
 * 单核阶段只有一个这样的模块(MMU),所以先只留一段;
 * 后续 SMP 启动第二个核时会需要第二段。
 */
#define HB_SLOT_MMUSTAGE  16u

/*
 * 相邻两次 tick 中断之间的最大间隔(微秒)。
 *
 * 私有定时器是电平触发的:CPU 若有一段时间不响应,
 * 多次 expire 会被合并成一次中断,计数上表现为"丢了 N 个 tick",
 * 而寄存器和 GIC 状态全都正常 —— 光看现场分不出这是丢中断还是时钟不准。
 * 这个槽把这个时长直接量出来,正常应紧贴 1000us。
 */
#define HB_SLOT_TICKGAP   17u

/*
 * 缓存加速比(使能缓存前后同一个内存密集循环的耗时之比)。
 *
 * 存在的意义同 maxgap:光看 SCTLR 的 C 位读回 1 不能说明缓存有效 ——
 * 内存属性写错时系统照样跑,只是白忙一场。这个比值能揭穿那种情况。
 * 正常情况下应当在数倍以上。
 */
#define HB_SLOT_CACHEBENCH 18u

/*
 * 缓存维护自检结果:0 = 通过,非 0 = 失败项编号(见 src/cache_hw.c)。
 *
 * 验证的是 DMA 依赖的语义 —— clean 有没有真的把数据写回内存、
 * invalidate 有没有真的丢弃缓存副本。两者任一不成立,
 * DMA 会表现为"偶尔错几个字节",那是最难查的一类问题。
 */
#define HB_SLOT_CACHESELFTEST 19u

/*
 * L2 有效性:同一个 128KB 工作集在"关 L2"与"开 L2"下的耗时之比。
 *
 * 存在的理由与 HB_SLOT_CACHEBENCH 相同,但针对的是 L2 ——
 * 4KB 的工作集装得进 L1,根本碰不到 L2,所以那个加速比证明不了 L2 的事。
 * 正常应当在数倍以上。
 */
#define HB_SLOT_L2BENCH   20u

/* MMU 阶段的取值。0 表示还没开始 */
#define HB_MMU_STAGE_IDLE       0u /* 尚未开始 */
#define HB_MMU_STAGE_BUILT      1u /* 页表已填好 */
#define HB_MMU_STAGE_TABLE_SET  2u /* TTBR0/DACR/ACTLR 已配置,MMU 未开 */
#define HB_MMU_STAGE_ENABLING   3u /* 正要写 SCTLR 打开 MMU */
#define HB_MMU_STAGE_ON         4u /* MMU 已开,且已回到 C 代码继续跑 */
#define HB_MMU_STAGE_FAILED     0xEEu /* 自检未通过,主动放弃开 MMU */

/* 已定义的槽位数。越界写会踩到后面的故障注入选择器 */
#define HB_SLOT_COUNT     21u

/* 心跳区预留的槽位容量(见 platform.h 的 PLAT_HEARTBEAT_REGION_SLOTS) */
#define HB_SLOT_CAPACITY  PLAT_HEARTBEAT_REGION_SLOTS

/* 故障注入选择器所在的槽号(紧跟在预留容量之后) */
#define HB_FAULT_SEL_SLOT ((PLAT_FAULT_SEL_ADDR - PLAT_HEARTBEAT_BASE) / 4u)

/*
 * 双向锁住心跳区与故障注入选择器的边界。
 *
 * 这两个结构都是会增长的:心跳已经扩过两次槽位,而选择器原先就在
 * 第 16 槽上。一旦心跳长到选择器头上,fault_test_poll() 会把
 * 心跳进度号当成注入码读走 —— 症状是"内核莫名进了 Data Abort",
 * 而心跳本身看上去一切正常,极难联想到是这两个结构撞了。
 * 让它在编译期就报错。
 */
_Static_assert(HB_SLOT_COUNT <= HB_SLOT_CAPACITY,
               "heartbeat slots exceed the reserved region capacity");
_Static_assert(HB_FAULT_SEL_SLOT >= HB_SLOT_CAPACITY,
               "the fault-injection selector overlaps the heartbeat region");
