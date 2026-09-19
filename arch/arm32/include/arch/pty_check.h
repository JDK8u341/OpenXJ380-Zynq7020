#pragma once

/*
 * pty 配对验收判据(M4A-1.5)
 *
 * ====================================================================
 * 验的是什么
 * ====================================================================
 *
 * `pty.cpp` 进图后,上一笔只验到**起搏**(`pty_init()` 跑完、`/dev/ptmx` 建出来)。
 * 这一组补的是**配对**:开 `/dev/ptmx` 拿到主设备 → 问出从设备号 →
 * 开 `/dev/pts/<n>` → **两个方向各做一次真实往返**。
 *
 * 上游的数据流(`pty.cpp` 实测读出来的,不是猜的):
 *
 *     ptmx_write  → 写进 **pair->slave_buffer**  ⇒ 由 pts_read  读走  (方向 A)
 *     pts_write   → 写进 **pair->master_buffer** ⇒ 由 ptmx_read 读走  (方向 B)
 *
 * ⇒ 两个方向走的是**两块不同的缓冲**、两端是**两个不同的节点**,
 *   所以"两个方向都对"比单向成立强得多(只写一端的假实现过不了两个方向)。
 *
 * ★ 从设备号是 **`TIOCGPTN` 问出来的**,不是假定 0:`ptmx_open` 每条 pair 都从
 *   `pty_id_alloc()` 取号,而这个分配器是共享的(第二次开就是 1)。
 *   写死 0 的判据第一次上板能过、之后随机失败 —— 坑表 57/58 那一类。
 *   从设备路径由落地层算完交回来(`arm_pty_slave_path`),这里只**展示**它。
 *
 * ====================================================================
 * ⚠⚠ 与 pipe 同一条安全约束:不要读**空**的那一端 ⚠⚠
 * ====================================================================
 *
 * `ptmx_read()`(`pty.cpp:244-274`)在没有数据时会 **`scheduler_yield()` 空转**,
 * 只有 `slave_fds == 0` 才返回 0;`pts_read()` 对称。
 * ⇒ 在 kmain 上下文里读空的一端就是**当场挂住**。
 * ⇒ 判据一律**先写后读**;`tests/test_arm32_vfs.py` 里有一条顺序检查把
 *   这条约束钉成机器可判(与 pipe 那条同型)。
 *
 * ⚠ 载荷避开 `'\r'` 与 `'\n'`:默认 termios 下 `ptmx_write` 会按 `ICRNL`
 *   把 `'\r'` 改写成 `'\n'`(`pty.cpp:298-305`),那样"读回内容是否逐字节相同"
 *   就会被**行规约**搅乱 —— 而那不是这一组要验的东西。
 */

#include <arch/types.h>

#define PTY_CHECK_PAYLOAD_LEN 32
#define PTY_CHECK_SHORT_LEN   8
/* 从设备路径的展示缓冲(`/dev/pts/<n>` 够用;留大一点便于看出异常)*/
#define PTY_CHECK_PATH_LEN    32

typedef struct
{
    int  pair;        /* `arm_pty_pair_create()`:0 = 主/从都开好了 */
    int  path_len;    /* 从设备路径长度(证据:它是个真实路径)*/
    char path[PTY_CHECK_PATH_LEN];
    int  a_write;     /* 方向 A:写主设备 */
    int  a_read;      /* 方向 A:读从设备 */
    int  a_roundtrip; /* 1 = 逐字节相同 */
    int  a_short_w;   /* 方向 A 短读:写 8 */
    int  a_short_r;   /* 方向 A 短读:按 32 去读、实际拿到 8 */
    int  b_write;     /* 方向 B:写从设备 */
    int  b_read;      /* 方向 B:读主设备 */
    int  b_roundtrip; /* 1 = 逐字节相同 */
} pty_check_t;

/* 跑一次配对验收。**幂等**。⚠ 必须在 pty 起搏之后、且 VFS 可用时调用 */
const pty_check_t *pty_check_run(void);
