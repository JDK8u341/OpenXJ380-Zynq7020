/*
 * pty 配对验收(M4A-1.5)
 *
 * 判据为什么是这几条、"不要读空的那一端"这条安全约束、以及载荷为什么要绕开
 * `'\r'`/`'\n'`,都写在 <arch/pty_check.h> 的文件头上。这里只写实现取舍。
 */

#include <arch/pty_check.h>

#include <arch/console.h>
#include <krlibc.h>

/* ------------------------------------------------------------------ */
/* 落地层转发(手写声明;理由见 upstream_api.cpp 的 pty 一节)           */
/* ------------------------------------------------------------------ */

extern int arm_pty_pair_create(void);
extern int arm_pty_slave_path(char *out, unsigned int len);
extern int arm_pty_master_write(const void *data, unsigned int len);
extern int arm_pty_slave_write(const void *data, unsigned int len);
extern int arm_pty_master_read(void *buf, unsigned int len);
extern int arm_pty_slave_read(void *buf, unsigned int len);

/* ------------------------------------------------------------------ */

static pty_check_t g_result;
static char        g_send[PTY_CHECK_PAYLOAD_LEN];
static char        g_recv[PTY_CHECK_PAYLOAD_LEN];
static bool        g_ran;

/*
 * 载荷:只用字母(`'A'` 起循环),**刻意避开 `'\r'` 与 `'\n'`**。
 *
 * ★ 两个方向的约束是**相反的**,而这是上板实测出来的(第一版就挂在这里):
 *
 *   - **方向 A(写主设备 → 读从设备)**:这条路是**输入路径**,
 *     默认 termios 是**规范模式**(`c_lflag = ISIG | ICANON | ECHO | …`,
 *     `pty.cpp:62`),而 `pts_data_available()`(`pty.cpp:117-129`)在 ICANON 下
 *     **只认行结束符**:缓冲区里没有 `'\n'`/VEOF/VEOL 时它返回 0 ⇒
 *     `pts_read` 会一直空转等下去。
 *     ⇒ 方向 A 的载荷**必须以 `'\n'` 结尾**(而且只能有一个,否则
 *       "读回多少"就不好判了)。第一版为了躲 ICRNL 把 `'\n'` 也避开了,
 *       于是**当场挂住**(日志停在 `pty: initialized` 之后)。
 *   - **方向 B(写从设备 → 读主设备)**:这条路是**输出路径**,
 *     `c_oflag = OPOST | ONLCR`(`pty.cpp:60`)⇒ `pts_write` 会把 `'\n'`
 *     改写成 `"\r\n"`(`pty.cpp:503`)⇒ 载荷**必须避开 `'\n'`/`'\r'`**,
 *     否则"逐字节相同"会被行规约搅乱。
 *
 * ⚠ 顺序:先 B 后 A。默认 termios 里 `ECHO` 是开着的,输入路径有可能把
 *   读走的内容**回显**到主设备那一侧的缓冲;若先做 A,那些回显字节会挡在
 *   方向 B 的读取前面。先做 B(读主设备)就完全避开了这件事。
 */
static void fill_payload(char *buf, char seed)
{
    int i;

    for (i = 0; i < PTY_CHECK_PAYLOAD_LEN; i++) {
        buf[i] = (char)('A' + ((seed + i) % 26));
    }
}

/* 输入路径(方向 A)的载荷:字母 + 一个结尾的 `'\n'` —— 规范模式的放行条件 */
static void fill_input_payload(char *buf, char seed, int len)
{
    int i;

    fill_payload(buf, seed);
    for (i = 0; i < len - 1; i++) {
        buf[i] = (char)('A' + ((seed + i) % 26));
    }
    buf[len - 1] = '\n';
}

const pty_check_t *pty_check_run(void)
{
    if (g_ran) {
        return &g_result;
    }
    g_ran = true;

    memset(&g_result, 0, sizeof(g_result));
    g_result.a_read    = -1;
    g_result.a_short_r = -1;
    g_result.b_read    = -1;

    /* ① 配对:主设备 + **问出来的**从设备 */
    g_result.pair = arm_pty_pair_create();
    if (g_result.pair != 0) {
        console_printf(" PTY check   : pair failed (%d)\n", g_result.pair);
        return &g_result;
    }

    /* 把算出来的从设备路径拿回来做展示(证据:它是 /dev/pts/<n>,不是写死的)*/
    g_result.path_len = arm_pty_slave_path(g_result.path, PTY_CHECK_PATH_LEN);
    if (g_result.path_len < 0) {
        g_result.path_len = 0;
        g_result.path[0]  = '\0';
    }

    /* ---- 方向 B:写从设备 → 读主设备(载荷避开 \n/\r;先做它,见头注释)---- */
    fill_payload(g_send, 11);
    memset(g_recv, 0x55, sizeof(g_recv));

    g_result.b_write = arm_pty_slave_write(g_send, PTY_CHECK_PAYLOAD_LEN);
    /* ⚠ 先写后读(读空的一端会空转挂住)*/
    g_result.b_read  = arm_pty_master_read(g_recv, PTY_CHECK_PAYLOAD_LEN);

    g_result.b_roundtrip = 0;
    if (g_result.b_read == PTY_CHECK_PAYLOAD_LEN &&
        memcmp(g_send, g_recv, PTY_CHECK_PAYLOAD_LEN) == 0) {
        g_result.b_roundtrip = 1;
    }

    /* ---- 方向 A:写主设备 → 读从设备(载荷以 `'\n'` 结尾)*/
    fill_input_payload(g_send, 0, PTY_CHECK_PAYLOAD_LEN);
    memset(g_recv, 0x55, sizeof(g_recv));

    g_result.a_write = arm_pty_master_write(g_send, PTY_CHECK_PAYLOAD_LEN);
    g_result.a_read  = arm_pty_slave_read(g_recv, PTY_CHECK_PAYLOAD_LEN);

    g_result.a_roundtrip = 0;
    if (g_result.a_read == PTY_CHECK_PAYLOAD_LEN &&
        memcmp(g_send, g_recv, PTY_CHECK_PAYLOAD_LEN) == 0) {
        g_result.a_roundtrip = 1;
    }

    /* ---- 方向 A 的短读:写 8(7 字母 + `'\n'`)、按 32 读 ⇒ 只拿到 8 ---- */
    fill_input_payload(g_send, 3, PTY_CHECK_SHORT_LEN);
    memset(g_recv, 0x55, sizeof(g_recv));
    g_result.a_short_w = arm_pty_master_write(g_send, PTY_CHECK_SHORT_LEN);
    g_result.a_short_r = arm_pty_slave_read(g_recv, PTY_CHECK_PAYLOAD_LEN);

    console_printf(" PTY check   : slave=%s A(w=%d r=%d rt=%s) B(w=%d r=%d rt=%s) short=%d\n",
                   g_result.path[0] != '\0' ? g_result.path : "(none)",
                   g_result.a_write, g_result.a_read, g_result.a_roundtrip ? "PASS" : "FAIL",
                   g_result.b_write, g_result.b_read, g_result.b_roundtrip ? "PASS" : "FAIL",
                   g_result.a_short_r);

    return &g_result;
}
