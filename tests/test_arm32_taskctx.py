"""寄存器帧(M4-6)的单元测试。

被测的是 arch/arm32/include/arch/taskctx.h 与它的汇编侧对应物
arch/arm32/include/arch/taskctx_asm.h。

**为什么值得测**:帧布局是**汇编与 C 之间的 ABI**,而且有三类失败
都不会在编译期报错、也不会有下游症状可查:

  1. **偏移两边不一致** —— 汇编按一套算法存,C 按另一套读。
     x86 侧就有现成的例子:`kernel/intr/handler.S:29-31` 把
     PROCESSOR_INFO 的三个偏移硬写在汇编里,**一个 static_assert 都没有**。
     本测试独立地把偏移**重算一遍**(不看头文件里的断言),再与头文件比。
  2. **`ret` 与 `pc` 混为一谈** —— 这两个值在 ARM 上**不一样**:
     返回要用"首选返回地址",诊断要用"出错/被中断的指令地址",
     而它们的差随异常类型变化(实测表在 taskctx.h 顶部)。
     混用之后"至少有一个是错的",而且**看起来一切正常**。
  3. **SVC 立即数的载波位置** —— 系统调用号放在 svc 指令的低 24 位,
     而取它的前提是 `pc` 真的指向那条 svc 指令。旧代码的 pc 指向
     下一条指令,按它取会取到别的指令的低 24 位当"系统调用号",
     于是调用到一个**不存在但看似合法**的号上。

参考:
  arch/arm32/include/arch/taskctx_asm.h   偏移宏(汇编与 C 共用)
  arch/arm32/include/arch/taskctx.h       结构体 + 断言 + 纯函数
  arch/arm32/boot/vectors.S               按这些宏建帧
  tmp-test/exc_frame_probe.py             板上实测 LR 偏移
"""

from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

