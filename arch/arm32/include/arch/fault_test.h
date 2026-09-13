#pragma once

/*
 * 故障注入:用于验证异常诊断路径真的能工作。
 *
 * 为什么需要它:异常处理函数(c_data_abort_handler 等)是"出了事才跑"的
 * 代码,正常路径永远走不到。没有专门触发过,就无法区分
 *   "处理函数写对了" 和 "处理函数根本没被调用、只是系统恰好没崩"。
 *
 * 用法:内核启动后,用 JTAG 往选择器地址写码即可触发,不需要重新烧录:
 *   xsdb> mwr -force 0x00020080 1     # 触发 Data Abort
 *   xsdb> mwr -force 0x00020080 2     # 触发 Undefined Instruction
 *   xsdb> mwr -force 0x00020080 3     # 触发 Prefetch Abort
 *   xsdb> mwr -force 0x00020080 4     # 触发 SVC(会返回,不致命)
 *   xsdb> mwr -force 0x00020080 5     # 从 XN 区域取指(验证 XN 生效)
 *   xsdb> mwr -force 0x00020080 6     # 栈溢出进 guard 页(验证 guard 生效)
 *   xsdb> mwr -force 0x00020080 7     # 同上,但 guard 用 AP=0b000 实现
 *   xsdb> mwr -force 0x00020080 8     # ★ 对照组:关掉 guard 再溢出,应当不报错 ★
 *   xsdb> mwr -force 0x00020080 9     # 异常帧布局自检(会返回,结果进自检报告)
 */

#include <arch/types.h>

#define FAULT_SEL_NONE          0u
#define FAULT_SEL_DATA_ABORT    1u
#define FAULT_SEL_UNDEF         2u
#define FAULT_SEL_PREFETCH      3u
#define FAULT_SEL_SVC           4u

/*
 * 从"已映射但被标记为不可执行(XN)"的区域取指。
 *
 * 与 FAULT_SEL_PREFETCH 的区别很重要。两者都产生 Prefetch Abort,
 * 但 **IFSR 完全不同**:
 *   3 -> IFSR = 0x08  synchronous external abort, not on translation table walk
 *                     (地址背后没有从设备,AXI 事务失败)
 *   5 -> IFSR = 0x0D  permission fault, level 1(被 XN 拦下来的)
 *
 * 所以只看到 "Prefetch Abort" 不足以说明 XN 生效了 ——
 * 必须核对 IFSR 的具体值。这个用例就是为此存在的:
 * 如果 XN 没写进描述符,选 5 会得到 0x08(那次访问同样没有从设备),
 * 看上去"也报了 Prefetch Abort",实际上什么都没验证到。
 *
 * ⚠ 状态码的名字按 ARMv7-A ARM 的**短描述符格式**表来写:
 *   level 1 = L1 描述符本身有问题(短描述符下 L1 覆盖 1MB,即"段");
 *   level 2 = L1 是页表项没问题,问题在 **L2 小页项**。
 *   以前这里写的是 "permission fault, section",语义对但用词不是权威表里的,
 *   且容易和长描述符(LPAE)的编码混起来。
 */
#define FAULT_SEL_XN_FETCH      5u

