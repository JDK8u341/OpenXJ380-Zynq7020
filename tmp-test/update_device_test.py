"""一次性:把 tests/test_arm32_device.py 从"devfs 骨架"时代更新到"上游 devfs"时代。

改什么、为什么:
  1. 被测代码不再包含 src/devfs.c(骨架已退场,真 devfs 是上游 dev.cpp,宿主编不了)
     ⇒ 编译列表去掉它,harness 里给一个**桩**,语义照上游 dev.cpp:344。
  2. 骨架特有的观测点(devfs_lookup / devfs_node_count / devfs_ab_set_disabled)不存在了
     ⇒ 相关 CHECK 删掉;那半边证据现在在**板上**(device_devfs_ab,走 vfs_open)。
  3. ★ 幽灵设备:上游 delete_device 把 `flag = 0` 写在**局部拷贝**上,
     设备表那一条没清 ⇒ `get_device(id)` 删除后仍非 NULL。
     这一段旧判据**故意**断言了那个行为,并在注释里写明"哪天改成清 flag,
     要是一个有意识的决定"。M4A-1.5 就是那个决定:按上游**写出来的意图**清掉。
     ⇒ 断言翻面,并把理由写进注释。

用完即弃(不是产品代码)。
"""

import pathlib
import re

path = pathlib.Path("tests/test_arm32_device.py")
text = path.read_text(encoding="utf-8")
orig = text

# ---- 1. 编译列表:去掉 devfs.c ----
text = text.replace('                str(ROOT / "arch/arm32/src/devfs.c"),\n', "")

# ---- 2. 契约豁免名单:新加的移植侧入口 ----
text = text.replace(
    '            if proto.startswith(("int devfs_lookup", "size_t devfs_node_count",\n'
    '                                 "void devfs_reset", "void devfs_ab_set_disabled")):',
    '            if proto.startswith(("size_t arm_disk_size", "void arm_devfs_setup")):')

# ---- 3. harness:加 devfs 桩(放在 make_dev 之前) ----
stub = '''
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

static void make_dev('''
text = text.replace("\nstatic void make_dev(", stub, 1)

# ---- 4. 删掉骨架特有的 CHECK ----
drop_patterns = [
    r"^ *CHECK\(devfs_node_count\(\)[^\n]*\n",
    r"^ *CHECK\(devfs_lookup\([^\n]*\n",
    r"^ *CHECK\(devfs_ab_set_disabled[^\n]*\n",
    r"^ *devfs_ab_set_disabled\([^\n]*\n",
]
for pattern in drop_patterns:
    text = re.sub(pattern, "", text, flags=re.M)

# ---- 5. 幽灵设备那段:断言翻面 + 说明 ----
old_block = text[text.index("    /*\n     * ⚠ 这里**故意断言"):text.index("    /* 越界注销必须是 no-op")]
new_block = '''    /*
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

'''
text = text.replace(old_block, new_block, 1)

# 越界注销那一段后面残留的 devfs_node_count 已经在上面的正则里删掉;补一条断言
text = text.replace(
    "    delete_device((int)DEVICE_TABLE_SIZE + 7);\n",
    "    delete_device((int)DEVICE_TABLE_SIZE + 7);\n"
    "    CHECK(get_device(0u) == NULL);             /* 越界注销不能碰别人的槽位 */\n")

path.write_text(text, encoding="utf-8")
print("changed:", text != orig)
print("devfs_node_count left:", text.count("devfs_node_count"))
print("devfs_lookup left:", text.count("devfs_lookup"))
print("devfs_ab_set_disabled left:", text.count("devfs_ab_set_disabled"))
print("devfs.c in build list:", "src/devfs.c" in text)
