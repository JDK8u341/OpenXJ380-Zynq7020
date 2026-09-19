"""FATFS 落地层的契约测试(M4A-1.4)

钉住三件事,每一件都是"源码上看不出来、只有工具能看出来"的那类:

============================================================================
一、这 8 个符号必须是 **C++ 链接**(与 VFS 那组**相反**)
============================================================================

`driver/fs/fatfs/` 编进 ARM 后,链接缺口正好 8 个,而实测目标文件里的名字是
**修饰名**:

    $ arm-none-eabi-nm out/arm32/upstream/driver/fs/fatfs/fatfs.o | grep ' U '
    U _Z12mutex_createP5mutexb      U _Z6mktimeP2tm
    U _Z10mutex_lockP5mutex         U _Z11realtime_nsv
    U _Z12mutex_unlockP5mutex       U _Z24ahci_is_qemu_environmentv
    $ ... diskio.o
    U _Z12alloc_framesj             U _Z12phys_to_virt

⇒ 上游这些声明都**不在** `extern "C"` 块里 ⇒ 移植侧的定义**不能**写
`extern "C"`(写了就是坑表 55 那个错误的重演:C 侧给出未修饰符号,
而调用点要的是修饰过的,源码上两个名字一模一样)。

⚠ 注意这和 `tests/test_arm32_vfs.py` 的规则**正好相反**:
   那边要求 `arm_vfs_*` 必须是 `extern "C"`(因为移植侧 C 要调它们),
   这边要求这 8 个**必须不是**(因为上游 C++ 要调它们)。
   两组规则都由"符号由谁调"决定,不是风格偏好。

============================================================================
二、`mktime()` 的语义必须与上游**逐值一致**(而且它不是 ISO C 的语义)
============================================================================

上游 `driver/rtc.cpp:100-127` 的 `mktime` 把
    `tm_year` 当**完整年份**(不是"1900 起的年数")、
    `tm_mon`  当 **1..12**(不是 ISO 的 0..11);
`fatfs.cpp:293-312` 正是按这两点构造 `tm` 的。
移植侧照搬了它 —— 这里把移植侧那份**编到宿主上跑**,与宿主机自己的
`datetime` 逐日期比对:**正常日期必须相等**,并且把两处非 ISO 语义
**显式钉住**(它们是上游契约的一部分,不是 bug 可以顺手"修"的)。
"""

from __future__ import annotations

import datetime as dt
import re
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LANDING = ROOT / "arch/arm32/src/upstream_api.cpp"
PORT_MUTEX_H = ROOT / "arch/arm32/include/arch/mutex.h"

# ⚠ 全部**不得**带 extern "C"(C++ 链接的符号)
CPP_LINKAGE_SYMBOLS = {
    "mutex_create": "void mutex_create(mutex_t *mtx, bool recursive)",
    "mutex_lock": "int mutex_lock(mutex_t *mtx)",
    "mutex_unlock": "int mutex_unlock(mutex_t *mtx)",
    "mktime": "int64_t mktime(tm *time)",
    "realtime_ns": "uint64_t realtime_ns()",
    "ahci_is_qemu_environment": "bool ahci_is_qemu_environment()",
    "alloc_frames": "uint64_t alloc_frames(size_t count)",
    "phys_to_virt": "void *phys_to_virt(uint64_t phys_addr)",
}


