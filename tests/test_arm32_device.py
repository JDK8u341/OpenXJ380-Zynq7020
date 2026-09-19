"""设备管理器 + devfs 骨架(M4A-1.1b)的单元测试。

分两部分,判据的性质完全不同:

一、**形状契约**(纯文本比对,不编译)
    `arch/arm32/include/arch/device.h` 是"按上游形状写的",而上游那份**不能**
    直接 include(它第 5 行 `#include "proto.hpp"`,拖进整个 x86 主干)。
    于是"形状一致"这件事只能靠**机械比对**来保证 —— 与 errno 的逐值比对
    同一个套路:上游改了字段而移植侧没跟上,这里会立刻报出来。

    比的是:枚举量的名字与顺序、回调 typedef、`device_t` 的**每个成员的
    类型拼写与顺序**、以及我们声明出去的每个原型在上游都存在(允许我们
    是子集 —— 块层那几个函数本阶段刻意不声明)。

二、**往返语义**(宿主编译运行)
    被测代码是 src/device.c + src/id_alloc.c。它们不含 MMIO/CP15,
    所以宿主能直接编。跑的是"M4A-1.1b 的判据":注册 → 按 id 取回 →
    按**名字**取回 → 注销 → 两个方向都消失。

    ⚠ 为什么"按名字取回"是独立判据:`get_device(id)` 只查设备表 —— 一个
      什么都没做的 devfs 也能让它通过。那正是计划 §4.4 警告的
      "只登记不建节点"的假兼容,所以这里还配了一个**破坏性对照**
      (devfs 关掉之后 get_device 照样成功、按名字必须找不到)。

参考:
  arch/arm32/include/arch/device.h(适配记录:与上游的每处差异及理由)
  ★ M4A-1.5 起**真 devfs 是上游 driver/fs/vfs/dev.cpp**(骨架已退场)⇒ 这里只给一个\n  语义桩(见 harness 里的说明);/dev 下节点的证据在**板上**(device_devfs_ab)
  driver/device.cpp、driver/fs/vfs/dev.cpp(上游对应物)
"""

from __future__ import annotations

import re
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

UPSTREAM_DEVICE_H = ROOT / "include" / "device.h"
ARM_DEVICE_H = ROOT / "arch" / "arm32" / "include" / "arch" / "device.h"
ARM_ID_ALLOC_H = ROOT / "arch" / "arm32" / "include" / "arch" / "id_alloc.h"

# 宿主 UCRT 已经导出了这些符号,直接链接会 duplicate symbol
KR_PREFIXED = ("memcpy", "memmove", "memset", "memcmp", "strlen", "strcmp", "strncmp",
               "strcpy", "strncpy", "strcat", "strchr", "strrchr", "strtok", "isdigit",
               "atoi", "strdup")
HEAP_PREFIXED = ("malloc", "calloc", "realloc", "free")


def _strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def _normalize(piece: str) -> str:
    text = " ".join(piece.split())
    # `f(void)` 与上游的 `f()` 是**同一件事**的两种拼写:移植侧用 (void)
    # 是 C 里更明确的写法(空括号在 C 里表示"参数未指定"),而上游是 C++。
    # 归一化掉,免得逼着把 C 写丑。
    return re.sub(r"\(void\)$", "()", text)


def _struct_members(text: str, tag: str, typedef_name: str) -> list[str]:
    """把 `typedef struct [<tag>] { ... } <typedef_name>;` 的成员拆成规范化声明列表。

    tag 允许为空:上游 `id_alloc.h` 写的是 `typedef struct { ... } id_allocator_t;`,
    而 `device.h` 写的是 `typedef struct _device { ... } device_t;` —— 两种都要吃。
    """
    body = re.search(
        r"typedef\s+struct\s*" + re.escape(tag) + r"\s*\{(.*?)\}\s*" + re.escape(typedef_name) + r"\s*;",
        text,
        flags=re.S,
    )
    if body is None:
        raise AssertionError(f"找不到 typedef struct {tag} ... {typedef_name}")
    return [_normalize(p) for p in body.group(1).split(";") if _normalize(p)]