/*
 * ====================================================================
 * 栈溢出进 guard 页(M4-5)—— 一组三件,构成一次受控 A/B
 * ====================================================================
 *
 * guard page 的验收不能用"读回页表看那一项是不是 0"来做:
 * 那个条件在 **TLB 里还留着拆段之前的段表项** 时同样成立,
 * 而那恰恰是最可能出的错(改完页表忘了失效 TLB)。
 * 要证明 guard 是承重的,只有一条路:**同一个地址、同一条指令,
 * 只在 guard 有无这一件事上不同,看行为是否随之改变。**
 *
 *   6 -> guard 用"不映射"实现,溢出必须命中
 *          **translation fault, level 2**:FS[4:0] = 0x07,
 *          且 DFAR = guard 页的最后一个字(= base-4),WnR=1(写)
 *   7 -> guard 用"映射但 AP=0b000"实现,溢出必须命中
 *          **permission fault, level 2**:FS[4:0] = 0x0F
 *
 *        ⚠ 状态位的取法见 gic.c 的说明:DFSR 的 bits[7:4] 是 **Domain**,
 *          它会污染 `dfsr & 0x1F`。本内核 domain=15,所以真值 0x07
 *          经 `& 0x1F` 会变成 0x17。判据必须用 FS[4:0] 的正确位域。
 *
 *        ⚠ 7 同时补上了 M2-4 欠的一笔账。当时把 DACR 从全 manager 切成
 *          全 client,理由是"所有区域的 AP 都是 0b011,所以行为应当完全
 *          不变" —— 也就是说 **AP 到底有没有被硬件执行,从未被验证过**。
 *          一个 AP=0b000 的页会把这件事变成可观测的。
 *
 *   8 -> ★ 对照组:临时把 guard 页映射成普通可读写页,再跑同一段溢出 ★
 *          这次**不该**有任何异常,写下去的内容还必须读得回来。
 *          少了这一件,6 和 7 只证明了"这里确实报错了",
 *          证明不了"报错是因为 guard"。
 *
 * 溢出代码本身在 src/kstack.c(kstack_probe_overflow),方向与真实栈一致
 * (地址递减),第一个字就落在 guard 页的最后一个字上。
 *
 * ⚠ 8 不会停机(这正是它的意义),所以它可以在主循环里反复触发;
 *   6 和 7 会停在 Data Abort 现场,触发一次就结束。
 */
#define FAULT_SEL_STACK_GUARD      6u
#define FAULT_SEL_STACK_GUARD_AP   7u
#define FAULT_SEL_STACK_GUARD_OFF  8u

/*
 * 异常帧布局的运行时自检(M4-6)。
 *
 * 把 r0-r12 设成已知图案,再发一个立即数为 `ARM_SVC_FRAME_CHECK` 的 SVC;
 * `c_svc_handler` 逐个核对帧里的 13 个槽,外加 SPSR 的模式位与 ret 的值。
 *
 * 会**返回**(SVC 的向量没有 wfe 自旋),结论进自检报告。
 * 与选择器 4 的区别:4 是"诊断路径能不能跑",9 是"帧布局对不对"。
 */
#define FAULT_SEL_SVC_FRAME        9u

/*
 * 哪些选择器会真的停机。
 *
 * 除 SVC 与 8 之外全部是致命的:处理函数打完现场后停在 vectors.S 的 wfe
 * 自旋里,永远不会返回。SVC 的向量没有自旋,处理完就返回到 SVC 的下一条指令;
 * 8 是**对照组**,它要达成的结果恰恰是"什么都不发生"。
 * 这个区别决定了调用方能不能指望"触发后不会再往下走"。
 *
 * 用逐个列举而不是区间判断:新增用例时如果忘了归类,
 * 区间写法会静默地把它算错(比如把 5 漏掉),列举法不会。
 */
#define FAULT_SEL_IS_FATAL(sel)                        \
    ((sel) == FAULT_SEL_DATA_ABORT ||                  \
     (sel) == FAULT_SEL_UNDEF ||                       \
     (sel) == FAULT_SEL_PREFETCH ||                    \
     (sel) == FAULT_SEL_XN_FETCH ||                    \
     (sel) == FAULT_SEL_STACK_GUARD ||                 \
     (sel) == FAULT_SEL_STACK_GUARD_AP)
/* 明确记下"不致命"的两个:SVC(4)与帧布局自检(9)都会返回 */
_Static_assert(FAULT_SEL_SVC != FAULT_SEL_SVC_FRAME, "两个非致命用例的选择器不能撞号");


/*
 * 按选择器触发对应异常。正常情况下不会返回 ——
 * 异常处理函数打完现场后会停机等待调试器。
 */
void fault_test_trigger(u32 selector);

/* 在主循环中检查选择器,非 0 则触发一次(触发前清零,避免反复进入) */
void fault_test_poll(void);
