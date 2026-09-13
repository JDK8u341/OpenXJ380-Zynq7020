"""`config.local.py` 的样板 —— 想把自己的机器路径留在 git 外面就照这个建。

    cp config.example.py config.local.py
    # 然后只改 config.local.py(它被 .gitignore 忽略,不会被提交)

它会被仓库根目录的 `config.py` 在末尾执行,因此**只需要写要覆盖的项**,
其余照旧。写错了(=没有这一项)会在 import 时直接报错,不会静默失效。
"""

# Vitis / Vivado 安装目录
VITIS_DIR = r"C:\AMDDesignTools\2025.2"

# 板子 PS UART1 接到 PC 的串口
SERIAL_PORT = "COM4"

# PL 比特流。**同一套设计换一块板时不要写这一项** —— 仓库里已经带了一份
# (arch/arm32/board/System_wrapper.bit),config.py 默认就指它。
# 只有换 PL 设计 / 换板子时才覆盖成自己的(形状通常是 <平台>\hw\sdt\System_wrapper.bit):
# BITSTREAM = r"C:\path\to\your\platform\hw\sdt\System_wrapper.bit"

# 可选:原理图 PDF(tmp-test/sch_render.py 用)
# SCHEMATIC_PDF = r"D:\path\to\schematic.pdf"