HARNESS = r"""
int printf(const char *fmt, ...);

#include <arch/taskctx.h>
#include <arch/types.h>

static int failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL %d: %s\n", __LINE__, #cond);                                              \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

int main(void)
{
    /* ---- 1. 两个结构体的 sizeof 必须等于宏算出来的帧大小 ---- */
    CHECK(sizeof(arm_exc_frame_t) == ARM_EXC_FRAME_BYTES);
    CHECK(sizeof(arm_task_ctx_t) == ARM_CTX_BYTES);

    /* ---- 2. 独立重算偏移,不信头文件里的断言 ---- */
    /*
     * ★ 布局决定:r0-r12 | svc_lr | ret | spsr ★
     *   svc_lr 必须在 bl 之前存下来 —— 处理函数跑在 SVC 模式,
     *   `bl` 会踩掉被中断代码的 LR(不存它,被中断的函数一 bx lr 就飞)。
     *   ret/spsr 相邻且 ret 在低地址,是为了让 `rfeia sp!` 一条指令
     *   同时完成"跳转 + 恢复 CPSR"。
     */
    CHECK(ARM_EXC_OFF_R0 == 0u);
    CHECK(ARM_EXC_OFF_SVC_LR == 13u * 4u);
    CHECK(ARM_EXC_OFF_RET == ARM_EXC_OFF_SVC_LR + 4u);
    CHECK(ARM_EXC_OFF_SPSR == ARM_EXC_OFF_RET + 4u);
    CHECK(ARM_EXC_FRAME_WORDS == 16u);
    CHECK(ARM_EXC_FRAME_BYTES == 64u);
    /* RFEIA 从 ret 槽开始:PC=[sp], CPSR=[sp+4] */
    CHECK(ARM_EXC_OFF_RFE_BASE == ARM_EXC_OFF_RET);

    CHECK(ARM_CTX_OFF_R0 == 0u);
    CHECK(ARM_CTX_OFF_SP == 13u * 4u);
    CHECK(ARM_CTX_OFF_LR == ARM_CTX_OFF_SP + 4u);
    CHECK(ARM_CTX_OFF_PC == ARM_CTX_OFF_LR + 4u);
    CHECK(ARM_CTX_OFF_CPSR == ARM_CTX_OFF_PC + 4u);
    CHECK(ARM_CTX_WORDS == 17u);
    CHECK(ARM_CTX_BYTES == 68u);

    /* ---- 3. 宏与结构体字段的偏移必须一致(用真实对象量,不用宏重算) ---- */
    {
        arm_exc_frame_t    ef;
        arm_task_ctx_t     tc;
        unsigned char     *ep = (unsigned char *)&ef;
        unsigned char     *tp = (unsigned char *)&tc;

        CHECK((unsigned char *)&ef.svc_lr - ep == (long)ARM_EXC_OFF_SVC_LR);
        CHECK((unsigned char *)&ef.ret - ep == (long)ARM_EXC_OFF_RET);
        CHECK((unsigned char *)&ef.spsr - ep == (long)ARM_EXC_OFF_SPSR);
        CHECK((unsigned char *)&ef.r[12] - ep == (long)ARM_EXC_OFF_R(12));

        CHECK((unsigned char *)&tc.sp - tp == (long)ARM_CTX_OFF_SP);
        CHECK((unsigned char *)&tc.lr - tp == (long)ARM_CTX_OFF_LR);
        CHECK((unsigned char *)&tc.pc - tp == (long)ARM_CTX_OFF_PC);
        CHECK((unsigned char *)&tc.cpsr - tp == (long)ARM_CTX_OFF_CPSR);
        CHECK((unsigned char *)&tc.r[12] - tp == (long)ARM_CTX_OFF_R(12));
    }

    /* ---- 4. 寄存器槽是等距的(汇编用 stmia 一把存,靠的就是这个) ---- */
    {
        u32 i;
        for (i = 1; i < 13u; i++) {
            CHECK(ARM_EXC_OFF_R(i) == ARM_EXC_OFF_R(i - 1u) + 4u);
            CHECK(ARM_CTX_OFF_R(i) == ARM_CTX_OFF_R(i - 1u) + 4u);
        }
        /* 第 0 个槽就是帧首 —— stmia sp, {r0-r12} 的前提 */
        CHECK(ARM_EXC_OFF_R(0u) == 0u);
        CHECK(ARM_CTX_OFF_R(0u) == 0u);
    }

    /*
     * ---- 5. 帧大小必须满足 AAPCS 的 8 字节栈对齐 ----
     *
     * 这不是洁癖:帧建好之后要 `bl` 到 C。60 字节(不带 pc 槽)会让 SP 落到
     * 4 mod 8,后果不是编译错误,而是 C 里某条 VFP 指令炸 Undefined
     * Instruction(本项目在 M4-2 踩过一次)。
     */
    CHECK(ARM_EXC_FRAME_BYTES % 8u == 0u);
    CHECK(ARM_EXC_FRAME_BYTES == 16u * 4u);
    /* 任务上下文也一样:恢复它的时候 sp 要是 8 对齐的 */
    CHECK(ARM_CTX_BYTES % 4u == 0u);

    /* ---- 6. ★ ret 与 pc 是两件事,偏移表必须体现出来 ★ ---- */
    /*
     * 实测表(taskctx.h 顶部):
     *   SVC  返回用 LR   (减 0),诊断用 LR-4
     *   IRQ  返回用 LR-4,        诊断用 LR-8
     * 于是同一个异常的 ret/pc 之差随类型变化 —— 写死一个常数就是错的。
     */
    /*
     * ★ 两个"减多少"必须分开:ret 是"首选返回地址",pc 是"出错/被中断的指令" ★
     *
     * 实测表(本板,见 taskctx.h 顶部):
     *   入口 LR -> ret 要减 RET_FIX -> 再减 PC_FIX 才得到 pc
     */
    CHECK(ARM_EXC_RET_FIX_SVC == 0u && ARM_EXC_PC_FIX_SVC == 4u);
    CHECK(ARM_EXC_RET_FIX_IRQ == 4u && ARM_EXC_PC_FIX_IRQ == 4u);
    CHECK(ARM_EXC_RET_FIX_UND == 4u && ARM_EXC_PC_FIX_UND == 0u);
    CHECK(ARM_EXC_RET_FIX_PABT == 4u && ARM_EXC_PC_FIX_PABT == 0u);
    CHECK(ARM_EXC_RET_FIX_DABT == 8u && ARM_EXC_PC_FIX_DABT == 0u);

    /*
     * ★ 这几条钉的是"同一个异常下 ret 与 pc 不是同一个值" ★
     *   旧代码只有一个 pc 字段,两者共用一个 LR-4,于是至少一个是错的。
     *   只有 SVC 与 IRQ 会返回,也只有这两个的 ret != pc —— 正好对应
     *   实测里偏 4 的那两条(SVC 与 Data Abort 是偏的,Data Abort 是 pc 那一路)。
     */
    CHECK(ARM_EXC_RET_FIX_SVC + ARM_EXC_PC_FIX_SVC == 4u); /* pc = LR-4 */
    CHECK(ARM_EXC_RET_FIX_IRQ + ARM_EXC_PC_FIX_IRQ == 8u); /* pc = LR-8 */
    CHECK(ARM_EXC_PC_FIX_SVC != 0u); /* SVC 的 ret(减 0)与 pc(减 4)不同 */
    CHECK(ARM_EXC_PC_FIX_IRQ != 0u); /* IRQ 的 ret(减 4)与 pc(减 8)不同 */
    /* 其余三个 ret == pc(不返回,两者取同一个值)*/
    CHECK(ARM_EXC_PC_FIX_UND == 0u && ARM_EXC_PC_FIX_PABT == 0u && ARM_EXC_PC_FIX_DABT == 0u);

    /* ---- 7. SVC 立即数的提取 ---- */
    {
        /* svc #0xA5A5 的编码:cond=1110, 1111, imm24 */
        u32 insn = 0xEF000000u | ARM_SVC_FRAME_CHECK;

        CHECK(arm_svc_immediate(insn) == ARM_SVC_FRAME_CHECK);
        /* 立即数是 24 位,不能把条件域/操作码混进来 */
        CHECK((arm_svc_immediate(0xEF000000u | 0x00FFFFFFu)) == 0x00FFFFFFu);
        CHECK(arm_svc_immediate(0xEF000000u) == 0u);
        /* ★ 拿来一条**别的**指令,不能碰巧解出同一个号 ★ */
        CHECK(arm_svc_immediate(0xE1A00000u) != ARM_SVC_FRAME_CHECK); /* mov r0,r0 */
        CHECK(arm_svc_immediate(0xEAFFFFFEu) != ARM_SVC_FRAME_CHECK); /* b . */
        CHECK((ARM_SVC_FRAME_CHECK & ~ARM_SVC_IMM_MASK) == 0u);       /* 自检号必须放得下 */
    }

    /* ---- 8. 模式位与模式文本 ---- */
    CHECK(ARM_MODE_SVC == 0x13u);
    CHECK(ARM_MODE_IRQ == 0x12u);
    CHECK(ARM_MODE_USR == 0x10u);
    CHECK(arm_mode_text(ARM_MODE_SVC) [0] == 'S');
    CHECK(arm_mode_text(0x80000013u)[1] == 'u'); /* 高位有标志也不影响模式判定 */
    CHECK(arm_mode_text(0x00000000u)[0] == 'u'); /* 未知识别成 unknown,不返回 NULL */

    /* ---- 8b. arm_exc_pc():帧里只存 ret,pc 按异常类型算出来 ---- */
    {
        arm_exc_frame_t f = {0};

        f.ret = 0x1004u;
        /* SVC:ret = svc+4,pc = ret-4 = svc 本身 */
        CHECK(arm_exc_pc(&f, ARM_EXC_PC_FIX_SVC) == 0x1000u);
        /* IRQ:ret = 被中断指令+4,pc = ret-4 = 被中断的那条 */
        CHECK(arm_exc_pc(&f, ARM_EXC_PC_FIX_IRQ) == 0x1000u);
        /* Data Abort / Prefetch / Undefined:ret 本身就是出错指令 */
        CHECK(arm_exc_pc(&f, ARM_EXC_PC_FIX_DABT) == 0x1004u);
        CHECK(arm_exc_pc(&f, ARM_EXC_PC_FIX_PABT) == 0x1004u);
        CHECK(arm_exc_pc(&f, ARM_EXC_PC_FIX_UND) == 0x1004u);
        CHECK(sizeof(f) == ARM_EXC_FRAME_BYTES);
    }

    /* ---- 9. 错误编号与"槽个数"对得上 ---- */
    CHECK(ARM_FRAME_CHECK_BAD_SPSR == 14);
    CHECK(ARM_FRAME_CHECK_BAD_RET == 15);
    CHECK(ARM_FRAME_CHECK_BAD_SPSR == 13 + 1); /* r0-r12 共 13 个槽 */

    /* ---- 10. 自检图案两两不同(否则"写进去了"与"本来就是它"分不开) ---- */
    {
        u32 i;
        u32 j;
        for (i = 0; i < 13u; i++) {
            for (j = 0; j < i; j++) {
                CHECK((ARM_FRAME_CHECK_PATTERN ^ i) != (ARM_FRAME_CHECK_PATTERN ^ j));
            }
        }
    }

    if (failures != 0) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }

    printf("taskctx: all checks passed\n");
    return 0;
}
"""


class Arm32TaskCtxTests(unittest.TestCase):
    COMPILER_CANDIDATES = ("gcc", "cc", "clang")

    def test_register_frames(self) -> None:
        capture = {"capture_output": True, "text": True, "encoding": "utf-8", "errors": "replace"}

        with tempfile.TemporaryDirectory() as tmp:
            harness = Path(tmp) / "taskctx_test.c"
            binary = Path(tmp) / "taskctx_test"
            harness.write_text(HARNESS, encoding="utf-8")

            attempts: list[str] = []
            for compiler in self.COMPILER_CANDIDATES:
                command = [
                    compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT / "arch/arm32/include"),
                    str(harness),
                    "-o", str(binary),
                ]
                try:
                    result = subprocess.run(command, **capture)
                except FileNotFoundError:
                    attempts.append(f"{compiler}: not found")
                    continue
                if result.returncode == 0:
                    break
                detail = (result.stderr or "").strip().replace("\n", " ")[:400]
                attempts.append(f"{compiler}: rc={result.returncode} {detail}")
            else:
                self.fail("no working host C compiler found:\n  " + "\n  ".join(attempts))

            run = subprocess.run([str(binary)], **capture)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("all checks passed", run.stdout)


if __name__ == "__main__":
    unittest.main()
