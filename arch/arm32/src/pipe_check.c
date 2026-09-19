/*
 * 管道(pipefs)验收(M4A-1.5)
 *
 * 判据为什么是这几条、以及"**不要读空管道**"那条安全约束的理由,
 * 都写在 <arch/pipe_check.h> 的文件头上。这里只写实现上的取舍。
 */

#include <arch/pipe_check.h>

#include <arch/console.h>
#include <krlibc.h>

/* ------------------------------------------------------------------ */
/* 落地层转发(手写声明;理由见 upstream_api.cpp 的管道那一节)          */
/* ------------------------------------------------------------------ */

extern int arm_pipefs_setup(void);
extern int arm_pipe_create(void);
extern int arm_pipe_write(const void *data, unsigned int len);
extern int arm_pipe_read(void *buf, unsigned int len);
extern int arm_pipe_fill(void);

/* ------------------------------------------------------------------ */

static pipe_check_t g_result;
static u8           g_payload[PIPE_CHECK_PAYLOAD_LEN];
static u8           g_readback[PIPE_CHECK_PAYLOAD_LEN];
static bool         g_ran;

const pipe_check_t *pipe_check_run(void)
{
    int i;

    if (g_ran) {
        return &g_result;
    }
    g_ran = true;

    memset(&g_result, 0, sizeof(g_result));
    g_result.fill_after = -1;
    g_result.fill_drain = -1;
    g_result.short_read = -1;

    /* ① 起搏:注册 pipefs + 建 /pipe + 挂载 */
    g_result.setup = arm_pipefs_setup();

    /* ② 造一个管道(两个节点 + 一块 8KB 缓冲)*/
    g_result.create = arm_pipe_create();

    if (g_result.setup != 0 || g_result.create != 0) {
        console_printf(" Pipe check  : setup=%d create=%d (skipped)\n",
                       g_result.setup, g_result.create);
        return &g_result;
    }

    /* ③ 写(载荷是可验证的模式,不是全 0)*/
    for (i = 0; i < PIPE_CHECK_PAYLOAD_LEN; i++) {
        g_payload[i] = (u8)((i * 13u) + 7u);
    }
    memset(g_readback, 0xAA, sizeof(g_readback)); /* 哨兵:没读到就留着 0xAA */

    g_result.write = arm_pipe_write(g_payload, PIPE_CHECK_PAYLOAD_LEN);

    /* ④ ★ 独立观测点:管道里**现在**有多少字节(不是"它说写了多少")*/
    g_result.fill_after = arm_pipe_fill();

    /* ⑤ 读回 + 逐字节比对(⚠ 上一步保证管道非空 —— 读空管道会阻塞)*/
    g_result.read = arm_pipe_read(g_readback, PIPE_CHECK_PAYLOAD_LEN);

    g_result.roundtrip = 0;
    if (g_result.read == PIPE_CHECK_PAYLOAD_LEN &&
        memcmp(g_payload, g_readback, PIPE_CHECK_PAYLOAD_LEN) == 0) {
        g_result.roundtrip = 1;
    }

    /* ⑥ ★ 另一个方向:读完之后管道必须**空** */
    g_result.fill_drain = arm_pipe_fill();

    /*
     * ⑦ 短读:写 8 字节,按 32 字节去读 —— 只能拿到 8。
     *
     * 这一条证的是"读的返回量由**管道里的存量**决定,而不是由请求量决定"。
     * ⚠ 同样必须**先写后读**(见头文件里那条安全约束)。
     */
    memset(g_readback, 0xAA, sizeof(g_readback));
    g_result.short_write = arm_pipe_write(g_payload, PIPE_CHECK_SHORT_LEN);
    g_result.short_read  = arm_pipe_read(g_readback, PIPE_CHECK_PAYLOAD_LEN);

    console_printf(" Pipe check  : write=%d fill=%d read=%d roundtrip=%s drain=%d short=%d\n",
                   g_result.write, g_result.fill_after, g_result.read,
                   g_result.roundtrip ? "PASS" : "FAIL",
                   g_result.fill_drain, g_result.short_read);

    return &g_result;
}
