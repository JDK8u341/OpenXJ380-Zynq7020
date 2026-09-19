"""一次性:把 §4.6 M4A-1.5 行里"pipe 自检未做"改成已完成。用完即弃。"""

import pathlib

p = pathlib.Path("docs/ZYNQ7020_PORT_PLAN.md")
t = p.read_text(encoding="utf-8")

old = ("⬜ **未完**：`procfs.cpp`（x86 专属，见 §4.6 的说明）、`pty.cpp`、"
       "以及 **`pipe` 自己的读写往返自检** |")

new = ("✅ **`pipe` 的读写往返自检已补（板上 136/0）**：九条判据，含两个**独立观测点**"
       "（`pipe_fill_after=32` / `pipe_fill_drain=0` —— 管道里**现在**有多少字节，"
       "与\"它说读写了多少\"是两件事；只看后者，一个原样返回入参的空壳也能全绿）"
       "与短读语义（写 8、按 32 读 ⇒ 8，证返回量由**存量**决定）。"
       "★ 并立下一条**机器可判的安全约束**：启动自检**不许读空管道**"
       "（`pipefs_read` 会 `pipe_wait_on` 阻塞 ⇒ 当场挂住且没有任何日志），"
       "由宿主测试的顺序检查钉住。<BR>⬜ **仍未完**：`procfs.cpp`（x86 专属：CPUID 特性名表 + "
       "数组指定初始化器）、`pty.cpp` |")

assert old in t, "M4A-1.5 行的尾巴没找到"
p.write_text(t.replace(old, new, 1), encoding="utf-8")
print("M4A-1.5 row updated; pipe 部分 =", "pipe 的读写往返自检已补" in p.read_text(encoding="utf-8"))