def _top_level_prototypes(text: str) -> set[str]:
    """文件里所有以 `;` 结尾、且带括号的声明(规范化)。

    ⚠ 两处归一化是**必须**的,而且都是被真实改动打出来的:

    1. 按行删预处理指令(`#include`/`#pragma` 等)—— 否则
       `#define BITS_PER_WORD (sizeof(uint32_t) * 8)` 会被当成函数声明。
    2. 去掉 `extern "C" {` —— 2026-09-18 给上游 `device.h` 与 `id_alloc.h`
       加了那个块(理由:实现方是 C、调用方是 C++),它会让紧随其后的第一条
       声明**多出一截前缀**。这个测试当时就是这么被抓住的。
    """
    text = _strip_directives(text).replace('extern "C" {', " ")
    out = set()
    for piece in text.split(";"):
        n = _normalize(piece)
        if "(" in n and ")" in n and not n.startswith("#"):
            out.add(n)
    return out


def _param_types(params: str) -> list[str]:
    """把参数表拆成型别序列 —— **去掉参数名**。

    上游写 `id_alloc(id_allocator_t *)`,移植侧写 `id_alloc(id_allocator_t *allocator)`,
    两者是同一个 API;参数名不该让契约测试失败。
    """
    params = params.strip()
    if params in ("", "void"):
        return []
    out = []
    for p in params.split(","):
        toks = p.split()
        if len(toks) >= 2:
            last = toks[-1]
            if re.fullmatch(r"\*+[A-Za-z_]\w*", last):      # id_allocator_t *allocator
                toks[-1] = last[: len(last) - len(last.lstrip("*"))]
            elif re.fullmatch(r"[A-Za-z_]\w*", last):       # uint32_t id
                toks = toks[:-1]
        out.append(" ".join(toks))
    return out


def _strip_directives(text: str) -> str:
    """按行删掉预处理指令。

    ⚠ 必须**按行**删,不能只在片段开头判断 `#`:加了 `extern "C"` 之后,
    第一条声明的那个片段是以 `#ifdef __cplusplus` 开头的 —— 靠 startsWith('#')
    跳过会把 `id_allocator_create` 整个漏掉(这就是第一版的 bug)。
    顺带也避免 `#define BITS_PER_WORD (sizeof(uint32_t) * 8)` 被当成函数声明。
    """
    return re.sub(r"(?m)^\s*#.*$", "", text)


def _prototype_signatures(text: str) -> dict[str, tuple[str, tuple[str, ...]]]:
    """函数名 -> (返回类型, 参数型别序列)。只取顶层、带括号、以 ; 结尾的声明。"""
    out: dict[str, tuple[str, tuple[str, ...]]] = {}
    # `extern "C" {` 会黏在紧随其后的那条声明的返回类型上 —— 它不是类型的一部分
    text = _strip_directives(text).replace('extern "C" {', " ")
    for piece in text.split(";"):
        n = _normalize(piece)
        m = re.fullmatch(r"(.+?)\b([A-Za-z_]\w*)\s*\((.*)\)", n)
        if not m:
            continue
        out[m.group(2)] = (m.group(1).strip(), tuple(_param_types(m.group(3))))
    return out


class IdAllocContractTests(unittest.TestCase):
    """`arch/arm32/include/arch/id_alloc.h` 必须与上游 `include/id_alloc.h` 同 API。

    为什么移植侧要有一份**声明副本**:上游那份是 C++ 专属的(它拉
    `include/krlibc.h`,而那份用了 `typeof(nullptr)`、`#define NULL 0`、
    `static memmove` —— 在 C 里全编不过,实测)。所以两边唯一的交界是
    "上游实现 + 移植侧 C 声明",而这份副本必须被机械地钉住。
    """

    def setUp(self) -> None:
        self.up = _strip_comments((ROOT / "include" / "id_alloc.h").read_text(encoding="utf-8"))
        self.arm = _strip_comments(ARM_ID_ALLOC_H.read_text(encoding="utf-8"))

    def test_struct_members_match(self) -> None:
        up = _struct_members(self.up, "", "id_allocator_t")
        arm = _struct_members(self.arm, "", "id_allocator_t")
        self.assertTrue(up, "上游 id_allocator_t 没解析到成员")
        self.assertEqual(up, arm, "id_allocator_t 与上游不一致(左=上游,右=移植侧)")

    def test_prototypes_match(self) -> None:
        up = _prototype_signatures(self.up)
        arm = _prototype_signatures(self.arm)

        for name in ("id_allocator_create", "id_alloc", "id_free"):
            self.assertIn(name, up, f"上游少了 {name} —— 解析器或上游都变了")
            self.assertIn(name, arm, f"移植侧的 C 声明里少了 {name}")
            self.assertEqual(up[name], arm[name], f"{name} 的签名与上游不一致")

    def test_upstream_header_is_extern_c(self) -> None:
        """上游那份必须带 `extern "C"` —— 否则 C 侧链不上(符号会被修饰)。

        实测过:没有它时 `kernel/id_alloc.cpp` 编出来的符号是
        `_Z8id_allocP14id_allocator_t`,C 那边根本找不到。
        """
        raw = (ROOT / "include" / "id_alloc.h").read_text(encoding="utf-8")
        self.assertIn('extern "C"', raw, '上游 id_alloc.h 的 extern "C" 块没了')


