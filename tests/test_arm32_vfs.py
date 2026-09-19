"""VFS 落地层的契约测试(M4A-1.2b)

被测的是**那条交界线**,不是 VFS 本身:

    arch/arm32/src/vfs_check.c        (移植侧 C:判据)
        ↓ 手写 extern 声明
    arch/arm32/src/upstream_api.cpp   (C++ 落地层:extern "C" 转发)
        ↓ 调用
    include/fs/vfs/vfs.h + tmpfs.cpp  (上游:C++ 链接的真实实现)

====================================================================
为什么这条线必须被钉住
====================================================================

`include/fs/vfs/vfs.h` **整个文件没有 `extern "C"`** —— VFS 的公开函数
全是 C++ 链接的(实测 `nm out/kernel-arm.elf` 里是 `_Z8vfs_initv` /
`_Z9vfs_mountPKcP8vfs_node`)。于是移植侧 C **不可能**直接调它们。

这个坑 M4A-1.2b 收尾时刚在 `sprintf` 上踩过一次(坑表 55):
**源码上名字一模一样,链接器看到的却是两个符号** —— 只有 `nm` 能看出来。
所以这里的每一条断言都对着"符号会不会真的对上":

  1. 移植侧 C 里每一条 `extern` 声明,落地层里必须有**同形的 `extern "C"` 定义**
     (少了 `extern "C"` 就成了另一个符号,C 这边链接期才会炸);
  2. 落地层调用的每一个上游函数,必须在**上游头文件里有声明**
     (凭空写一个名字会编译过、链接才炸);
  3. 上游 VFS 真的**没有** `extern "C"` —— 这条是整组断言的前提,
     哪天上游加上了,这里必须失败,因为那时"必须走落地层"的理由就变了;
  4. `TMPFS_REGISTER_ID` 的数值与 `tmpfs_mount()` 里那个
     `if ((uint64_t)src != TMPFS_REGISTER_ID)` 必须一致
     (它是"挂载判据"的根:`tmpfs_setup()` 靠它把挂载点认出来)。
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

CHECK_C = ROOT / "arch/arm32/src/vfs_check.c"
LANDING = ROOT / "arch/arm32/src/upstream_api.cpp"
UPSTREAM_VFS_H = ROOT / "include/fs/vfs/vfs.h"
UPSTREAM_TMPFS = ROOT / "driver/fs/vfs/tmpfs.cpp"

# 移植侧 C 要用的转发入口,按"**在哪个文件里声明**"分组。
# 每一条都不能少:少了就是链接期才炸,所以在这里钉住。
WRAPPER_DECLARATIONS = {
    "arch/arm32/src/vfs_check.c": (
        "arm_vfs_bringup",
        "arm_vfs_create",
        "arm_vfs_write",
        "arm_vfs_read",
        "arm_vfs_child_count",
        "arm_vfs_fsid",
        "arm_vfs_cwd_is_root",
    ),
    # M4A-1.5:管道
    "arch/arm32/src/pipe_check.c": (
        "arm_pipefs_setup",
        "arm_pipe_create",
        "arm_pipe_write",
        "arm_pipe_read",
        "arm_pipe_fill",
    ),
    # M4A-1.5:devfs(上游那份)+ 设备自检
    "arch/arm32/src/kmain.c": (
        "arm_devfs_setup",
        "arm_devfs_node_exists",
    ),
}
EXPECTED_WRAPPERS = tuple(name for names in WRAPPER_DECLARATIONS.values() for name in names)


def _strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def _param_types(params: str) -> tuple[str, ...]:
    """形参表规范化成"只有类型"的元组(数组退化成指针,去掉参数名)。"""
    result: list[str] = []
    for raw in params.split(","):
        piece = " ".join(raw.split())
        if not piece:
            continue
        if piece == "...":
            result.append("...")
            continue
        piece = re.sub(r"\[[^\]]*\]", " *", piece)
        piece = piece.replace("*", " * ")
        words = piece.split()
        if len(words) > 1:
            words = words[:-1]
        result.append("".join(words))
    return tuple(result)


def _exact_decl(text: str, name: str) -> tuple[str, tuple[str, ...]] | None:
    """精确匹配 `[extern] [extern "C"] <ret> name(params);` 形式的声明。

    ⚠ 返回类型里要**去掉前导的 `extern`**:移植侧 C 有时写成
    `extern int arm_pipe_write(...)`(习惯写法),而落地层那边是 `int …`。
    两者是同一个声明,比较时必须先归一(第一版没做,于是报了假 FAIL)。
    """
    pattern = re.compile(
        r'(?:extern\s*"C"\s*)?((?:extern\s+)?[A-Za-z_][A-Za-z_0-9 \t\*]*?)\b'
        + re.escape(name)
        + r"[ \t]*\(([^;{)]*)\)[ \t]*;",
    )
    match = pattern.search(text)
    if match is None:
        return None
    return_type = re.sub(r"^extern\s+", "", " ".join(match.group(1).split()))
    return return_type, _param_types(match.group(2))


def _definitions(text: str, name: str) -> list[tuple[int, str, tuple[str, ...], bool]]:
    """取 `[extern "C"] <ret> name(params) {` 形式的定义。

    返回 (位置, 返回类型, 形参类型元组, 是否带 `extern "C"`)。
    ⚠ 第四个字段必须由**正则的分组**给出,不能靠"往前面看 40 个字符里有没有
    `extern "C"`" —— 那样写第一次就假失败了(实测:前缀被这条正则自己吃掉了)。
    """
    pattern = re.compile(
        r'(extern\s*"C"\s*)?([A-Za-z_][A-Za-z_0-9 \t\*]*?)\b'
        + re.escape(name)
        + r"[ \t]*\(([^;{)]*)\)[ \t]*\n?\{",
    )
    out: list[tuple[int, str, tuple[str, ...], bool]] = []
    for match in pattern.finditer(text):
        out.append((match.start(),
                    " ".join(match.group(2).split()),
                    _param_types(match.group(3)),
                    match.group(1) is not None))
    return out


class Arm32VfsBridgeContract(unittest.TestCase):
    def setUp(self) -> None:
        self.check_c = _strip_comments(CHECK_C.read_text(encoding="utf-8"))
        self.landing = _strip_comments(LANDING.read_text(encoding="utf-8"))
        self.vfs_h = _strip_comments(UPSTREAM_VFS_H.read_text(encoding="utf-8"))

    # ---------------------------------------------------------------- 1
    def test_port_c_declares_every_wrapper(self) -> None:
        for relative, names in WRAPPER_DECLARATIONS.items():
            text = _strip_comments((ROOT / relative).read_text(encoding="utf-8"))
            for name in names:
                with self.subTest(symbol=name, declared_in=relative):
                    self.assertIsNotNone(_exact_decl(text, name),
                                         f"{relative} 里缺少 {name} 的 extern 声明")

    def test_landing_layer_defines_every_wrapper_with_c_linkage(self) -> None:
        for name in EXPECTED_WRAPPERS:
            with self.subTest(symbol=name):
                # ⚠ 不能按返回类型过滤:`arm_devfs_setup` 就是 `void`
                #   (它转发上游 `devfs_setup()`,本身就是 void)。
                definitions = _definitions(self.landing, name)
                self.assertTrue(definitions, f"落地层里找不到 {name} 的定义")
                for _, _, _, has_extern_c in definitions:
                    self.assertTrue(has_extern_c,
                                    f"{name} 的定义没写 `extern \"C\"` —— "
                                    f"C 侧给出的是未修饰符号,链接期才会炸")

    def test_wrapper_signatures_agree_across_the_boundary(self) -> None:
        """两边的**返回类型与形参**都必须一致 —— 差一个 `const` 就是"能不能编过"的区别。"""
        for relative, names in WRAPPER_DECLARATIONS.items():
            declared_text = _strip_comments((ROOT / relative).read_text(encoding="utf-8"))
            for name in names:
                with self.subTest(symbol=name, declared_in=relative):
                    declared = _exact_decl(declared_text, name)
                    defined = _definitions(self.landing, name)
                    self.assertIsNotNone(declared, f"{relative} 里没有 {name} 的声明")
                    self.assertTrue(defined, f"落地层里没有 {name} 的定义")
                    _, ret, params, _ = defined[0]
                    self.assertEqual((declared[0], declared[1]), (ret, params),
                                     f"{name} 在 {relative} 与落地层的签名不一致")

    # ---------------------------------------------------------------- 2
    def test_upstream_calls_are_declared_upstream(self) -> None:
        """
        落地层调用的上游函数必须在上游头里有声明 —— 否则就是"凭记忆写的名字",
        编译能过(隐式声明在 C++ 里是错误,但写对了名字就看不出来)、链接才炸。
        """
        # 名字 → 载荷参数个数(只数个数:类型由上游头说了算,这里不做第二份真相)
        calls = {
            "vfs_init": 0,
            "vfs_mkfile": 1,
            "vfs_open": 1,
            "vfs_write": 4,
            "vfs_read": 4,
            "get_rootdir": 0,
        }
        for name, arity in calls.items():
            with self.subTest(symbol=name):
                declared = _exact_decl(self.vfs_h, name)
                self.assertIsNotNone(declared, f"上游 vfs.h 里没有 {name}")
                self.assertEqual(len(declared[1]), arity, f"{name} 的形参个数与上游不符")

    def test_tmpfs_setup_is_declared_like_upstream_does(self) -> None:
        """
        `tmpfs_setup()` 上游**没有**放进任何头文件 —— `kernel/main.cpp:424`
        自己 `extern int tmpfs_setup();`。落地层照抄这个做法。
        """
        main_cpp = _strip_comments((ROOT / "kernel/main.cpp").read_text(encoding="utf-8"))
        self.assertIn("tmpfs_setup", main_cpp)
        self.assertIn("tmpfs_setup", self.landing)
        upstream_tmpfs = (UPSTREAM_TMPFS.read_text(encoding="utf-8"))
        self.assertIn("int tmpfs_setup(void)", upstream_tmpfs,
                      "上游 tmpfs.cpp 里 tmpfs_setup 的签名变了")

    # ---------------------------------------------------------------- 3
    def test_upstream_vfs_header_has_no_extern_c(self) -> None:
        """
        ★ 整组断言的前提 ★

        VFS 的 API 是 C++ 链接的,所以"必须走落地层"。哪天上游给 vfs.h
        加上了 `extern "C"`,这条会失败 —— 那不是坏事,是提醒:
        那时可以直接在 C 里调,落地层这一组就该被重新审视(留着也不算错,
        但理由变了,注释也得跟着改)。
        """
        raw = UPSTREAM_VFS_H.read_text(encoding="utf-8")
        self.assertNotIn('extern "C"', raw,
                         "上游 vfs.h 现在有 extern \"C\" 了 —— "
                         "落地层那组转发的存在理由变了,请重新评估")

    # ---------------------------------------------------------------- 4
    def test_tmpfs_register_id_is_consistent(self) -> None:
        """
        `tmpfs_setup()` 用 `TMPFS_REGISTER_ID` 当"设备路径"传给 `vfs_mount`,
        而 `tmpfs_mount()` 里用 `(uint64_t)src != TMPFS_REGISTER_ID` 认它。
        两处必须同一个数 —— 不然"挂载"会静默失败(而 `tmpfs_setup()` 会
        返回 -EIO)。
        """
        vfs_h_raw = UPSTREAM_VFS_H.read_text(encoding="utf-8")
        match = re.search(r"TMPFS_REGISTER_ID\s*=\s*(\d+)", vfs_h_raw)
        self.assertIsNotNone(match, "vfs.h 里找不到 TMPFS_REGISTER_ID")
        value = match.group(1)

        tmpfs_raw = UPSTREAM_TMPFS.read_text(encoding="utf-8")
        self.assertIn("(uint64_t)src != TMPFS_REGISTER_ID", tmpfs_raw,
                      "tmpfs_mount() 的挂载点判据变了")
        self.assertIn(f"TMPFS_REGISTER_ID, 0x01021994", tmpfs_raw,
                      "tmpfs_setup() 的注册参数变了")
        self.assertEqual(value, "2", "TMPFS_REGISTER_ID 的数值变了")

    # ---------------------------------------------------------------- 5
    def test_mount_witness_is_still_fsid(self) -> None:
        """
        "挂载真的发生了"这条判据靠 `node->fsid = tmpfs_id`
        (`tmpfs.cpp:111`)。这条断言盯的是**判据的根**:
        哪天上游不在 mount 里写 fsid 了,`vfs_tmpfs_mounted` 就会
        变成一条永远为假的判据(而那是最糟的失败形态:看起来在验东西)。
        """
        tmpfs_raw = UPSTREAM_TMPFS.read_text(encoding="utf-8")
        self.assertRegex(tmpfs_raw, r"node->fsid\s*=\s*tmpfs_id\s*;")
        self.assertRegex(tmpfs_raw, r"tmpfs_id\s*=\s*vfs_regist\(")

    def test_port_header_exposes_the_result_struct(self) -> None:
        header = _strip_comments((ROOT / "arch/arm32/include/arch/vfs_check.h").read_text(encoding="utf-8"))
        self.assertIn("vfs_check_t", header)
        self.assertIn("vfs_check_run", header)
        self.assertIn("VFS_CHECK_PAYLOAD_LEN", header)

    # ---------------------------------------------------------------- 6
    def test_pipe_bridges_reference_upstream_globals(self) -> None:
        """
        管道那一组要碰上游**两个全局变量**(`pipefs_root` / `pipefs_id`)。

        ⚠ 顺便记一条**容易误判的事实**:GCC **不修饰全局变量名**(只修饰函数与
        有作用域的实体)⇒ C++ 里 `extern vfs_node_t pipefs_root;` 与上游 C++ 定义
        里那个变量是**同一个符号**(实测两边都是未修饰的)。
        这也是上游 `extern device_t device_ctl[26];` 能直接接到移植侧 C 定义上的
        原因。这条断言钉住"管道那一组确实靠这条路",免得有人以为还需要
        `extern "C"` 而去改上游。
        """
        landing = self.landing
        for symbol in ("pipefs_root", "pipefs_id"):
            with self.subTest(symbol=symbol):
                self.assertRegex(landing, r"extern\s+\w[\w \*]*\b" + re.escape(symbol) + r"\s*;")

        pipefs_raw = (ROOT / "driver/fs/vfs/pipefs.cpp").read_text(encoding="utf-8")
        # ⚠ `assertRegex` 的第三个参数是 **msg**,不是 flags —— 想带 re.M 必须
        #   传一个**编译好的**模式(第一版写成 assertRegex(text, pat, re.M),
        #   于是 re.M 被当成消息字符串,锚点没生效 ⇒ 假 FAIL)。
        self.assertRegex(pipefs_raw, re.compile(r"^vfs_node_t pipefs_root = NULL;", re.M))
        self.assertRegex(pipefs_raw, re.compile(r"^int\s+pipefs_id\s+= 0;", re.M))

    def test_pipe_check_never_reads_an_empty_pipe(self) -> None:
        """
        ★ 安全约束的机械判据 ★

        `pipefs_read()` 在**没有数据**时会循环 `pipe_wait_on()` 等写者
        (只有 `write_fds == 0` 才立即返回 0)⇒ 在启动自检里读空管道就是
        **当场挂住**。所以 `pipe_check_run()` 里每一次读之前必须有一次写。

        这里做的是**顺序检查**:按出现次序,`arm_pipe_write` 的调用必须
        先于同一步里的 `arm_pipe_read`。它不是形式主义 —— 这条约束失效的
        症状是"板子卡死",而卡死时没有任何日志能告诉你原因。
        """
        source = _strip_comments((ROOT / "arch/arm32/src/pipe_check.c").read_text(encoding="utf-8"))
        events = [(m.start(), "write" if "arm_pipe_write" in m.group(0) else "read")
                  for m in re.finditer(r"arm_pipe_(?:write|read)\s*\(", source)]
        self.assertTrue(events, "pipe_check.c 里没有任何读写调用?")

        pending_write = False
        for _, kind in events:
            if kind == "write":
                pending_write = True
            else:
                self.assertTrue(pending_write,
                                "pipe_check.c 里出现了一次**先于任何写**的读 —— "
                                "读空管道会让启动自检当场挂住")
                pending_write = False


if __name__ == "__main__":
    unittest.main()
