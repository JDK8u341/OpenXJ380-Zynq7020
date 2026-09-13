"""每 CPU 数据(percpu)的单元测试。

被测代码是 arch/arm32/src/percpu.c —— 刻意做成**纯逻辑,不含任何 CP15 或 MMIO**,
所以宿主编译器能直接编译运行。CP15 部分在 percpu_hw.c,只能在板上验证。

**为什么值得测**:本模块最容易出错的地方是**越界核号**。
`percpu_for()` 若返回一个越界指针,后果是某个核把**别的核**的结构体写坏 ——
在板上的表现是"另一个核莫名其妙状态不对",极难定位,而且往往要等到
多核真正并发跑起来才暴露。放在宿主机上一秒就能钉住。

另外钉住"表里的字段被正确重置":`last_intid` 的初值必须是 0xFFFFFFFF
(而不是 0)—— 0 恰好是一个合法的中断号,"还没处理过任何中断"和
"最后处理的是 INTID 0"必须能区分,否则诊断时会读出一个看似合理的错值。

参考:
  arch/arm32/include/arch/percpu.h
  arch/arm32/src/percpu.c
  arch/arm32/src/percpu_hw.c
"""

from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

HARNESS = r"""
#include <stdio.h>

#include <arch/percpu.h>

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
    percpu_t *pc;
    u32       i;

    /* ---- 范围校验 ---- */
    CHECK(percpu_id_valid(0));
    CHECK(percpu_id_valid(PERCPU_MAX_CPUS - 1u));
    CHECK(!percpu_id_valid(PERCPU_MAX_CPUS));
    CHECK(!percpu_id_valid(0xFFFFFFFFu));

    /*
     * 越界必须返回 NULL,而不是"某个地址"。
     * 返回越界指针是本模块最危险的失败模式:调用方会照写不误,
     * 把别的核(或别的全局变量)悄悄改掉。
     */
    CHECK(percpu_for(PERCPU_MAX_CPUS) == NULL);
    CHECK(percpu_for(0xFFFFFFFFu) == NULL);
    CHECK(percpu_for(1u) != NULL);

    /* 表内每个 id 都必须映射到自己那一项,不能互相串 */
    for (i = 0; i < PERCPU_MAX_CPUS; i++) {
        pc = percpu_for(i);
        CHECK(pc != NULL);
        CHECK(pc == &g_percpu[i]);
    }

    /* ---- 重置 ---- */
    pc = percpu_for(1u);
    pc->online     = 7u;
    pc->loops      = 12345u;
    pc->irq_count  = 9u;
    pc->mpidr      = 0xDEADBEEFu;
    pc->spin_retry = 3u;

    percpu_table_reset();

    for (i = 0; i < PERCPU_MAX_CPUS; i++) {
        pc = percpu_for(i);
        CHECK(pc->cpu_id == i); /* 重置后 cpu_id 必须等于它的下标 */
        CHECK(pc->online == 0u);
        CHECK(pc->loops == 0u);
        CHECK(pc->irq_count == 0u);
        CHECK(pc->mpidr == 0u);
        CHECK(pc->spin_retry == 0u);
        CHECK(pc->stack_top == 0u);

        /*
         * last_intid 的初值是 0xFFFFFFFF 而不是 0。
         * 0 是合法的中断号 —— 用 0 当"无"会让诊断分不清
         * "还没处理过中断"和"最后处理的是 INTID 0"。
         */
        CHECK(pc->last_intid == 0xFFFFFFFFu);
    }

    /* 重置必须覆盖掉刚才故意写脏的值 */
    CHECK(percpu_for(1u)->loops == 0u);
    CHECK(percpu_for(1u)->mpidr == 0u);

    /* ---- 上线计数 ---- */
    CHECK(percpu_online_count() == 0u);

    percpu_for(0u)->online = 1u;
    CHECK(percpu_online_count() == 1u);

    percpu_for(1u)->online = 1u;
    CHECK(percpu_online_count() == 2u);

    percpu_for(0u)->online = 0u;
    CHECK(percpu_online_count() == 1u);

    if (failures != 0) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }

    printf("percpu: all checks passed\n");
    return 0;
}
"""


class Arm32PerCpuTests(unittest.TestCase):
    COMPILER_CANDIDATES = ("gcc", "cc", "clang")

    def test_percpu_table_and_index_validation(self) -> None:
        capture = {"capture_output": True, "text": True, "encoding": "utf-8", "errors": "replace"}

        with tempfile.TemporaryDirectory() as tmp:
            harness = Path(tmp) / "percpu_test.c"
            binary = Path(tmp) / "percpu_test"
            harness.write_text(HARNESS, encoding="utf-8")

            attempts: list[str] = []
            for compiler in self.COMPILER_CANDIDATES:
                command = [
                    compiler,
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "arch/arm32/include"),
                    str(harness),
                    str(ROOT / "arch/arm32/src/percpu.c"),
                    "-o",
                    str(binary),
                ]
                try:
                    result = subprocess.run(command, **capture)
                except FileNotFoundError:
                    attempts.append(f"{compiler}: not found")
                    continue
                if result.returncode == 0:
                    break
                detail = (result.stderr or "").strip().replace("\n", " ")[:300]
                attempts.append(f"{compiler}: rc={result.returncode} {detail}")
            else:
                self.fail("no working host C compiler found:\n  " + "\n  ".join(attempts))

            run = subprocess.run([str(binary)], **capture)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("all checks passed", run.stdout)


if __name__ == "__main__":
    unittest.main()