class DeviceShapeContractTests(unittest.TestCase):
    """形状必须与上游一致 —— 靠机械比对,不靠自觉。"""

    def setUp(self) -> None:
        self.up = _strip_comments(UPSTREAM_DEVICE_H.read_text(encoding="utf-8"))
        self.arm = _strip_comments(ARM_DEVICE_H.read_text(encoding="utf-8"))

    def test_device_t_members_match_upstream_exactly(self) -> None:
        """★ 核心判据:成员的类型拼写与**顺序**逐条相等。

        顺序也要比,是因为 `regist_device(path, vd)` 是**按值**传结构体:
        顺序错了不会报错,只会让两边的字段互相错位。
        """
        up = _struct_members(self.up, "_device", "device_t")
        arm = _struct_members(self.arm, "_device", "device_t")

        self.assertTrue(up, "上游 device_t 一个成员都没解析到 —— 解析器与文件格式脱节了")
        self.assertEqual(up, arm, "device_t 与上游不一致(左=上游,右=移植侧)")

    def test_enum_and_callback_typedefs_match(self) -> None:
        """枚举量的名字/顺序、以及回调 typedef 的拼写。"""
        for pattern in (
            r"typedef\s+enum\s*\{(.*?)\}\s*device_flag_t\s*;",
            r"typedef\s+size_t\s*\(\s*\*read_vbuff\s*\)\s*\([^;]*\)\s*;",
        ):
            up_hits = [_normalize(m.group(0)) for m in re.finditer(pattern, self.up, flags=re.S)]
            arm_hits = [_normalize(m.group(0)) for m in re.finditer(pattern, self.arm, flags=re.S)]
            self.assertTrue(up_hits, f"上游里没解析到 {pattern!r}")
            self.assertEqual(up_hits, arm_hits, f"{pattern!r} 两边不一致")

        up_flags = re.search(r"DEVICE_BLOCK\s*,\s*DEVICE_STREAM\s*,\s*DEVICE_FB", self.up)
        arm_flags = re.search(r"DEVICE_BLOCK\s*,\s*DEVICE_STREAM\s*,\s*DEVICE_FB", self.arm)
        self.assertIsNotNone(up_flags, "上游的 device_flag_t 顺序变了")
        self.assertIsNotNone(arm_flags, "移植侧的 device_flag_t 顺序与上游不符")

    def test_sectors_once_matches(self) -> None:
        for name, text in (("上游", self.up), ("移植侧", self.arm)):
            m = re.search(r"#define\s+SECTORS_ONCE\s+(\d+)", text)
            self.assertIsNotNone(m, f"{name}没有 SECTORS_ONCE")
            self.assertEqual(m.group(1), "8")

    def test_every_prototype_we_declare_exists_upstream(self) -> None:
        """我们声明出去的每个原型,上游都得有**一模一样**的。

        允许我们是子集(块层那几个函数本阶段刻意不声明),但不允许出现
        上游没有的签名 —— 那意味着共享源码时会对不上。
        """
        ours = _top_level_prototypes(self.arm)
        theirs = _top_level_prototypes(self.up)

        missing = []
        for proto in sorted(ours):
            # 移植侧专有的入口(上游没有对应物):内核侧 A/B 与落地层转发
            # 是骨架特有的(上游靠 vfs_open 做这件事),不在比对范围内
            if proto.startswith(("size_t arm_disk_size", "void arm_devfs_setup",
                                 "size_t blk_device_read", "size_t blk_device_write",
                                 "u32 blk_device_calls")):
                continue
            if proto not in theirs:
                missing.append(proto)

        self.assertEqual([], missing, "这些原型在上游 device.h 里找不到:\n  " + "\n  ".join(missing))

    def test_device_table_size_matches_upstream_literal(self) -> None:
        """上游没有这个宏,它写死 256 —— 我们从它的数组声明里把这个数取出来对。"""
        m = re.search(r"DEVICE_TABLE_SIZE\s+(\d+)u", self.arm)
        self.assertIsNotNone(m, "移植侧没有 DEVICE_TABLE_SIZE")

        src = (ROOT / "driver" / "device.cpp").read_text(encoding="utf-8")
        up = re.search(r"device_t\s+device_ctl\[(\d+)\]", src)
        self.assertIsNotNone(up, "上游 device.cpp 里的 device_ctl[] 声明变了")

        self.assertEqual(m.group(1), up.group(1), "DEVICE_TABLE_SIZE 与上游的 device_ctl[] 长度不一致")


