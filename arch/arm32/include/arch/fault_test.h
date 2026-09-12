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
 */

#include <arch/types.h>

#define FAULT_SEL_NONE          0u
#define FAULT_SEL_DATA_ABORT    1u
#define FAULT_SEL_UNDEF         2u
#define FAULT_SEL_PREFETCH      3u
#define FAULT_SEL_SVC           4u

/*
 * 前三个是致命异常:处理函数打完现场后停在 vectors.S 的 wfe 自旋里,
 * 永远不会返回。SVC 不同 —— 它的向量没有自旋,处理完就返回到下一条指令。
 * 这个区别决定了调用方能不能指望"触发后不会再往下走"。
 */
#define FAULT_SEL_IS_FATAL(sel) ((sel) >= FAULT_SEL_DATA_ABORT && (sel) <= FAULT_SEL_PREFETCH)

/*
 * 按选择器触发对应异常。正常情况下不会返回 ——
 * 异常处理函数打完现场后会停机等待调试器。
 */
void fault_test_trigger(u32 selector);

/* 在主循环中检查选择器,非 0 则触发一次(触发前清零,避免反复进入) */
void fault_test_poll(void);
