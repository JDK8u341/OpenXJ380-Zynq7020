"""把原理图 PDF 的指定区域渲染成 PNG,便于用视觉直接读连线关系。

为什么需要它:原理图的文本抽取只能拿到"有哪些器件和网络名",
拿不到"谁连到谁" —— 而这次要回答的恰恰是连通性问题
(PC -> 板 方向的 PS_UART_RXD 是否真的接到了 CH9102F 的 TXD)。

用法:
    python tmp-test/sch_render.py --list                 # 列出关键词坐标
    python tmp-test/sch_render.py --around "CH9102F" --pad 240 --dpi 500
"""

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# 本机路径(工具链/串口/比特流)全在仓库根目录的 config.py —— 换机器只改那一个文件。
sys.path.insert(0, str(ROOT))
import config  # noqa: E402

import pypdfium2 as pdfium
from pypdf import PdfReader

DEFAULT_PDF = Path(config.SCHEMATIC_PDF) if config.SCHEMATIC_PDF else None


def text_items(pdf_path: Path, page_index: int):
    """返回 [(x, y, text)]，坐标为 PDF 用户空间(原点左下、未旋转)。"""
    reader = PdfReader(str(pdf_path))
    page = reader.pages[page_index]
    out = []

    def visit(text, cm, tm, font_dict, font_size):
        t = (text or "").strip()
        if t:
            out.append((tm[4], tm[5], t))

    page.extract_text(visitor_text=visit)
    return out


def find(items, needle):
    needle = needle.lower()
    return [it for it in items if needle in it[2].lower()]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--pdf", type=Path, default=DEFAULT_PDF)
    ap.add_argument("--page", type=int, default=4, help="1-based 页码")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--around", type=str, default=None, help="要居中的关键词")
    ap.add_argument("--pad", type=float, default=90.0, help="四周留白(PDF 点)")
    ap.add_argument("--dpi", type=int, default=600)
    ap.add_argument("--out", type=Path, default=Path("tmp-test/out/sch_crop.png"))
    args = ap.parse_args()

    items = text_items(args.pdf, args.page - 1)
    print(f"页面 {args.page}:{len(items)} 个文本项")

    if args.list or not args.around:
        for x, y, t in sorted(items, key=lambda v: (-v[1], v[0])):
            print(f"  x={x:7.1f} y={y:7.1f}  {t}")
        return 0

    hits = find(items, args.around)
    if not hits:
        print(f"!! 没找到 {args.around!r}", file=sys.stderr)
        return 1
    for x, y, t in hits:
        print(f"  命中 x={x:.1f} y={y:.1f}  {t}")

    doc = pdfium.PdfDocument(str(args.pdf))
    page = doc[args.page - 1]
    dev_w, dev_h = page.get_size()  # 已按 /Rotate 交换宽高
    scale = args.dpi / 72.0
    pad = args.pad

    # 把 PDF 用户坐标(原点左下、未旋转)换算到渲染后的设备坐标(原点左上)。
    # 页面普遍带 /Rotate 90,不能假定不旋转 —— 第一版就是漏了这个才裁错位置。
    raw_w = float(page.get_mediabox()[2])
    raw_h = float(page.get_mediabox()[3])
    rotate = int(page.get_rotation()) % 360

    def to_device(x, y):
        if rotate == 0:
            return x, raw_h - y
        if rotate == 90:
            return y, x
        if rotate == 180:
            return raw_w - x, raw_h - y
        return raw_h - y, raw_w - x  # 270

    dev = [to_device(h[0], h[1]) for h in hits]
    xs = [d[0] for d in dev]
    ys = [d[1] for d in dev]
    box = (
        max(0.0, min(xs) - pad) * scale,
        max(0.0, min(ys) - pad) * scale,
        min(dev_w, max(xs) + pad) * scale,
        min(dev_h, max(ys) + pad) * scale,
    )
    if box[2] - box[0] < 8 or box[3] - box[1] < 8:
        print("!! 裁剪区域太小", file=sys.stderr)
        return 1

    # 整页渲染再用 PIL 裁。pdfium 的 crop 参数单位与文档不符(实测报
    # "Crop exceeds page dimensions"),与其猜不如整页渲染 —— A4 页面在
    # 600dpi 下约 7000x5000,内存完全够,而且坐标只在一处换算,不容易错。
    bitmap = page.render(scale=scale)
    img = bitmap.to_pil().crop(tuple(int(v) for v in box))
    args.out.parent.mkdir(parents=True, exist_ok=True)
    img.save(args.out)
    print(f"已写出 {args.out}  ({img.width}x{img.height} px, {args.dpi} dpi)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
