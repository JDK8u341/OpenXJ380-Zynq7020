"""源 OS yield 型互斥锁的宿主穷尽测(M4-11.1)。

被测代码是 `arch/arm32/src/mutex.c` —— 它不含 MMIO/CP15/内联汇编/时间源,
唯一的对外依赖是四个函数指针(取当前任务 / 让出 / 进临界区 / 出临界区),
所以宿主机能直接编译,并且能构造出板子上**没法构造**的场景:

    板子上:  "等锁时持有者放锁"依赖真实的调度与抢占,只能靠时序碰;
    这里   :  让出钩子在**第 N 次**让出时替持有者放锁 —— 于是
             "让出了几次""第几次之后拿到的"都是**确定值**,逐值钉住。

**为什么状态机的判据必须在这里**:与 `sched.h` 顶部的分工同一条 ——
板级证据只能证明"机制接上了、真的被竞争过",证明不了
"`-EPERM` 与 `-EDEADLK` 的分支没有走错"。

三个"钉住源 OS 行为"的地方(照抄,不修正):
  1. `-EINVAL / -EDEADLK / -EPERM / -EBUSY` 四个返回值的**分支条件**;
  2. `trylock` **一次 yield 都不发生**(否则它就不是非阻塞的);
  3. 没有 current 任务时,空闲的锁会命中 `owner == current`(NULL == NULL)
     ⇒ 递归锁"返回 0 但没拿到锁"。见 arch/mutex.h 的"边界 1"。

参考:
  arch/arm32/include/arch/mutex.h   设计说明与源 OS 出处
  arch/arm32/src/mutex.c            实现
  kernel/task/mutex.cpp             源 OS 的同一把锁(189 行)
"""

from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

KR_PREFIXED = ("memcpy", "memmove", "memset", "memcmp", "strlen", "strcmp", "strncmp",
               "strcpy", "strncpy", "strcat", "strchr", "strrchr", "strtok", "isdigit", "atoi")

