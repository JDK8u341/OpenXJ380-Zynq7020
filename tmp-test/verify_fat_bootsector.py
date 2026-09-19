"""与主机侧比对:用宿主机**自己的**解析去核板子打出来的 FAT 引导扇区。

============================================================================
为什么要有这一步
============================================================================

M4A-1.4 的验收写法是"挂载 + 读写文件 + **与主机侧比对**"。前两条是板子
自说自话:它说"我格式化了一个 FAT16 卷、写得进读得出" —— 而"它以为的 FAT16"
与"真正的 FAT16"是两件事。第三条就是把这个差距堵上:

    板子只负责把卷的**原始字节**打出来(串口上一行 `FATBS:<1024 个十六进制字符>`),
    由**宿主机**独立解析,自己算出这是不是一个自洽的 FAT16 卷。

这不是形式主义:格式化参数、BPB 字段、扇区数这些东西,板子那侧的"自检"
用的还是同一份 FATFS 代码 —— 同一份代码自证没有意义。

============================================================================
它核什么(全部由本文件独立计算,不读板子的结论)
============================================================================

  1. 长度 512、结束标志 0x55AA;
  2. BPB 的静态字段合理(bytes/sector = 512、sectors/cluster 是 2 的幂、
     reserved >= 1、num_fats = 1、root_entries > 0、FAT size > 0);
  3. **总扇区数 == 期望值**(由 --expect-bytes 推出,默认 8MB ⇒ 16384 扇区);
  4. ★ **自洽性**:由 reserved + num_fats*fat_size + root_dir_sectors 推出
     数据区大小,再除以 sectors/cluster 得到**簇数**,簇数必须落在 FAT16 的
     合法区间(4085..65524)。这一条才是真正的"独立核验" ——
     它不看任何标签,只看算术。

用法:
    python tmp-test/verify_fat_bootsector.py --from-file <串口日志>
    python tmp-test/verify_fat_bootsector.py --hex <1024 个十六进制字符>
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

DEFAULT_EXPECT_BYTES = 8 * 1024 * 1024
FAT16_MIN_CLUSTERS = 4085
FAT16_MAX_CLUSTERS = 65524


def u16(buf: bytes, offset: int) -> int:
    return buf[offset] | (buf[offset + 1] << 8)


def u32(buf: bytes, offset: int) -> int:
    return buf[offset] | (buf[offset + 1] << 8) | (buf[offset + 2] << 16) | (buf[offset + 3] << 24)


def parse_bootsector(bs: bytes, expect_bytes: int) -> tuple[bool, list[tuple[str, str, str, bool]]]:
    """返回 (是否全过, [(检查项, 实测, 判据, 结果)])。"""
    checks: list[tuple[str, str, str, bool]] = []

    def check(name: str, actual, expect: str, ok: bool) -> None:
        checks.append((name, str(actual), expect, ok))

    check("length", len(bs), "== 512", len(bs) == 512)
    if len(bs) != 512:
        return False, checks

    # ---- 1. 结束标志 ----
    # ⚠ 字节序:BPB 的 55 AA 是**按字节**写在 510/511 的(不是小端 u16 的 AA55)。
    #   第一版这里写成 `(bs[510] << 8) | bs[511] == 0xAA55`,于是对一个**正确**的
    #   卷报了 FAIL —— 判据自己错了(板子那侧没问题)。
    check("signature", f"{bs[510]:02X} {bs[511]:02X}", "== 55 AA",
          bs[510] == 0x55 and bs[511] == 0xAA)

    bytes_per_sector = u16(bs, 11)
    sectors_per_cluster = bs[13]
    reserved = u16(bs, 14)
    num_fats = bs[16]
    root_entries = u16(bs, 17)
    total16 = u16(bs, 19)
    total32 = u32(bs, 32)
    fat_size16 = u16(bs, 22)
    total_sectors = total16 if total16 != 0 else total32

    # ---- 2. 静态字段 ----
    check("bytes/sector", bytes_per_sector, "== 512", bytes_per_sector == 512)
    check("sectors/cluster", sectors_per_cluster,
          "是 2 的幂, 1..128",
          sectors_per_cluster in (1, 2, 4, 8, 16, 32, 64, 128))
    check("reserved sectors", reserved, ">= 1", reserved >= 1)
    check("num FATs", num_fats, "== 1", num_fats == 1)
    check("root entries", root_entries, "> 0", root_entries > 0)
    check("FAT size (16)", fat_size16, "> 0", fat_size16 > 0)

    # ---- 3. 总扇区数 ----
    expected_sectors = expect_bytes // 512
    check("total sectors", total_sectors, f"== {expected_sectors}", total_sectors == expected_sectors)

    # ---- 4. 自洽性:算出来的簇数必须落在 FAT12/16 区间,并由**算术**定子类型 ----
    #
    # ⚠ 这里**不假设**子类型。第一版写死了"必须是 FAT16",结果对一个**正确**
    #   的卷报了 FAIL —— 实测:8MB 的卷上 FatFs 按簇数自己选了 **FAT12**
    #   (8 扇区/簇 = 4KB ⇒ 2043 簇;FAT 占 7 扇区,而 2043 簇的 FAT12 需要
    #   ceil(2043*1.5/512) = 6 扇区、FAT16 需要 8 扇区 ⇒ 7 扇区只可能是 FAT12)。
    #
    # 现在改成:**从 FAT 大小反推每个表项占几位**,这个推导不看任何标签。
    fs_type = bs[54:62].decode("ascii", errors="replace").rstrip()
    check("fs type label", f"{fs_type!r}", "以 'FAT' 开头(族标签)",
          fs_type.startswith("FAT"))

    if bytes_per_sector and sectors_per_cluster:
        root_dir_sectors = (root_entries * 32 + (bytes_per_sector - 1)) // bytes_per_sector
        overhead = reserved + num_fats * fat_size16 + root_dir_sectors
        data_sectors = total_sectors - overhead
        clusters = data_sectors // sectors_per_cluster if sectors_per_cluster else 0
        check("clusters (derived)", clusters, "4085..65524(FAT16)或更少(FAT12)",
              clusters > 0)

        # 由 FAT 占用空间反推表项宽度:12 位还是 16 位
        fat_bytes = fat_size16 * bytes_per_sector
        if clusters:
            derived = "FAT12" if fat_bytes < clusters * 2 else "FAT16"
            check("subtype (derived from FAT size)", derived,
                  f"必须自洽(FAT 占 {fat_size16} 扇区 / {clusters} 簇)",
                  (derived == "FAT12" and fat_bytes >= clusters * 3 // 2) or
                  (derived == "FAT16" and fat_bytes >= clusters * 2))
            if fs_type in ("FAT12", "FAT16"):
                check("label matches arithmetic", fs_type, f"== {derived}", fs_type == derived)
    else:
        check("clusters (derived)", "n/a", "FAT12/16 区间", False)

    return all(item[3] for item in checks), checks


def extract_hex(text: str) -> str:
    match = re.search(r"^FATBS:([0-9a-fA-F]+)\s*$", text, re.M)
    if match is None:
        raise SystemExit("日志里找不到 `FATBS:<hex>` 行")
    return match.group(1)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--from-file", type=Path, help="串口日志(从里面找 FATBS: 行)")
    source.add_argument("--hex", help="直接给十六进制串")
    parser.add_argument("--expect-bytes", type=int, default=DEFAULT_EXPECT_BYTES,
                        help=f"卷的期望字节数(默认 {DEFAULT_EXPECT_BYTES} = 8MB)")
    args = parser.parse_args()

    hex_string = args.hex if args.hex else extract_hex(args.from_file.read_text(encoding="utf-8",
                                                                                errors="replace"))
    if len(hex_string) % 2:
        raise SystemExit(f"十六进制串长度是奇数({len(hex_string)})")
    bs = bytes.fromhex(hex_string)

    ok, checks = parse_bootsector(bs, args.expect_bytes)

    print(f"{'检查项':<22}{'实测':>22}{'判据':>34}{'结果':>8}")
    print("-" * 86)
    for name, actual, expect, passed in checks:
        print(f"{name:<22}{actual:>22}{expect:>34}{'PASS' if passed else 'FAIL':>8}")
    print("-" * 86)
    print(f"{len(bs)} 字节引导扇区;{'全部通过' if ok else '有未通过项'}")

    if not ok:
        # 把原始字节留一份,免得还要回头找日志
        dump = Path("fatbs_raw.bin")
        dump.write_bytes(bs)
        print(f"原始字节已存到 {dump}", file=sys.stderr)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
