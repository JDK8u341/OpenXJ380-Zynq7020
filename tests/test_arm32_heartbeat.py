"""心跳区槽位与故障注入选择器地址的一致性测试。

**被测对象不是一个模块,而是一组跨文件的常量。** 它们分布在:

    arch/arm32/include/arch/platform.h     <- 唯一定义处(PLAT_HEARTBEAT_*)
    arch/arm32/include/arch/heartbeat.h    <- 槽号(唯一定义处)
    tmp-test/jtag/run_kernel_uart.tcl      \
    tmp-test/jtag/inject_fault.tcl          | 硬编码的副本
    tmp-test/guard_trip.py                  |
    arch/arm32/include/arch/fault_test.h    |
    arch/arm32/README.md                   /

为什么值得单独一个测试
----------------------

故障注入选择器**紧跟在心跳区预留容量之后**,而心跳已经扩过三次槽位
(16 → 32 → 64;最后一次是 2026-09-18 的合流决策 D5)。

扩容时如果只改 `PLAT_HEARTBEAT_REGION_SLOTS` 而忘了同步地址,
**C 侧会被挡住**:`heartbeat.h` 末尾那两条 `_Static_assert` 双向锁住
"心跳区不许长到选择器头上"。那是编译期的事,做得很好。

**但脚本里的副本没有任何东西拦。** 而它的失效形态特别难查:

    mwr -force 0x00020080 1     # 扩容之后,这个地址已经落在心跳槽 32 上了

于是:写成功、不报错、内核毫无反应、心跳区被悄悄改了一个字。
排查方向会先往"注入的码对不对""内核是不是没跑到主循环"上去 ——
而真正的原因只是"这个数字该改了"。

所以这个文件就是那两条 `_Static_assert` 在**非 C 文件**上的延伸。

判据里为什么带"下限"
--------------------

槽地址(0x00020000 + 4k)本身是**允许**出现在这些文件里的 ——
它们就是"读第 k 槽"的意思,而且 `HB_SLOT_COUNT` 以内的槽号没变过。

真正不许出错的是**选择器那一个地址**。所以规则写成:

    任何 mwr/rd/mrd 的目标地址 v,只要 v >= BASE + HB_SLOT_COUNT*4,
    就必须恰好等于 PLAT_FAULT_SEL_ADDR

下界取 `HB_SLOT_COUNT*4`(而不是"预留容量"):扩容前写下的 0x00020080
正好等于它,于是**旧的副本会被这条规则抓住**,而保留槽(32..63)的读
仍然合法。

刻意不做的事
------------

`docs/ZYNQ7020_*.md` 不在检查范围内:那两份文档会把旧地址当作
**变更历史**引用("32 → 64 槽""0x00020080 → 0x00020100"),把它们
一律当成错误会逼着人删掉正确的记录。规则只对**描述当前布局**的文件生效。

参考:
  arch/arm32/include/arch/heartbeat.h(两条静态断言)
  docs/ZYNQ7020_PORT_PLAN.md §0.5.8(合流决策 D5 的取证)
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

PLATFORM_H = ROOT / "arch" / "arm32" / "include" / "arch" / "platform.h"
HEARTBEAT_H = ROOT / "arch" / "arm32" / "include" / "arch" / "heartbeat.h"
README = ROOT / "arch" / "arm32" / "README.md"

# 硬编码了选择器地址、或直接对选择器下手的文件
COPY_FILES = (
    "tmp-test/jtag/run_kernel_uart.tcl",
    "tmp-test/jtag/inject_fault.tcl",
    "tmp-test/guard_trip.py",
    "arch/arm32/include/arch/fault_test.h",
)

# 一次 JTAG 读写:助记符 + 可选 -force + 地址
_ACCESS_RE = re.compile(
    r"\b(?:mwr|mrd|rd|rr)\b(?:\s+-force)?\s+(0x[0-9A-Fa-f]+)"
)

# 行尾注释要容忍:#define HB_SLOT_MAGIC 0u /* ... */ 也是合法写法
_DEFINE_RE = re.compile(r"^#define\s+(\w+)\s+([0-9][0-9A-Fa-fxXuUlL]*)", re.MULTILINE)

# 槽号定义:#define HB_SLOT_XXX <n>u
_SLOT_RE = re.compile(r"^#define\s+HB_SLOT_(\w+)\s+(\d+)u", re.MULTILINE)

# 不是"槽号"、只是借了 HB_SLOT_ 前缀的两个量
_SLOT_NAME_EXCLUDE = {"COUNT", "CAPACITY"}

# 判定"这个地址该不该是选择器"的窗口大小。
#
# ⚠ 必须限定窗口。不加的话,`rd 0xE0001004`(UART)、`mwr 0xF8000104`(SLCR)
#   这些**完全无关的 MMIO 访问**都会落进判据里 —— 这是这个检查器第一版
#   真实踩到的错(5 个假阳性,全在 run_kernel_uart.tcl 的 UART/SLCR 读写上)。
#   心跳与选择器都挤在 0x0002_0000 起的这一小段里,窗口取 64KB 足够。
_NEIGHBOURHOOD = 0x10000


def _parse_defines(path: Path) -> dict[str, int]:
    """把 `#define NAME <整数>` 摘出来。带 u/U 后缀的值按 0 进制解析。"""
    out: dict[str, int] = {}
    for name, raw in _DEFINE_RE.findall(path.read_text(encoding="utf-8")):
        cleaned = raw.rstrip("uUlL")
        try:
            out[name] = int(cleaned, 0)
        except ValueError:  # 0x 之类的畸形写法:交给别的用例去报
            continue
    return out


def _access_targets(text: str) -> list[tuple[int, int]]:
    """返回 [(行号, 地址)] —— 所有 mwr/rd/mrd 的目标地址。"""
    hits = []
    for m in _ACCESS_RE.finditer(text):
        line = text[: m.start()].count("\n") + 1
        hits.append((line, int(m.group(1), 16)))
    return hits


def _stale_selector_sites(
    text: str, base: int, slot_floor: int, selector: int
) -> list[tuple[int, int]]:
    """找出"地址落在心跳那一段、却不是心跳槽也不是选择器"的读写点。

    只在这个 64KB 窗口里判:窗口外的 MMIO(UART 0xE0...,SLCR 0xF8...)
    与这件事无关,放进来只会制造假阳性。

    slot_floor 以下的地址是心跳槽,合法;窗口内 >= slot_floor 的必须是选择器本身。
    """
    bad = []
    for line, addr in _access_targets(text):
        if not (base <= addr < base + _NEIGHBOURHOOD):
            continue
        if addr >= slot_floor and addr != selector:
            bad.append((line, addr))
    return bad


class HeartbeatLayoutTests(unittest.TestCase):
    """platform.h 与 heartbeat.h 里那组常量的自洽性。"""

    def setUp(self) -> None:
        self.plat = _parse_defines(PLATFORM_H)
        self.hb = _parse_defines(HEARTBEAT_H)

        for name in ("PLAT_HEARTBEAT_BASE", "PLAT_HEARTBEAT_REGION_SLOTS", "PLAT_FAULT_SEL_ADDR"):
            self.assertIn(name, self.plat, f"{PLATFORM_H.name} 里找不到 {name}")
        for name in ("HB_SLOT_COUNT",):
            self.assertIn(name, self.hb, f"{HEARTBEAT_H.name} 里找不到 {name}")

        self.base = self.plat["PLAT_HEARTBEAT_BASE"]
        self.slots = self.plat["PLAT_HEARTBEAT_REGION_SLOTS"]
        self.selector = self.plat["PLAT_FAULT_SEL_ADDR"]
        self.slot_count = self.hb["HB_SLOT_COUNT"]

    def test_selector_sits_right_after_the_reserved_region(self) -> None:
        """选择器必须紧跟在预留容量之后 —— 这正是那两条静态断言在管的事。

        在这里复述一遍不是重复:静态断言只在**编 ARM 的内核**时求值,
        而这个测试连不编内核也能跑,并且顺带把 D5 的具体取值钉住。
        """
        self.assertEqual(
            self.selector,
            self.base + self.slots * 4,
            "PLAT_FAULT_SEL_ADDR 必须 = PLAT_HEARTBEAT_BASE + PLAT_HEARTBEAT_REGION_SLOTS*4",
        )

    def test_reserved_region_is_inside_the_non_cacheable_low_1mb(self) -> None:
        """心跳必须落在低 1MB。挪出去它就不再是 JTAG 能读的通道了。"""
        self.assertGreaterEqual(self.base, 0)
        self.assertLessEqual(
            self.base + self.slots * 4,
            0x00100000,
            "心跳区不能越出低 1MB(那里是页表里唯一的不可缓存段)",
        )

    def test_slot_numbers_are_dense_and_within_capacity(self) -> None:
        """槽号必须是 0..HB_SLOT_COUNT-1 且没有空洞。

        槽号一旦被外部引用就是契约(JTAG 脚本、自检报告、文档都按号读),
        所以"中间挖掉一个"或"两个名字撞同一个号"都必须当场失败。
        """
        named = [
            (name, int(num))
            for name, num in _SLOT_RE.findall(HEARTBEAT_H.read_text(encoding="utf-8"))
            if name not in _SLOT_NAME_EXCLUDE
        ]
        self.assertTrue(named, "一个槽号都没解析到 —— 正则与头文件格式脱节了")

        numbers = sorted(n for _, n in named)
        self.assertEqual(
            numbers,
            list(range(len(numbers))),
            f"槽号不是 0..{len(numbers) - 1} 的无空洞序列: {numbers}",
        )
        self.assertEqual(
            len(numbers),
            self.slot_count,
            f"HB_SLOT_COUNT 声明为 {self.slot_count},实际定义了 {len(numbers)} 个槽",
        )
        self.assertLessEqual(
            self.slot_count,
            self.slots,
            "已分配的槽数超过了预留容量 —— C 侧会有静态断言,这里也一样",
        )

    def test_d5_decision_is_reflected(self) -> None:
        """D5(2026-09-18)拍的是"扩到 64 槽",这条把它钉住。

        写成显式断言而不是 `>= 32`:扩容是有意的决定,不是随便调大的数 ——
        有人把它改回去时,应当看到一条写着"为什么是 64"的失败,
        而不是一条沉默的通过。
        """
        self.assertEqual(self.slots, 64, "合流决策 D5:心跳预留容量为 64 槽")
        self.assertEqual(self.selector, 0x00020100, "合流决策 D5:选择器在 0x00020100")


class SelectorCopyTests(unittest.TestCase):
    """脚本与注释里那些硬编码副本 —— 它们没有任何编译期保护。"""

    def setUp(self) -> None:
        plat = _parse_defines(PLATFORM_H)
        self.base = plat["PLAT_HEARTBEAT_BASE"]
        self.selector = plat["PLAT_FAULT_SEL_ADDR"]
        # 下界取**已分配槽数**而不是预留容量:旧的 0x00020080 正好等于它,
        # 于是"扩容后漏改"会被抓住,而保留槽的读仍然合法。
        self.slot_floor = self.base + _parse_defines(HEARTBEAT_H)["HB_SLOT_COUNT"] * 4

    def test_no_copy_still_points_at_the_old_selector(self) -> None:
        problems = []
        for rel in COPY_FILES:
            path = ROOT / rel
            self.assertTrue(path.exists(), f"{rel} 不见了 —— 副本清单该更新了")
            for line, addr in _stale_selector_sites(
                path.read_text(encoding="utf-8"), self.base, self.slot_floor, self.selector
            ):
                problems.append(f"{rel}:{line} 读写 0x{addr:08X},而选择器在 0x{self.selector:08X}")

        self.assertEqual(
            [],
            problems,
            "有副本还指着旧的/错误的选择器地址。这个写错了不会报错,"
            "只会写进心跳槽里 —— 症状是'注入脚本没反应':\n  " + "\n  ".join(problems),
        )

    def test_every_copy_file_actually_mentions_the_selector(self) -> None:
        """反向:副本文件里必须真的出现当前地址。

        没有这一条的话,把整段注入代码删掉也能让上一条通过。
        只查"文本里有没有这个数",不查用法 —— 因为 guard_trip.py 是**拼**出
        注入命令的(`0x{FAULT_SEL_ADDR:08X}`),那一处不会以字面量出现在
        mwr 后面,但它同样是这份契约的一部分。
        """
        needle = f"0x{self.selector:08X}".upper()
        for rel in COPY_FILES:
            text = (ROOT / rel).read_text(encoding="utf-8").upper()
            self.assertIn(
                needle,
                text,
                f"{rel} 里找不到 0x{self.selector:08X} —— "
                "是注入路径被删了,还是地址又变了?",
            )

    def test_readme_describes_the_current_layout(self) -> None:
        """`arch/arm32/README.md` 描述的是**当前**布局,不是变更历史。"""
        text = README.read_text(encoding="utf-8").upper()
        self.assertIn(
            f"0x{self.selector:08X}".upper(),
            text,
            f"README 没有提到当前的选择器地址 0x{self.selector:08X}",
        )
        region_end = self.base + _parse_defines(PLATFORM_H)["PLAT_HEARTBEAT_REGION_SLOTS"] * 4 - 1
        self.assertIn(
            f"0x{region_end:08X}".upper(),
            text,
            f"README 没有提到当前心跳区的末地址 0x{region_end:08X}",
        )

    def test_the_checker_has_discrimination(self) -> None:
        """★ 对照组:把地址换回扩容前那个,检查器**必须**报出来。

        没有这条,一个恒返回空的检查器也能让上面全绿 —— 这个项目已经
        被"判据成立但这一相什么都没发生"咬过一次(坑 43)。
        """
        text = (ROOT / COPY_FILES[0]).read_text(encoding="utf-8")
        stale = text.replace(f"0x{self.selector:08X}", "0x00020080")

        self.assertNotEqual(stale, text, "对照组没能改动文本 —— 这条用例本身就失效了")
        self.assertNotEqual(
            [],
            _stale_selector_sites(stale, self.base, self.slot_floor, self.selector),
            "把选择器换回 0x00020080 之后检查器竟然没报 —— 它没有区分能力",
        )
        # 同一份对照文本,把地址改对就应当干净
        self.assertEqual(
            [],
            _stale_selector_sites(text, self.base, self.slot_floor, self.selector),
            "未改动的原文本身就没通过检查器",
        )


if __name__ == "__main__":
    unittest.main()
