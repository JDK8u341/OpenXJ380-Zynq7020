"""一次性:把 M4A-1.5 的两处文档(计划 §0.5.1/§0.5.2/§4.6)更新到"管道验收完成"。

只用标准库,按行拼接后写回 —— 避免 PowerShell 里中文引号把命令引号打乱。
用完即弃(不是产品代码)。
"""

import pathlib

path = pathlib.Path("docs/ZYNQ7020_PORT_PLAN.md")
lines = path.read_text(encoding="utf-8").splitlines()
text = "\n".join(lines)

# ---- 1. §0.5.1 状态:136/0 + 管道 ----
old_status = """**★ M4A-1.1a / 1.1b / 1.2a / 1.2b / 1.4 全部完成,1.5 的 `dev` 与 `pipe` 也已进图**
—— 上游 VFS、FATFS、devfs 都在板上跑起来了:板上 **127 passed / 0 failed**,"""
new_status = """**★ M4A-1.1a / 1.1b / 1.2a / 1.2b / 1.4 全部完成,1.5 的 `dev` 与 `pipe` 也已进图并上板验证**
—— 上游 VFS、FATFS、devfs、pipefs 都在板上跑起来了:板上 **136 passed / 0 failed**,"""
assert old_status in text, "status 段没找到"
text = text.replace(old_status, new_status, 1)

old_next = """**下一步是 M4A-1.5 的其余项(`pty`、`pipe` 自己的往返自检)与 M4A-1.3(真实块设备 SD/arasan)。**"""
new_next = """★ **管道的验收也补上了(板上 136/0)**:内存往返 + 两个独立观测点
(`pipe_fill_after=32` / `pipe_fill_drain=0`)+ 短读语义(写 8、按 32 读 ⇒ 8);
并留下一条**机器可判的安全约束**(启动自检**不许读空管道** —— 那会阻塞死)。
**下一步是 M4A-1.5 的其余项(`pty`、`procfs` 的 ARM 侧取舍)与 M4A-2/3/B5;
M4A-1.3(真实块设备 SD/arasan)需要插卡,到了那一步会弹窗提醒。**"""
assert old_next in text, "下一步 段没找到"
text = text.replace(old_next, new_next, 1)

# ---- 2. §0.5.2 提交表:补一行 ----
anchor = "| **★ M4A-1.5（dev 部分）：上游 devfs 取代移植侧骨架 + 抓到\"幽灵设备\"** |"
idx = text.index(anchor)
end = text.index("\n", idx)
row = ("\n| **★ M4A-1.5（pipe 部分）：管道的读写往返上板（136/0）** | 本笔提交 | "
       "九条新判据 `pipe_setup` / `pipe_create` / `pipe_write=32` / **`pipe_fill_after=32`** / "
       "`pipe_read=32` / `pipe_roundtrip` / **`pipe_fill_drain=0`** / `pipe_short_write=8` / "
       "`pipe_short_read=8`；宿主 `test_arm32_vfs.py` 11 条 + 50 子判据（含一条**顺序检查**："
       "每次读之前必须有写 ⇒ 把\"不许读空管道\"钉成机器可判） |")
text = text[:end] + row + text[end:]

path.write_text(text + "\n", encoding="utf-8")
print("updated:", len(text.splitlines()), "lines")
print("has pipe row:", "M4A-1.5（pipe 部分）" in text)
