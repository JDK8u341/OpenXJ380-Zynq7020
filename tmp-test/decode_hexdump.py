"""把 run_and_capture.py 的十六进制转储还原成文本(只做这一件事)。

为什么要单独一个工具:`run_and_capture.py` 用 `ascii` 解码(中文会变问号),
但**十六进制列是完整的** —— 从十六进制列还原是无损的。
`FATBS:` 那一行是纯 ASCII,所以用这条路拿它最稳。

用法:
    python tmp-test/decode_hexdump.py tmp-test/board_fatfs.hexdump > board.txt
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

# 形如:  000AB0  61 6C 3D 31 30 ...  |al=10 ...|
LINE = re.compile(r"^\s*[0-9A-Fa-f]{4,8}\s+((?:[0-9A-Fa-f]{2}\s+)+)")


def decode(path: Path) -> bytes:
    out = bytearray()
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = LINE.match(line)
        if match is None:
            continue
        out.extend(bytes.fromhex(match.group(1)))
    return bytes(out)


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    data = decode(Path(sys.argv[1]))
    sys.stdout.buffer.write(data)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
