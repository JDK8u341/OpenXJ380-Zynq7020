# `arch/arm32/board/` —— 固化进仓库的**硬件身份**

这个目录放的是"**这块板子/这套设计到底是什么**"的原始材料。它们不是代码，
但**换了任何一个，内核就可能点到不存在的寄存器、或者干脆收不到串口**。

> ★ **它们是从 Vivado/Vitis 导出的二进制，仓库里保留原件** ——
> 这样别人 clone 下来就能上板，不必先自己建一遍工程。
> 想自己重新导出、或者确认"我导出的跟你这份是不是同一套设计"，
> 步骤见 [`docs/BUILD_ARM32.md`](../../../docs/BUILD_ARM32.md) §2。

## 文件清单

| 文件 | 是什么 | 来自哪个工程 | 谁用它 |
|---|---|---|---|
| `xparameters.h` | 设备/地址表（Vitis 从 XSA 导出） | **`AXI_GPIO_1`**（有 PL 设计那个） | `tools/gen_board_desc.py` 的**输入**（见下） |
| `System_wrapper.bit` | **PL 比特流**（AXI GPIO → LED 流水灯） | **`AXI_GPIO_1`** | `config.py` 的 `BITSTREAM`；JTAG 加载器烧它 |
| `System_wrapper.xsa` | 上面那个 `.bit` 的**出处**（PL 设计 + PS 配置） | **`AXI_GPIO_1`** | 用来核对/重新导出；**不进构建** |
| `opjtmp.xsa` | **PS 配置的出处**（UART1 已使能、MIO bank1 = 1.8V） | **`opjtmp`**（只有 PS、没有 PL IP） | 仓库里那份 `tmp-test/zynq/ps7_init_uart1.tcl` 就是从它里面取的 |

## ⚠ 这里最容易搞错的一件事

`opjtmp.xsa` 里**也带了一个比特流**（`opxjtmp.bit`），但它的 `.hwh` 里
**只有 `processing_system7`、没有任何 PL IP** —— 烧下去 PL 是空的，**LED 流水灯不会亮**。

**PS 配置**（`ps7_init`）与 **PL 比特流** 来自**两个不同的工程**：
`ps7_init` 管 PS（时钟 / MIO / DDR），比特流管 PL，两者独立、可以交叉组合。

⇒ 判断一个 `.bit` / `ps7_init.tcl` 能不能用，**看它来自哪个工程**，
不要看它放在哪、叫什么名字、多大 —— 那两个 `.bit` 的**大小只差 2 字节**。

## `xparameters.h` 与设备描述表的关系

```
arch/arm32/board/xparameters.h              ← 本目录（手工放置，从 XSA 导出）
        │  python tools/gen_board_desc.py arch/arm32/board/xparameters.h \
        │         --out-c arch/arm32/src/board_devices.c \
        │         --out-h arch/arm32/include/arch/board_devices.h
        ▼
arch/arm32/src/board_devices.c              ← 产物，也进仓库
arch/arm32/include/arch/board_devices.h
        │  ninja 编译/包含的是这两个
        ▼
out/kernel-arm.elf
```

> ⚠ **构建图里没有"重跑生成器"这条规则。** 只改 `xparameters.h` 然后 `ninja`，
> 内核会继续用**旧的**设备表，而且**不报错**。改了它就必须手动重跑上面那条命令。
> （本目录这份 `xparameters.h` 与当前 `board_devices.*` 是一致的，已核对过。
> "同一套设计"的前提下你不需要动它；动了说明设计变了。）

## 来源核对（SHA-256）

拿到一份新的导出物时，可以直接对哈希确认是不是同一份：

| 文件 | SHA-256 |
|---|---|
| `System_wrapper.bit` | `FF1F7C884F1FCB92C1800E4F9DC29B4FB9A81A65F61F92565CA16E15F810F04C` |
| `System_wrapper.xsa` | `DB935BD52AC5E49F444650832A9BC999A6865C767352703288B3669D056B9BF1` |
| `opjtmp.xsa` | `B0B54F340A27A4A98A88DEAAE5571F3162192652B12407DE8BF7545E1152BF75` |
| `xparameters.h` | `git log` 里那一份即当前版本（它是文本，直接 diff 更直观） |