HARNESS = r"""
int printf(const char *fmt, ...);

#include <arch/device.h>
#include <arch/heap.h>
#include <arch/id_alloc.h>
#include <arch/kmalloc.h>
#include <arch/errno.h>
#include <krlibc.h>

/* console.c 依赖 MMIO,不上宿主 —— 这里给个桩,顺便数一下注册日志的条数 */
static int log_lines;
void console_printf(const char *fmt, ...);

void console_printf(const char *fmt, ...)
{
    (void)fmt;
    log_lines++;
}

static int failures;
static int checks;

/*
 * ⚠ 这个 harness 必须自己把堆建起来并绑给 libc 那四个名字。
 *
 * 不建的话 `malloc` 返回 NULL(那是刻意的:启动早期还没绑定 ⇒ "分配失败"
 * 而不是崩),于是 `device_manager_init()` 返回 -1、`regist_device()`
 * 返回 -1 —— 而下面那些 `device_ctl[id]` 就会拿 **-1** 去索引,
 * 直接从"判据失败"变成**段错误**。
 * (第一版就是这么炸的:返回码 0xC0000005、一个字都没打出来。)
 */
static u8     heap_area[16384] __attribute__((aligned(8)));
static heap_t h_heap;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(cond)) {                                                                             \
            printf("FAIL %d: %s\n", __LINE__, #cond);                                              \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

/* ------------------------------------------------------------------ */
/* devfs 桩(M4A-1.5 起真 devfs 是上游 driver/fs/vfs/dev.cpp,宿主编不了) */
/* ------------------------------------------------------------------ */

/*
 * 语义照上游 `dev.cpp:341-347`:
 *     device->path = path != NULL ? strdup(path) : NULL;
 *     if (devfs_root == NULL) { return EOK; }     // 没挂载 ⇒ 不建节点
 * 宿主上永远"没挂载",所以只要做 strdup 那一步。
 * ⚠ 上游**不**先释放旧 path(重复注册会泄漏);骨架当年会释放。
 *   这一条不影响 device.c 的判据,但记在这里免得被误读成"我们照抄了"。
 */
errno_t devfs_register(const char *path, size_t id)
{
    device_ctl[id].path = path != NULL ? strdup(path) : NULL;
    return EOK;
}

/* 上游会 `vfs_open("/dev/<name>")` 再 unlink;宿主没有 VFS ⇒ no-op */
errno_t devfs_delete(const char *path)
{
    (void)path;
    return EOK;
}

/* ------------------------------------------------------------------ */
/* 块层的假设备(M4A-3/B5):扇区读写回调 + 一块内存盘                    */
/* ------------------------------------------------------------------ */

static uint8_t *g_fake_disk;     /* 只有这一块盘,回调靠它找到数据 */
static uint32_t g_seen_number;   /* 回调最近一次收到的 (number, lba) —— 流设备要判它 */
static size_t   g_seen_lba;

static size_t blk_read_cb(int drive, uint8_t *buffer, size_t number, size_t lba)
{
    (void)drive;
    if (g_fake_disk == NULL || buffer == NULL) return 0;
    if ((lba + number) * 512u > 4u * 512u) return 0; /* 越界 ⇒ 按"没读到"处理 */
    memcpy(buffer, g_fake_disk + lba * 512u, number * 512u);
    return number;
}

static size_t blk_write_cb(int drive, uint8_t *buffer, size_t number, size_t lba)
{
    (void)drive;
    if (g_fake_disk == NULL || buffer == NULL) return 0;
    if ((lba + number) * 512u > 4u * 512u) return 0;
    memcpy(g_fake_disk + lba * 512u, buffer, number * 512u);
    return number;
}

static size_t stream_read_cb(int drive, uint8_t *buffer, size_t number, size_t lba)
{
    (void)drive;
    (void)buffer;
    g_seen_number = (uint32_t)number;
    g_seen_lba    = lba;
    return number;
}

static size_t stream_write_cb(int drive, uint8_t *buffer, size_t number, size_t lba)
{
    return stream_read_cb(drive, buffer, number, lba);
}

static void make_dev(device_t *dev, const char *name, device_flag_t type, size_t size)
{
    memset(dev, 0, sizeof(*dev));
    dev->flag = 1; /* ⚠ 上游规矩:flag 由**驱动**置,regist_device 不碰它 */
    dev->type = type;
    dev->size = size;
    strncpy(dev->drive_name, name, sizeof(dev->drive_name) - 1u);
}

int main(void)
{
    device_t dev;
    int      id0;
    int      id1;
    int      id2;
    int      id3;

    static const char path_lit[] = "/blk";

    /* ---- 0. 先建堆并绑定 —— 否则下面全是"分配失败" ---- */
    CHECK(heap_init(&h_heap, (uintptr_t)heap_area, sizeof(heap_area)) == HEAP_OK);
    kmalloc_set_heap(&h_heap);

    /* ---- 1. 建表 ---- */
    CHECK(device_manager_init() == 0);

    /* 空槽位与越界都要取不到。★ 越界检查是移植侧加的(上游会读越界内存) */
    CHECK(get_device(5u) == NULL);
    CHECK(get_device(DEVICE_TABLE_SIZE) == NULL);
    CHECK(get_device(DEVICE_TABLE_SIZE + 100u) == NULL);
    CHECK(have_vdisk(5) == false);
    CHECK(disk_size(5) == 0u);

    /* ---- 2. 注册(路径 NULL):表里要有、devfs 里要有 ---- */
    make_dev(&dev, "d0", DEVICE_BLOCK, 4096u);
    id0 = regist_device(NULL, dev);
    CHECK(id0 >= 0);
    CHECK(log_lines == 1);

    {
        device_t *p = get_device((size_t)id0);
        CHECK(p != NULL);
        CHECK(p == &device_ctl[id0]);
        CHECK(p->vdiskid == (size_t)id0);
        CHECK(p->size == 4096u);
        CHECK(p->type == DEVICE_BLOCK);
        CHECK(strcmp(p->drive_name, "d0") == 0);
    }
    CHECK(have_vdisk(id0) == true);
    CHECK(disk_size(id0) == 4096u);
    CHECK(device_ctl[id0].path == NULL);       /* path 传 NULL ⇒ .path 就是 NULL */

    /* ---- 3. 注册(带路径):路径必须是**副本** ---- */
    /*
     * ⚠ 路径用一个**数组**而不是字符串字面量:字面量比较指针时
     *   clang 会报 -Wstring-compare(未指定行为),而 -Werror 下那是错误。
     */
    make_dev(&dev, "d1", DEVICE_STREAM, 512u);
    id1 = regist_device(path_lit, dev);
    CHECK(id1 >= 0);
    CHECK(id1 != id0);
    /* ⚠ 索引前必须判 id >= 0:注册失败时 id1 == -1,
       直接写 device_ctl[id1] 会越界(而不是给出一条 FAIL) */
    if (id1 >= 0) {
        CHECK(device_ctl[id1].path != NULL);
        CHECK(device_ctl[id1].path != path_lit);            /* 不是同一个指针 ⇒ 是副本 */
        CHECK(strcmp(device_ctl[id1].path, path_lit) == 0); /* 但内容相同 */
    }

    /* ---- 4. 空名字:注册仍返回 id(上游语义),但 devfs 登记失败并记一行 ---- */
    make_dev(&dev, "", DEVICE_STREAM, 8u);
    {
        int before = log_lines;
        id3 = regist_device(NULL, dev);
        CHECK(id3 >= 0);                       /* ⚠ 失败仍返回 id —— 与上游一致 */
        CHECK(log_lines == before + 1);        /* 但要**报出来**,不能静默 */
        CHECK(get_device((size_t)id3) != NULL);
    }

    /* ---- 5. 注销:两个方向都消失,别的设备不受影响 ---- */
    delete_device(id1);
    if (id1 >= 0) {
        CHECK(device_ctl[id1].path == NULL);   /* 路径副本被释放并清空 */
    }
    CHECK(get_device((size_t)id0) != NULL);

    /*
     * ★★ 槽位必须真的清掉 —— M4A-1.5 的**有意识决定**(旧注释等的就是这一刻)★★
     *
     * 上游 `delete_device()`(`driver/device.cpp:183-211`)结尾写的是
     *     device_t dev = device_ctl[vdiskid];               // 局部**拷贝**
     *     dev.path = NULL; dev.flag = 0; dev.vdiskid = 0;   // 改的是拷贝
     * ⇒ 意图清清楚楚(槽位标空闲 + 清三个字段),但那三句**一点没落到表上**。
     * 后果是"幽灵设备":删掉之后 `get_device(id)` 仍非 NULL(且 path 已释放)、
     * `have_vdisk(id)` 仍为真。上游没发作,只因为四个调用方删完就不再用。
     *
     * ⇒ 按**写出来的意图**修(与 D16 那条同型),判据就是下面这一行。
     *   (旧版本这里断言的是 `!= NULL`,即照字面抄;那段注释写明"哪天改成
     *    清 flag,要是一个有意识的决定" —— 现在就是。)
     */
    delete_device(id0);
    CHECK(get_device((size_t)id0) == NULL);    /* ← 槽位真的空了 */
    CHECK(have_vdisk(id0) == false);           /* 连带:不再是"有盘" */
    CHECK(device_ctl[id0].path == NULL);       /* 路径副本也清了 */
    CHECK(device_ctl[id0].flag == 0);

    /* 越界注销必须是 no-op,不能写坏内存 */
    delete_device(-1);
    delete_device((int)DEVICE_TABLE_SIZE + 7);
    CHECK(get_device(0u) == NULL);             /* 越界注销不能碰别人的槽位 */

    /* ---- 6. ★ 破坏性对照:关掉 devfs 之后,只有 get_device 还成立 ---- */
    make_dev(&dev, "ab0", DEVICE_STREAM, 16u);
    id2 = regist_device(NULL, dev);
    CHECK(id2 >= 0);
    CHECK(get_device((size_t)id2) != NULL);    /* 表里有 */
    delete_device(id2);

    /* ---- 7. 同名重复注册:覆盖 id,节点数不涨(上游会建出两个同名孩子) ---- */
    make_dev(&dev, "dup", DEVICE_STREAM, 1u);
    (void)regist_device(NULL, dev);
    make_dev(&dev, "dup", DEVICE_STREAM, 2u);
    {
        int second = regist_device(NULL, dev);
        CHECK(second >= 0);
    }

    /* ---- 8. id 分配器的边界(设备表满时会用到这条路径) ---- */
    {
        id_allocator_t *a = id_allocator_create(4u);
        int             x[4];
        int             i;

        CHECK(a != NULL);
        for (i = 0; i < 4; i++) {
            x[i] = id_alloc(a);
            CHECK(x[i] >= 0);
        }
        CHECK(id_alloc(a) == -1);              /* 表满 */
        CHECK(id_free(a, (uint32_t)x[1]) == true);
        CHECK(id_free(a, (uint32_t)x[1]) == false); /* 重复释放必须被拒 */
        CHECK(id_free(a, 999u) == false);           /* 越界必须被拒 */
        CHECK(id_alloc(a) == x[1]);                 /* 释放掉的能被重新分配 */
        CHECK(id_alloc(a) == -1);
    }

    /*
     * ================================================================
     * ★ 块层:按字节偏移读写(M4A-3/B5)★
     * ================================================================
     *
     * 这一节要钉住的是**扇区算术**,而它恰恰是"看起来对、边界上错"的重灾区:
     *   - 非对齐偏移(头的片段)
     *   - 跨扇区(头 + 尾)
     *   - 一次超过 `SECTORS_ONCE` 的传输(分块)
     *   - **非对齐写是读-改-写**:只改那几个字节,周围必须原样
     *     (这一条最容易错成"把整扇区当垃圾写回去")
     *   - 流设备走另一条路(参数原样转交)
     */
    {
        /* 一块 4 扇区的假盘,内容 = 可预测的图案(= 扇区号 * 16 + 扇区内偏移)*/
        static uint8_t disk[4 * 512];
        device_t       blk;
        device_t       stream;
        uint8_t        buf[4096];
        uint8_t        before[8];
        int            id_blk;
        int            id_stream;
        size_t         i;

        for (i = 0; i < sizeof(disk); i++) {
            disk[i] = (uint8_t)((i / 512u) * 16u + (i % 512u));
        }

        memset(&blk, 0, sizeof(blk));
        blk.flag        = 1;
        blk.type        = DEVICE_BLOCK;
        blk.size        = sizeof(disk);
        blk.sector_size = 512u;
        strcpy(blk.drive_name, "blk0");

        id_blk = regist_device(NULL, blk);
        CHECK(id_blk >= 0);
        if (id_blk >= 0) {
            device_ctl[id_blk].read  = blk_read_cb;
            device_ctl[id_blk].write = blk_write_cb;
            /* 回调要拿到盘 —— 用一张只有这一块的静态指针 */
            g_fake_disk = disk;

            /* ① 对齐的整扇区读 */
            memset(buf, 0, sizeof(buf));
            CHECK(blk_device_read(id_blk, buf, 0u, 512u) == 512u);
            CHECK(buf[0] == disk[0]);
            CHECK(buf[511] == disk[511]);

            /* ② 非对齐偏移读(第 100 字节起 100 字节)⇒ 走"头"那一段 */
            memset(buf, 0, sizeof(buf));
            CHECK(blk_device_read(id_blk, buf, 100u, 100u) == 100u);
            CHECK(memcmp(buf, disk + 100, 100) == 0);

            /* ③ 跨扇区读(500 起 100 字节 ⇒ 头 12 + 尾 88)*/
            memset(buf, 0, sizeof(buf));
            CHECK(blk_device_read(id_blk, buf, 500u, 100u) == 100u);
            CHECK(memcmp(buf, disk + 500, 100) == 0);

            /* ④ 一次超过 SECTORS_ONCE 的传输(要分块,内容仍必须完全一致)*/
            memset(buf, 0, sizeof(buf));
            CHECK(blk_device_read(id_blk, buf, 0u, 4u * 512u) == 4u * 512u);
            CHECK(memcmp(buf, disk, sizeof(disk)) == 0);

            /* ⑤ ★ 非对齐写 = 读-改-写:只改那几个字节,周围原样 */
            memcpy(before, disk + 96, sizeof(before));
            memset(buf, 0xAB, sizeof(buf));
            CHECK(blk_device_write(device_ctl[id_blk], buf, 100u, 100u) == 100u);
            CHECK(disk[100] == 0xAB);          /* 改到了 */
            CHECK(disk[199] == 0xAB);
            CHECK(memcmp(disk + 96, before, 4) == 0); /* 前面 4 字节没被动过 */
            CHECK(disk[96] == before[0]);
            CHECK(disk[200] == (uint8_t)((200u / 512u) * 16u + (200u % 512u))); /* 后面也没被动 */

            /* ⑥ 边界与错误:没有这个盘 / 长度 0 ⇒ 0;NULL 缓冲 ⇒ -1 */
            CHECK(blk_device_read(DEVICE_TABLE_SIZE + 3, buf, 0u, 8u) == 0u);
            CHECK(blk_device_read(id_blk, buf, 0u, 0u) == 0u);
            CHECK(blk_device_read(id_blk, NULL, 0u, 8u) == (size_t)-1);
            CHECK(blk_device_write(device_ctl[id_blk], NULL, 0u, 8u) == (size_t)-1);

            /* ⑦ 没有扇区大小 / 没有回调 ⇒ -1(不是 0 —— 0 会被上层当 EOF)*/
            {
                device_t broken;
                int      id_broken;

                memset(&broken, 0, sizeof(broken));
                broken.flag = 1;
                broken.type = DEVICE_BLOCK;
                strcpy(broken.drive_name, "bad0");
                id_broken = regist_device(NULL, broken);
                CHECK(id_broken >= 0);
                if (id_broken >= 0) {
                    CHECK(blk_device_read(id_broken, buf, 0u, 8u) == (size_t)-1);
                }
            }

            delete_device(id_blk);
            g_fake_disk = NULL;
        }

        /* ⑧ 流设备:两个参数**原样**转交给驱动(上游同一语义)*/
        memset(&stream, 0, sizeof(stream));
        stream.flag = 1;
        stream.type = DEVICE_STREAM;
        strcpy(stream.drive_name, "stm0");
        id_stream = regist_device(NULL, stream);
        CHECK(id_stream >= 0);
        if (id_stream >= 0) {
            device_ctl[id_stream].read  = stream_read_cb;
            device_ctl[id_stream].write = stream_write_cb;
            CHECK(blk_device_read(id_stream, buf, 7u, 13u) == 13u);
            CHECK(g_seen_number == 13u && g_seen_lba == 7u); /* (number, lba) = (len, offset) */
            CHECK(blk_device_write(device_ctl[id_stream], buf, 5u, 9u) == 9u);
            CHECK(g_seen_number == 9u && g_seen_lba == 5u);
            delete_device(id_stream);
        }
    }

    printf("checks=%d\n", checks);
    if (failures == 0) {
        printf("all checks passed\n");
    } else {
        printf("%d check(s) failed\n", failures);
    }

    return failures == 0 ? 0 : 1;
}
"""