HARNESS = r"""
int printf(const char *fmt, ...);

#include <arch/errno.h>
#include <arch/mutex.h>
#include <arch/tcb.h>

static int failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL %d: %s\n", __LINE__, #cond);                                              \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

/*
 * 两个假 TCB。**只用它们的地址**当身份 —— 状态机不认识 TCB 的内容,
 * 这一点本身就是要钉住的:"owner 是一根指针,不是线程号"。
 */
static struct arm_thread_control_block ta;
static struct arm_thread_control_block tb;
static struct arm_thread_control_block tc; /* 第 12 节里"半路抢走"的第三个线程 */

static mutex_t g_m;

static tcb_t g_cur;           /* 钩子返回的"当前任务" */
static u32   g_yields;        /* 让出钩子被调用了几次 */
static u32   g_enter_depth;   /* 临界区嵌套深度(正常情况下 yield 时必须是 0)*/
static u32   g_enter_count;
static u32   g_leave_count;
static u32   g_yield_in_critical; /* ★ 在临界区里让出的次数 —— 必须恒为 0 */

static u32   g_release_after; /* 第几次让出之后,替持有者放锁(0 = 不放)*/
static u32   g_steal_at;      /* 第几次让出时,让第三个线程把锁抢走(0 = 不抢)*/
static u32   g_steal_done;
static mutex_t *g_wait_m;     /* 要替谁放锁 */

static tcb_t hook_current(void)
{
    return g_cur;
}

/*
 * ★ 让出钩子同时是"模拟调度器" ★
 *
 * 板子上"等锁时持有者放锁"是别人干的;这里由它代劳,而且**只在第 N 次**
 * 让出时放 —— 于是"让出几次才拿到锁"是确定值。
 *
 * ⚠ 代劳时要**先把 current 切成持有者**:`mutex_unlock` 判的是
 *   `owner == current`,用等待者的身份去放锁会被正确地拒绝(-EPERM),
 *   而那会让这个测试**看起来**是"等锁永远等不到"。
 */
static void hook_yield(void)
{
    g_yields++;

    if (g_enter_depth != 0u) {
        g_yield_in_critical++; /* 持着自旋锁去让出 = 死锁的写法 */
    }

    if (g_wait_m == NULL) {
        return;
    }

    /* 半路杀出第三者:持有者放锁,而锁被 tc 抢走(等待者必须继续等)*/
    if ((g_steal_at != 0u) && (g_yields == g_steal_at)) {
        tcb_t save = g_cur;

        g_cur = g_wait_m->owner;
        (void)mutex_unlock(g_wait_m);
        g_cur = &tc;
        (void)mutex_lock(g_wait_m);
        g_cur = save;
        g_steal_done = 1u;
    }

    if ((g_release_after != 0u) && (g_yields == g_release_after)) {
        tcb_t save = g_cur;

        g_cur = g_wait_m->owner;
        (void)mutex_unlock(g_wait_m);
        g_cur = save;
    }
}

static void hook_enter(void)
{
    g_enter_depth++;
    g_enter_count++;
}

static void hook_leave(void)
{
    g_leave_count++;
    if (g_enter_depth > 0u) {
        g_enter_depth--;
    }
}

static const mutex_ops_t g_hooks = { hook_current, hook_yield, hook_enter, hook_leave };

/* 每节开始时的统一重置:递归锁 + 当前任务 = ta + 计数器清零 */
static void reset(void)
{
    mutex_create(&g_m, true);
    g_cur           = &ta;
    g_yields        = 0u;
    g_release_after = 0u;
    g_steal_at      = 0u;
    g_steal_done    = 0u;
    g_wait_m        = NULL;
}

int main(void)
{
    mutex_set_ops(&g_hooks);

    /* ---- 1. create:初值逐字段钉住 ---- */
    mutex_create(&g_m, true);
    CHECK(g_m.state == MUTEX_UNLOCKED);
    CHECK(g_m.owner == NULL);
    CHECK(g_m.rcc == 0u);
    CHECK(g_m.rec == true);
    CHECK(!mutex_is_locked(&g_m));
    CHECK(mutex_get_owner(&g_m) == NULL);

    mutex_create(&g_m, false);
    CHECK(g_m.rec == false);
    mutex_create(NULL, true); /* 源 OS 是静默返回 —— 不崩即可 */

    /* ---- 2. 获取 / 递归 / 释放 ---- */
    reset();
    CHECK(mutex_lock(&g_m) == 0);
    CHECK(g_yields == 0u); /* 空闲的锁不该让出 */
    CHECK(mutex_is_locked(&g_m));
    CHECK(mutex_get_owner(&g_m) == &ta);
    CHECK(g_m.rcc == 1u);

    CHECK(mutex_lock(&g_m) == 0); /* ← 递归(rec = true)*/
    CHECK(g_m.rcc == 2u);
    CHECK(g_yields == 0u); /* 自己持有的锁也不让出 */

    CHECK(mutex_unlock(&g_m) == 0);
    CHECK(g_m.rcc == 1u);
    CHECK(mutex_is_locked(&g_m)); /* 还没放干净:别人依旧进不来 */
    CHECK(mutex_get_owner(&g_m) == &ta);

    CHECK(mutex_unlock(&g_m) == 0);
    CHECK(g_m.rcc == 0u);
    CHECK(!mutex_is_locked(&g_m));
    CHECK(mutex_get_owner(&g_m) == NULL);

    CHECK(mutex_unlock(&g_m) == -EPERM); /* 没人持有时放锁 */
    CHECK(g_m.state == MUTEX_UNLOCKED);  /* 状态没被弄坏 */

    /* ---- 3. 非递归锁自锁 ⇒ -EDEADLK ---- */
    mutex_create(&g_m, false);
    g_cur    = &ta;
    g_yields = 0u;
    CHECK(mutex_lock(&g_m) == 0);
    CHECK(mutex_lock(&g_m) == -EDEADLK);
    CHECK(mutex_trylock(&g_m) == -EDEADLK);
    CHECK(g_m.rcc == 1u);   /* 计数一个都没多 */
    CHECK(g_yields == 0u);  /* 自锁不是"等",一次都不让出 */
    CHECK(mutex_unlock(&g_m) == 0);
    CHECK(!mutex_is_locked(&g_m));

    /* ---- 4. 被别人占用:trylock 不阻塞、**不让出** ---- */
    reset();
    CHECK(mutex_lock(&g_m) == 0); /* ta 持有 */
    g_cur    = &tb;
    g_yields = 0u;
    CHECK(mutex_trylock(&g_m) == -EBUSY);
    CHECK(g_yields == 0u); /* ★ 非阻塞的定义就是这一条 */
    CHECK(mutex_get_owner(&g_m) == &ta);
    CHECK(g_m.rcc == 1u);
    CHECK(mutex_is_locked(&g_m));

    /* ---- 5. 被别人占用:lock 让出 + 重试,直到拿到 ---- */
    g_wait_m        = &g_m;
    g_release_after = 3u;
    CHECK(mutex_lock(&g_m) == 0); /* 第 3 次让出时持有者放锁,再进来就拿到了 */
    CHECK(g_yields == 3u);
    CHECK(mutex_get_owner(&g_m) == &tb);
    CHECK(g_m.rcc == 1u);
    CHECK(mutex_unlock(&g_m) == 0);

    /*
     * 让出次数是"到拿到为止"的次数,不是常数 ——
     * 换个 N 再跑一遍,免得上面那条其实是"总是 3 次"。
     */
    reset();
    CHECK(mutex_lock(&g_m) == 0);
    g_cur           = &tb;
    g_yields        = 0u;
    g_wait_m        = &g_m;
    g_release_after = 1u;
    CHECK(mutex_lock(&g_m) == 0);
    CHECK(g_yields == 1u);
    CHECK(mutex_get_owner(&g_m) == &tb);
    CHECK(mutex_unlock(&g_m) == 0);

    /* ---- 6. 非持有者放锁 ⇒ -EPERM,且一个字段都不许动 ---- */
    reset();
    CHECK(mutex_lock(&g_m) == 0); /* ta 持有 */
    g_cur = &tb;
    CHECK(mutex_unlock(&g_m) == -EPERM);
    CHECK(g_m.state == MUTEX_LOCKED);
    CHECK(g_m.rcc == 1u);
    CHECK(mutex_get_owner(&g_m) == &ta);
    CHECK(mutex_is_locked(&g_m));
    g_cur = &ta;
    CHECK(mutex_unlock(&g_m) == 0);

    /* ---- 7. destroy:锁着的时候 -EBUSY;销毁之后一律 -EINVAL ---- */
    reset();
    CHECK(mutex_lock(&g_m) == 0);
    CHECK(mutex_destroy(&g_m) == -EBUSY);
    CHECK(g_m.state == MUTEX_LOCKED); /* 没被"销毁掉" */
    CHECK(mutex_unlock(&g_m) == 0);

    CHECK(mutex_destroy(&g_m) == 0);
    CHECK(g_m.state == MUTEX_DESTROYED);
    CHECK(g_m.owner == NULL);
    CHECK(g_m.rcc == 0u);
    CHECK(!mutex_is_locked(&g_m));
    CHECK(mutex_get_owner(&g_m) == NULL);

    g_yields = 0u;
    CHECK(mutex_lock(&g_m) == -EINVAL); /* 已销毁 ⇒ 立刻返回,不等待、不让出 */
    CHECK(g_yields == 0u);
    CHECK(mutex_trylock(&g_m) == -EINVAL);
    CHECK(mutex_unlock(&g_m) == -EINVAL);
    CHECK(mutex_destroy(&g_m) == 0); /* 重复销毁:源 OS 也返回 0 */
    CHECK(mutex_destroy(NULL) == -EINVAL);

    /* ---- 8. NULL 一把锁 ---- */
    CHECK(mutex_lock(NULL) == -EINVAL);
    CHECK(mutex_trylock(NULL) == -EINVAL);
    CHECK(mutex_unlock(NULL) == -EINVAL);
    CHECK(!mutex_is_locked(NULL));
    CHECK(mutex_get_owner(NULL) == NULL);

    /* ---- 9. errno 数值(与源 OS 的四个返回值逐值对应)---- */
    CHECK(EPERM == 1);
    CHECK(EBUSY == 16);
    CHECK(EINVAL == 22);
    CHECK(EDEADLK == 35);

    /* ---- 10. 临界区进出口必须配对(漏一个 = 板子上同核自死锁)---- */
    CHECK(g_enter_count > 0u); /* 非空转:上面真的走过临界区 */
    CHECK(g_enter_count == g_leave_count);
    CHECK(g_enter_depth == 0u);
    CHECK(g_yield_in_critical == 0u); /* ★ 从不在临界区里让出 ★ */

    /* ---- 11. ★ 照抄过来的边界:没有 current 任务 ★ ---- */
    /*
     * 源 OS 在"锁空闲 + 没有 current"时会先命中 `owner == current`
     * (NULL == NULL):rec = false ⇒ -EDEADLK;rec = true ⇒ **返回 0
     * 却没有真的拿到锁**。我们照抄这个行为(理由见 arch/mutex.h 的"边界 1"
     * —— 尊重源 OS,且它在 ARM 侧不可达),所以这里把它**钉住**:
     * 将来谁"顺手修好"了它,这条测试会红,而不是悄悄地改了语义。
     */
    mutex_create(&g_m, false);
    g_cur    = NULL;
    g_yields = 0u;
    CHECK(mutex_lock(&g_m) == -EDEADLK);
    CHECK(g_m.state == MUTEX_UNLOCKED); /* 一个字段都没动 */
    CHECK(g_m.rcc == 0u);
    CHECK(g_yields == 0u);

    mutex_create(&g_m, true);
    g_yields = 0u;
    CHECK(mutex_lock(&g_m) == 0);       /* ★ 返回 0 …… */
    CHECK(g_m.state == MUTEX_UNLOCKED); /* …… 但锁并没有被拿走 */
    CHECK(g_m.rcc == 1u);               /* 计数却涨了(源 OS 就是这样)*/
    CHECK(mutex_unlock(&g_m) == -EPERM);/* 于是连放锁都放不掉 */
    CHECK(g_yields == 0u);

    /* ---- 12. 让出之后"世界变了":每次循环都必须重新判定 ---- */
    /*
     * 第 5 节验的是"能拿到";这一节验的是**它每次都重新读锁**,而不是拿着
     * 第一次的判定就下结论:让出的间隙里,锁被**第三者 tc 抢走** ——
     * 等待者于是必须继续让出(而不是以为自己拿到了)。
     *
     * 板子上这一类交错是随机的;这里把它做成了确定序列:
     *   第 1 次让出:ta 放锁 → tc 抢到
     *   第 3 次让出:tc 放锁 → tb(等待者)拿到
     */
    reset();
    CHECK(mutex_lock(&g_m) == 0); /* ta 持有 */
    g_cur           = &tb;
    g_yields        = 0u;
    g_wait_m        = &g_m;
    g_steal_at      = 1u;
    g_release_after = 3u;
    CHECK(mutex_lock(&g_m) == 0);
    CHECK(g_steal_done == 1u);          /* ★ 半路真的被抢走过 ★ */
    CHECK(g_yields == 3u);              /* 于是多等了两轮 */
    CHECK(mutex_get_owner(&g_m) == &tb); /* 最后拿到的是等待者 */
    CHECK(g_m.rcc == 1u);
    CHECK(mutex_unlock(&g_m) == 0);

    mutex_set_ops(NULL);
    if (failures != 0) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }

    printf("mutex: all checks passed\n");
    return 0;
}
"""


class Arm32MutexTests(unittest.TestCase):
    COMPILER_CANDIDATES = ("gcc", "cc", "clang")

    def test_yield_mutex_state_machine(self) -> None:
        capture = {"capture_output": True, "text": True, "encoding": "utf-8", "errors": "replace"}

        with tempfile.TemporaryDirectory() as tmp:
            harness = Path(tmp) / "mutex_test.c"
            binary = Path(tmp) / "mutex_test"
            harness.write_text(HARNESS, encoding="utf-8")

            attempts: list[str] = []
            for compiler in self.COMPILER_CANDIDATES:
                command = [
                    compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT / "arch/arm32/include"),
                ]
                for fn in KR_PREFIXED:
                    command.append(f"-D{fn}=krtest_{fn}")
                command += [
                    str(harness),
                    str(ROOT / "arch/arm32/src/mutex.c"),
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