def _strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def _extract_function(text: str, signature_regex: str) -> str:
    """从源码里取出一个函数的**完整定义文本**(含函数体,按花括号配对)。"""
    match = re.search(signature_regex, text)
    if match is None:
        raise AssertionError(f"找不到函数定义: {signature_regex}")
    start = match.start()
    brace = text.index("{", match.end() - 1)
    depth = 0
    for index in range(brace, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[start:index + 1]
    raise AssertionError("函数体没有配对的花括号")


class FatfsSymbolShapeTests(unittest.TestCase):
    """① 8 个符号的链接形状。"""

    def setUp(self) -> None:
        self.landing = _strip_comments(LANDING.read_text(encoding="utf-8"))

    def test_all_eight_are_defined_in_the_landing_layer(self) -> None:
        for name in CPP_LINKAGE_SYMBOLS:
            with self.subTest(symbol=name):
                self.assertIn(name, self.landing, f"落地层里没有 {name}")

    def test_none_of_them_is_extern_c(self) -> None:
        """
        ★ 这条是这一组的核心 ★

        上游要的是**修饰名**;`extern "C"` 会给出未修饰符号。
        判据:每个定义那一行**不带** `extern "C"`。
        """
        for name, signature in CPP_LINKAGE_SYMBOLS.items():
            with self.subTest(symbol=name):
                pattern = re.compile(r"^(extern\s+\"C\"\s+)?" + re.escape(signature) + r"\s*\n?\{",
                                     re.M)
                match = pattern.search(self.landing)
                self.assertIsNotNone(match, f"落地层里找不到 {name} 的这一定义形式:{signature}")
                self.assertIsNone(match.group(1),
                                  f"{name} 被写成了 extern \"C\" —— 上游要的是修饰名 "
                                  f"(_Z…),两者不是同一个符号")

    def test_upstream_target_files_still_ask_for_mangled_names(self) -> None:
        """
        反向确认:上游那两个目标文件里的引用**现在是**修饰名。
        (目标文件由构建产生;没构建过就跳过 —— 但绝不"猜一个结论"。)
        """
        nm = ROOT / "tmp-test"
        objects = [
            ROOT / "out/arm32/upstream/driver/fs/fatfs/fatfs.o",
            ROOT / "out/arm32/upstream/driver/fs/fatfs/diskio.o",
        ]
        if not all(obj.exists() for obj in objects):
            self.skipTest("上游 FATFS 目标文件还没构建(out/arm32/...,先跑一次 ninja arm32)")
        nm_tool = self._find_nm()
        if nm_tool is None:
            self.skipTest("找不到 arm-none-eabi-nm")

        undefined: set[str] = set()
        for obj in objects:
            out = subprocess.run([str(nm_tool), "-u", str(obj)], capture_output=True, text=True)
            undefined.update(line.split()[-1] for line in out.stdout.splitlines() if line.strip())

        for expected in ("_Z12mutex_createP5mutexb", "_Z6mktimeP2tm", "_Z11realtime_nsv",
                         "_Z24ahci_is_qemu_environmentv", "_Z12alloc_framesj",
                         "_Z12phys_to_virty", "_Z10mutex_lockP5mutex", "_Z12mutex_unlockP5mutex"):
            with self.subTest(symbol=expected):
                self.assertIn(expected, undefined,
                              f"上游目标文件里没有引用 {expected} —— "
                              f"上游的声明方式变了,落地层的符号形状要跟着改")
        _ = nm

    @staticmethod
    def _find_nm() -> Path | None:
        candidates = list(Path("C:/AMDDesignTools").glob("*/gnu/aarch32/nt/gcc-arm-none-eabi/bin/arm-none-eabi-nm.exe"))
        return candidates[0] if candidates else None

    def test_arm_mutex_wrappers_are_the_only_bridge(self) -> None:
        """
        移植侧那把 mutex 是**换名字**转发过去的(`arm_mutex_*`),
        因为两边同名会与上游的 C++ 声明冲突。
        """
        landing = self.landing
        for name in ("arm_mutex_create", "arm_mutex_lock", "arm_mutex_unlock"):
            with self.subTest(symbol=name):
                self.assertIn(f'extern "C"', landing)
                self.assertRegex(landing, re.escape(name))
        header = PORT_MUTEX_H.read_text(encoding="utf-8")
        self.assertIn("arm_mutex_create", header)
        self.assertIn("arm_mutex_unlock", header)

    def test_static_assert_pins_the_16_byte_storage_assumption(self) -> None:
        """
        落地层把上游 `mutex_t` 对象当前 16 字节存储用 —— 这个前提有两半,
        各由一条断言钉住(改任何一半都应该让某处失败)。
        """
        source = (ROOT / "arch/arm32/src/mutex.c").read_text(encoding="utf-8")
        self.assertIn("sizeof(mutex_t) == 16u", source,
                      "移植侧少了'只占 16 字节'的静态断言")
        self.assertIn("sizeof(mutex_t) >= 16u", self.landing,
                      "落地层少了'上游对象够大'的静态断言")

    def test_declaration_headers_are_the_source_of_truth(self) -> None:
        """上游的声明来自这三个头 —— 落地层的定义形式必须与之一致。"""
        proto = (ROOT / "include/proto.hpp").read_text(encoding="utf-8")
        mutex_h = (ROOT / "include/mutex.h").read_text(encoding="utf-8")
        rtc_h = (ROOT / "include/rtc.h").read_text(encoding="utf-8")

        self.assertRegex(proto, r"uint64_t alloc_frames\(size_t count\)")
        self.assertRegex(proto, r"void\s+\*phys_to_virt\(uint64_t phys_addr\)")
        self.assertRegex(mutex_h, r"void mutex_create\(mutex_t\* mtx,bool recursive\)")
        self.assertRegex(mutex_h, r"int mutex_lock\(mutex_t \*mutex\)")
        self.assertRegex(mutex_h, r"int mutex_unlock\(mutex_t \*mutex\)")
        self.assertRegex(rtc_h, r"int64_t mktime\(tm \*time\)")
        self.assertRegex(rtc_h, r"uint64_t realtime_ns\(\)")
        # 上游把这两条声明放在 extern "C" **之外** —— 这正是我们要 C++ 链接的理由
        self.assertNotIn('extern "C"', _strip_comments(rtc_h))


class MktimeFaithfulnessTests(unittest.TestCase):
    """② `mktime()` 与上游逐值一致(在宿主上真跑一遍)。"""

    HARNESS = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year;
    int tm_wday, tm_yday, tm_isdst;
} tm;