class DeviceRoundTripTests(unittest.TestCase):
    COMPILER_CANDIDATES = ("gcc", "cc", "clang")
    CAPTURE = {"capture_output": True, "text": True, "encoding": "utf-8", "errors": "replace"}

    def _build(self, tmp: Path, harness: Path) -> Path:
        """编出测试可执行文件:上游的 C++ 一份 + 移植侧的 C 若干。

        ⚠ 两个翻译单元的 **include 路径是分开的** —— 与 ARM 构建里的
        arm32_cc / arm32_cxx 两条规则同构:

          上游 .cpp  : `-I include`(只看得见上游头文件树)
          移植侧 .c  : `-I arch/arm32/include`(看不到上游头文件)

        这不是洁癖:两边的头文件树在同一个 TU 里会打架(上游 stdint.h 的
        `typedef char int8_t` vs 移植侧的 `signed char` —— ARM 上 char 默认
        无符号),而且上游 krlibc.h 里有 `typeof(nullptr)` 这种 C++ 专属语法。
        """
        binary = Path(tmp) / "device_test"
        cxx_obj = Path(tmp) / "id_alloc.o"
        attempts: list[str] = []
        defines = [f"-D{fn}=krtest_{fn}" for fn in KR_PREFIXED + HEAP_PREFIXED]

        for compiler in self.COMPILER_CANDIDATES:
            # ---- 1) 上游的 C++ 翻译单元 ----
            cxx_cmd = [
                compiler, "-x", "c++", "-std=gnu++17", "-fno-exceptions", "-fno-rtti",
                "-Wall", "-Wextra", "-Werror",
                "-I", str(ROOT / "include"),
                *defines,
                "-c", str(ROOT / "kernel" / "id_alloc.cpp"), "-o", str(cxx_obj),
            ]
            try:
                r = subprocess.run(cxx_cmd, **self.CAPTURE)
            except FileNotFoundError:
                attempts.append(f"{compiler}: not found")
                continue
            if r.returncode != 0:
                detail = (r.stderr or "").strip().replace("\n", " ")[:400]
                attempts.append(f"{compiler}: 编译上游 .cpp 失败 rc={r.returncode} {detail}")
                continue

            # ---- 2) 移植侧的 C,与 C++ 目标文件链接 ----
            command = [
                compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-I", str(ROOT / "arch/arm32/include"),
                *defines,
                str(harness),
                str(ROOT / "arch/arm32/src/device.c"),
                str(ROOT / "arch/arm32/src/kmalloc.c"),
                str(ROOT / "arch/arm32/src/heap.c"),
                str(ROOT / "arch/arm32/src/krlibc.c"),
                str(cxx_obj),
                "-o", str(binary),
            ]
            r = subprocess.run(command, **self.CAPTURE)
            if r.returncode == 0:
                return binary
            detail = (r.stderr or "").strip().replace("\n", " ")[:400]
            attempts.append(f"{compiler}: 链接失败 rc={r.returncode} {detail}")

        self.fail("no working host C/C++ compiler found:\n  " + "\n  ".join(attempts))

    def test_round_trip(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            harness = Path(tmp) / "device_test.c"
            harness.write_text(HARNESS, encoding="utf-8")

            binary = self._build(Path(tmp), harness)
            run = subprocess.run([str(binary)], **self.CAPTURE)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("all checks passed", run.stdout)

            executed = [line for line in run.stdout.splitlines() if line.startswith("checks=")]
            self.assertEqual(len(executed), 1, f"harness 没报判据数:\n{run.stdout}")
            count = int(executed[0].split("=", 1)[1])
            self.assertGreaterEqual(count, 40, f"只执行了 {count} 条判据,疑似用例被削弱")


if __name__ == "__main__":
    unittest.main()
