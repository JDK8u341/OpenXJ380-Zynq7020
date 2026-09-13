"""设备描述生成器（tools/gen_board_desc.py）的单元测试。

分两部分：

**一、解码与解析的纯逻辑**
  重点是 `_INTERRUPTS` 的解码。xparameters.h 里那个值**不是 INTID**,
  而是"相对号 + 触发类型 + SPI/PPI"的编码,实际 INTID 还要加偏移
  (SPI +32 / PPI +16)。差 32 的错会让驱动挂到一个完全无关的中断上,
  而**编译期完全不会暴露** —— 只有当那个中断恰好来了才会表现异常。
  基准值全部取自本板真实的 xparameters.h,并已用硬件行为交叉验证:
  SCUTIMER 解出 29,正是我们实际在用的 Cortex-A9 私有定时器。

**二、生成物与输入一致**
  重新跑一遍生成器,与仓库里已提交的生成物逐字节比较。
  这条能抓住"有人手改了生成文件" —— 那是比生成器本身出错更难查的情况,
  因为手改的部分会在下次重新生成时无声消失。
"""

from __future__ import annotations

import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
GENERATOR = ROOT / "tools" / "gen_board_desc.py"
XPARAMETERS = ROOT / "arch" / "arm32" / "board" / "xparameters.h"
GENERATED_C = ROOT / "arch" / "arm32" / "src" / "board_devices.c"
GENERATED_H = ROOT / "arch" / "arm32" / "include" / "arch" / "board_devices.h"


def _load_generator():
    """把生成器当模块导入，以便直接测它的纯函数。"""
    spec = importlib.util.spec_from_file_location("gen_board_desc", GENERATOR)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class IntrDecodeTests(unittest.TestCase):
    """`_INTERRUPTS` 解码 —— 本文件最重要的一组测试。"""

    @classmethod
    def setUpClass(cls) -> None:
        cls.gen = _load_generator()

    def test_real_board_values(self) -> None:
        """全部取自本板 xparameters.h 的真实值。"""
        cases = [
            # (原始编码, 期望 INTID, 期望触发类型, 说明)
            (0x13100D, 29, 1, "SCUTIMER: PPI 13 + 16 = 29,正是私有定时器"),
            (0x10400E, 30, 4, "SCUWDT: PPI 14 + 16 = 30"),
            (0x4013, 51, 4, "QSPI: SPI 19 + 32 = 51"),
            (0x4008, 40, 4, "DEVCFG: SPI 8 + 32 = 40"),
            (0x400D, 45, 4, "DMAC_S: SPI 13 + 32 = 45"),
        ]
        for encoded, want_irq, want_trigger, why in cases:
            irq, trigger, _note = self.gen.decode_irq(encoded)
            self.assertEqual(irq, want_irq, f"INTID for 0x{encoded:X} ({why})")
            self.assertEqual(trigger, want_trigger, f"trigger for 0x{encoded:X} ({why})")

    def test_ppi_offset_is_16_not_32(self) -> None:
        """PPI 的偏移是 16 而不是 32 —— 用同一个相对号对比两种类型。"""
        # rel = 13;PPI -> 29,SPI -> 45
        ppi = 13 | (1 << 20)
        spi = 13
        self.assertEqual(self.gen.decode_irq(ppi)[0], 29, "PPI 13 must decode to 29")
        self.assertEqual(self.gen.decode_irq(spi)[0], 45, "SPI 13 must decode to 45")

    def test_zero_means_no_interrupt(self) -> None:
        irq, trigger, note = self.gen.decode_irq(0)
        self.assertEqual(irq, -1, "0 means no interrupt")
        self.assertEqual(trigger, 0)
        self.assertIn("none", note)

    def test_sgi_is_rejected(self) -> None:
        """SGI 是软件产生的,不该出现在设备描述里。"""
        sgi = 3 | (1 << 22)
        irq, _trigger, note = self.gen.decode_irq(sgi)
        self.assertEqual(irq, -1, "an SGI must not become a device interrupt")
        self.assertIn("SGI", note)

    def test_relative_field_is_12_bits(self) -> None:
        """相对号是 bits[11:0],高位不能串进来。"""
        encoded = 0xFFF | (1 << 20)  # PPI, rel = 0xFFF
        self.assertEqual(self.gen.decode_irq(encoded)[0], 0xFFF + 16)


class ParsingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.gen = _load_generator()

    def test_parse_defines_only_collects_xpar(self) -> None:
        """
        解析器只收 XPAR_* 开头的定义。

        这是有意的收窄:xparameters.h 里还有大量与设备描述无关的宏
        (版本号、位宽常量等),全部收进来既浪费内存也让后续过滤更容易出错。
        测试把它写成断言,免得以后有人"顺手放宽"了却没人注意。
        """
        text = """
#define XPAR_FOO_COMPATIBLE "xlnx,foo-1.0"
#define XPAR_FOO_BASEADDR 0x41200000
#define XPAR_FOO_HIGHADDR 0x4120ffff  /* trailing comment */
#define NOT_XPAR_BAR 1
"""
        defines = self.gen.parse_defines(text)
        self.assertEqual(defines["XPAR_FOO_COMPATIBLE"], "xlnx,foo-1.0", "quotes stripped")
        self.assertEqual(defines["XPAR_FOO_BASEADDR"], "0x41200000")
        self.assertEqual(
            defines["XPAR_FOO_HIGHADDR"], "0x4120ffff", "trailing comment stripped"
        )
        self.assertNotIn("NOT_XPAR_BAR", defines, "non-XPAR defines are not collected")

    def test_canonical_duplicates_are_folded(self) -> None:
        """
        每个设备在 xparameters.h 里出现两次:外设名形式与驱动规范形式。
        不去重的话描述表里每个设备会有两个节点、驱动 probe 两次。
        """
        text = """
#define XPAR_AXI_GPIO_0_COMPATIBLE "xlnx,axi-gpio-2.0"
#define XPAR_AXI_GPIO_0_BASEADDR 0x41200000
#define XPAR_AXI_GPIO_0_HIGHADDR 0x4120ffff
#define XPAR_XGPIO_0_COMPATIBLE "xlnx,axi-gpio-2.0"
#define XPAR_XGPIO_0_BASEADDR 0x41200000
#define XPAR_XGPIO_0_HIGHADDR 0x4120ffff
"""
        nodes, warnings = self.gen.collect_nodes(self.gen.parse_defines(text))
        self.assertEqual(len(nodes), 1, "the canonical duplicate must be folded away")
        self.assertEqual(nodes[0].instance, "XPAR_AXI_GPIO_0", "the peripheral form wins")
        self.assertTrue(any("规范形式" in w for w in warnings), "the folding must be reported")

    def test_prop_names_follow_upstream_dts(self) -> None:
        """
        IS_DUAL -> xlnx,is-dual。
        Xilinx 自己的 xgpio_g.c 里注释写的就是 "xlnx,is-dual" / "xlnx,gpio-width",
        所以这个名字不是我们编的。
        """
        node = self.gen.Node("XPAR_AXI_GPIO_0", "xlnx,axi-gpio-2.0")
        self.assertEqual(node.prop_name("IS_DUAL"), "xlnx,is-dual")
        self.assertEqual(node.prop_name("GPIO_WIDTH"), "xlnx,gpio-width")
        self.assertEqual(node.prop_name("INTERRUPT_PRESENT"), "xlnx,interrupt-present")

    def test_node_name_drops_the_xpar_prefix(self) -> None:
        node = self.gen.Node("XPAR_AXI_GPIO_0", "xlnx,axi-gpio-2.0")
        self.assertEqual(node.name, "axi_gpio_0")

    def test_pl_detection(self) -> None:
        """PL 落在 0x40000000-0xBFFFFFFF,当前一律标记为未启用。"""
        pl = self.gen.Node("XPAR_AXI_GPIO_0", "xlnx,axi-gpio-2.0")
        pl.base = 0x41200000
        ps = self.gen.Node("XPAR_QSPI", "xlnx,zynq-qspi-1.0")
        ps.base = 0xE000D000
        self.assertTrue(pl.is_pl)
        self.assertFalse(ps.is_pl)

    def test_extra_interrupt_lines_do_not_become_props(self) -> None:
        """
        多中断设备的第 N 条中断线不能被当成不透明的原始编码值塞进 props ——
        驱动读到的是个没有意义的数字。应当丢掉并报警告。
        """
        text = """
#define XPAR_DMAC_S_COMPATIBLE "arm,pl330"
#define XPAR_DMAC_S_BASEADDR 0xf8003000
#define XPAR_DMAC_S_HIGHADDR 0xf8003fff
#define XPAR_DMAC_S_INTERRUPTS 0x400d
#define XPAR_DMAC_S_INTERRUPTS_1 0x400e
#define XPAR_DMAC_S_INTERRUPTS_2 0x400f
"""
        nodes, _warnings = self.gen.collect_nodes(self.gen.parse_defines(text))
        node = nodes[0]
        self.assertEqual(node.irq, 45, "the first interrupt line goes into irq")
        self.assertEqual(len(node.extra_irqs), 2, "the extra lines are recorded, not stored")
        names = [name for name, _ in node.props]
        self.assertNotIn("arm,interrupts-1", names, "encoded values must not leak into props")


class RegenerationTests(unittest.TestCase):
    """重新生成一遍，与仓库里的生成物比对 —— 抓"有人手改了生成文件"。"""

    def test_generated_files_are_up_to_date(self) -> None:
        self.assertTrue(GENERATOR.exists(), "generator must exist")
        self.assertTrue(XPARAMETERS.exists(), "the vendored xparameters.h must exist")

        with tempfile.TemporaryDirectory() as tmp:
            out_c = Path(tmp) / "board_devices.c"
            out_h = Path(tmp) / "board_devices.h"

            result = subprocess.run(
                [
                    sys.executable,
                    str(GENERATOR),
                    str(XPARAMETERS),
                    "--out-c",
                    str(out_c),
                    "--out-h",
                    str(out_h),
                    "--quiet",
                ],
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
            )
            self.assertEqual(result.returncode, 0, f"generator failed: {result.stderr}")

            for generated, committed in ((out_c, GENERATED_C), (out_h, GENERATED_H)):
                self.assertTrue(committed.exists(), f"{committed} must be committed")
                self.assertEqual(
                    generated.read_text(encoding="utf-8").replace("\r\n", "\n"),
                    committed.read_text(encoding="utf-8").replace("\r\n", "\n"),
                    f"{committed.name} 与输入不一致 —— 重新跑生成器,不要手改生成物",
                )

    def test_every_generated_file_says_it_is_generated(self) -> None:
        for path in (GENERATED_C, GENERATED_H):
            head = path.read_text(encoding="utf-8")[:400]
            self.assertIn("生成", head, f"{path.name} 必须在文件头声明是生成物")
            self.assertIn("不要手改", head, f"{path.name} 必须写明不要手改")


if __name__ == "__main__":
    unittest.main()