/*__CODE__*/

int main(void)
{
    /* year, mon(1..12), day, hour, min, sec —— 全部是"人类写法" */
    static const int dates[][6] = {
        {1970, 1, 1, 0, 0, 0},      {1970, 1, 1, 0, 0, 1},
        {1970, 1, 2, 0, 0, 0},      {1970, 2, 1, 0, 0, 0},
        {1971, 1, 1, 0, 0, 0},      {1972, 2, 29, 0, 0, 0},
        {2000, 2, 29, 23, 59, 59},  {2000, 3, 1, 0, 0, 0},
        {2024, 12, 31, 23, 59, 59},{2026, 9, 19, 12, 34, 56},
        {2038, 1, 19, 3, 14, 7},    {2100, 3, 1, 0, 0, 0},
    };
    unsigned i;

    for (i = 0; i < sizeof(dates) / sizeof(dates[0]); i++) {
        tm t = {0};
        t.tm_sec = dates[i][5]; t.tm_min = dates[i][4]; t.tm_hour = dates[i][3];
        t.tm_mday = dates[i][2]; t.tm_mon = dates[i][1]; t.tm_year = dates[i][0];
        printf("%d %d %d %d %d %d %lld\n", dates[i][0], dates[i][1], dates[i][2],
               dates[i][3], dates[i][4], dates[i][5], (long long)mktime(&t));
    }
    return 0;
}
"""

    def _build(self, tmp: Path) -> Path:
        source = _strip_comments(LANDING.read_text(encoding="utf-8"))
        code = "\n\n".join([
            _extract_function(source, r"static bool arm_is_leap_year\(int year\)"),
            _extract_function(source, r"int64_t mktime\(tm \*time\)"),
        ])
        # 月份天数表是文件里的一个 static 数组,单独取出来
        table = re.search(r"static const int g_days_in_month\[2\]\[12\] = \{[^;]*\};", source, re.S)
        self.assertIsNotNone(table, "落地层里找不到 g_days_in_month")
        harness = tmp / "mktime_test.c"
        harness.write_text(self.HARNESS.replace("/*__CODE__*/", table.group(0) + "\n" + code), encoding="utf-8")
        binary = tmp / "mktime_test"
        for compiler in ("clang", "gcc", "cc"):
            result = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                                     str(harness), "-o", str(binary)],
                                    capture_output=True, text=True)
            if result.returncode == 0:
                return binary
        self.fail(f"宿主编译失败: {result.stderr[:300]}")

    def test_matches_host_datetime(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            binary = self._build(Path(tmp))
            out = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(out.returncode, 0, out.stderr)

            rows = [line.split() for line in out.stdout.strip().splitlines()]
            self.assertEqual(len(rows), 12)

            for row in rows:
                year, mon, day, hour, minute, sec = (int(v) for v in row[:6])
                got = int(row[6])
                with self.subTest(date=f"{year}-{mon:02d}-{day:02d} {hour:02d}:{minute:02d}:{sec:02d}"):
                    want = int((dt.datetime(year, mon, day, hour, minute, sec)
                                - dt.datetime(1970, 1, 1)).total_seconds())
                    self.assertEqual(got, want,
                                     "移植侧的 mktime 与宿主机 datetime 不一致 —— "
                                     "上游的语义(tm_year=完整年份, tm_mon=1..12)被改动了?")

    def test_non_iso_semantics_are_pinned(self) -> None:
        """
        ★ 上游这两处**非 ISO** 语义是契约的一部分,不是可以顺手"修正"的 bug ★

        只要 FatFs 那两个调用点按同一套构造 `tm`,文件时间就是对的;
        而一旦有人把 `mktime` 改成 ISO 语义(tm_year 从 1900 起、tm_mon 0..11),
        **不会报错**,文件时间会整体偏掉几十年。所以这里把"偏离方向"也钉住。
        """
        with tempfile.TemporaryDirectory() as tmp:
            binary = self._build(Path(tmp))

            def run(year: int, mon: int, day: int = 1) -> int:
                harness = Path(tmp) / "probe.c"
                base = self.HARNESS.split("/*__CODE__*/")[0]
                code = "\n\n".join([
                    _extract_function(_strip_comments(LANDING.read_text(encoding="utf-8")),
                                      r"static bool arm_is_leap_year\(int year\)"),
                    _extract_function(_strip_comments(LANDING.read_text(encoding="utf-8")),
                                      r"int64_t mktime\(tm \*time\)"),
                ])
                table = re.search(r"static const int g_days_in_month\[2\]\[12\] = \{[^;]*\};",
                                  _strip_comments(LANDING.read_text(encoding="utf-8")), re.S).group(0)
                harness.write_text(base + table + "\n" + code + textwrap.dedent(f"""
                    int main(void) {{
                        tm t = {{0}};
                        t.tm_year = {year}; t.tm_mon = {mon}; t.tm_mday = {day};
                        printf("%lld\\n", (long long)mktime(&t));
                        return 0;
                    }}
                    """), encoding="utf-8")
                out_bin = Path(tmp) / "probe"
                subprocess.run(["clang", "-std=c11", "-w", str(harness), "-o", str(out_bin)],
                               capture_output=True, text=True, check=True)
                result = subprocess.run([str(out_bin)], capture_output=True, text=True, check=True)
                return int(result.stdout.strip())

            # tm_year 是**完整年份**:1970 年 1 月 1 日 = 0 秒
            self.assertEqual(run(1970, 1), 0)
            # tm_mon 是 **1..12**:1 月 = 0 秒,2 月 = 31 天
            self.assertEqual(run(1970, 2), 31 * 86400)
            # 若被改成 ISO 语义(0 基月份),上面那条会变成 0 —— 这条断言就是那道护栏
            self.assertNotEqual(run(1970, 1), run(1970, 2))
            _ = binary


if __name__ == "__main__":
    unittest.main()
