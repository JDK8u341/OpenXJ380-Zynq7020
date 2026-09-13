# 编译与上板指南（Zynq-7020 / ARM32 分支）

> 这份文档只讲**这一支**（`arch/arm32`，Zynq-7020 上的 Cortex-A9）。
> 上游 x86_64 的构建没被改动，用法见 `OLD_README.md` / `docs/BUILD.md`。
>
> **换一台机器要改的东西只有一个文件：仓库根目录的 `config.py`。**
> 其余脚本都只是 `import config`，逻辑没有变。

---

## 0. 一句话流程

```bash
# ① 导出/解包 XSA 并放好文件(见 §2) → ② 改 config.py → ③ 自检
# → ④ 构建 → ⑤ 上板
python config.py                       # 自检 + 生成 Tcl 要用的 paths.tcl
python tools/gen_ninja.py --out build-arm.ninja --arch arm32
ninja -f build-arm.ninja arm32         # 产物 out/kernel-arm.elf
python tmp-test/verify_board.py --load --seconds 75
```

> 第 ① 步（**前提是「同一套设计、你自己的板」**：自己导出 XSA → 解包 →
> 放到对应路径 → 改 `config.py`）见 **§2**。
> 如果这两样文件你已经有了，直接从第 ② 步开始。

---

## 1. 需要什么

| 项 | 要求 | 备注 |
|---|---|---|
| 主机 | **Windows**（本移植只在 Windows 上验证过） | 脚本里没有 Linux 专属假设，但没实测过 |
| Vitis | **2025.2**（自带 GNU ARM 工具链 + `xsdb`） | 文件名叫 `arm-none-eabi-gcc.exe`，`--version` 自报 `arm-xilinx-eabi-gcc (GCC) 13.3.0` |
| Python | 3.10+ | 只有 `config.py`、构建图生成器与板级脚本用 |
| Ninja | 任意较新版本 | |
| pyserial | `pip install pyserial` | 只有串口脚本用 |
| 硬件 | Zynq-7020 板 + JTAG 下载器 + 串口线 | 本移植是照 AC880-CB / AC850-CORE 调的 |
| 硬件工程 | 从**同一套设计**导出并解包出来的两样东西：**带 PL 设计**那个工程的 `hw\sdt\System_wrapper.bit`，**外加** UART1 已使能、MIO bank1 = 1.8V 那个工程的 `ps7_init.tcl` |★ **见 §2** —— 这两样来自**两个不同工程**，别顺着目录拿文件 |

> `xsdb` 的用法：加载 PS 配置、烧比特流、下载 ELF、读寄存器，全部走 JTAG。
>
> ⚠ 这两半是**分开**的：`ps7_init` 管 PS，比特流管 PL。它们可以、而且现在就是
> 来自不同工程 —— 这一点**必须**先读 §2，否则很容易拿错文件。

---

## 2. ★ 硬件工程：导出 XSA → 解包 → 放路径 → 改 `config.py` ★

### 2.1 这一节适用于什么情况

> **✅ 适用：同一套硬件设计，你自己的板 / 你自己的机器。**
> 设计（PS 的 BD 配置 + PL 的 AXI GPIO）**不用你重新发明**。你要做的是把**这套设计**
> 在你自己的 Vivado/Vitis 里**导出成 XSA**、**解包**、把文件**放到对应路径**、
> 再改 `config.py` 指过去。
>
> **❌ 不适用：改设计本身**（换 IP、改地址、改 MIO 复用）。
> 那会连带改内核侧的 `arch/arm32/board/xparameters.h` 与设备描述表，超出这份指南。

要做的就四步：

| # | 做什么 | 产出 | 落到哪 |
|---|---|---|---|
| ① | 从**两个工程**各导出一次 XSA | `*.xsa`（其实是个 ZIP） | 你自己的目录，随便放 |
| ② | 解包，挑出真正要用的两个文件 | `*.bit`、`ps7_init.tcl` | 见 **§2.5** |
| ③ | 按 §2.5 的对照表放到对应路径 | — | 一个进仓库，一个留本地 |
| ④ | 改 `config.py` 指向它们，再 `python config.py` 自检 | — | 见 **§3** |

**§2.3 / §2.4 是「要求」** —— 你的导出物必须满足这些，用来判断"我导出的对不对"，
**不是**"怎么在 Vivado 里搭 BD"的教程。
**§2.6 是「核对」**（两段脚本 + 一条板上硬校验）。

下面每条事实都来自**对现有两个工程的实测**（解包 XSA 读 `.hwh`、逐字节比对
三份 `ps7_init.tcl`、板上读回 `MIO_PIN_*` 寄存器）。

### 2.2 ★★ 前提：PS 与 PL 来自**两个不同的工程** ★★

这套移植是把两个工程**交叉组合**起来用的：

| 哪一半 | 内容 | 来自哪个工程 | 落地成什么 |
|---|---|---|---|
| **PS 配置** | PLL / 时钟 / DDR / MIO 复用 / MIO bank 电压 | **`opjtmp`**（只要 PS、没有 PL） | `tmp-test/zynq/ps7_init_uart1.tcl`（**进仓库**） |
| **PL 比特流** | AXI GPIO → LED 流水灯 | **`AXI_GPIO_1`**（有 PL 设计） | `<工程>\hw\sdt\System_wrapper.bit`（只填进 `config.py`） |

**它们不是同一个 XSA**，所以**两边各有一个"看起来能用、其实不能用"的文件**：

| 陷阱文件 | 谁最容易拿到它 | 用了会怎样 |
|---|---|---|
| `opjtmp.xsa` 里的 `opxjtmp.bit` | "顺手把 XSA 里的 bit 烧了" | 它的 `.hwh` 里**只有 `processing_system7`、没有任何 PL IP** ⇒ PL 是空的，**LED 流水灯不亮** |
| `AXI_GPIO_1` 里的 `ps7_init.tcl` | "bit 就在这儿，ps7_init 也在旁边，一起拿" | 它的 BD 里 `PCW_EN_UART1 = 0` ⇒ **不把 MIO48/49 复用成 UART1** ⇒ 内核照常启动（心跳/MMU/LED 自检全对）但**串口一个字节都没有** —— 看起来就像"内核没跑起来" |

> ⚠⚠ **所以不要"顺着目录拿文件"。** 判断一个 `.bit` / `ps7_init.tcl` 能不能用，
> 要看**它来自哪个工程**，不要看它放在哪、叫什么名字、多大 ——
> 那两个 `.bit` 的大小只差 **2 字节**（`4,045,692` vs `4,045,690`）。

原理（也是"交叉组合"能成立的原因）：`ps7_init` 管 **PS**（时钟/MIO/DDR），
比特流管 **PL**，两者相互独立，所以可以来自不同工程。

### 2.3 要求一：PS 侧（`opjtmp` 那个工程）的 BD 必须长这样

| 项 | 必须是 | `.hwh` 里的参数名 | 实测值（`opjtmp.xsa`） |
|---|---|---|---|
| **UART1 使能** | ✅ 开 | `PCW_EN_UART1` / `PCW_UART1_PERIPHERAL_ENABLE` | `1` / `1` |
| **UART0** | 关掉（不用） | `PCW_EN_UART0` | `0` |
| **UART1 基地址** | `0xE0001000` | `PCW_UART1_BASEADDR` / `HIGHADDR` | `0xE0001000` / `0xE0001FFF` |
| **MIO48 = UART1 TX** | 复用给 UART1 | `PCW_MIO_TREE_PERIPHERALS` / `..._SIGNALS` | `MIO48 -> UART 1`、`tx` |
| **MIO49 = UART1 RX** | 复用给 UART1 | 同上 | `MIO49 -> UART 1`、`rx` |
| ★ **MIO bank 1 电压** | **LVCMOS 1.8V** | `PCW_PRESET_BANK1_VOLTAGE` | `LVCMOS 1.8V` |
| MIO bank 0 电压 | LVCMOS 3.3V | `PCW_PRESET_BANK0_VOLTAGE` | `LVCMOS 3.3V` |
| **不要有 PL IP** | 这个工程**只出 PS 配置**，PL 用另一个工程 | — | `.hwh` 里只有 `processing_system7` |

**bank 1 这一条最容易配错、也最难查**：本板（AC850-CORE + AC880-CB）的
`VCCIO_BANK1` 实际接的是 **1.8V**。如果导出的 XSA 把它声明成 3.3V，`ps7_init` 会给
MIO16-53 写下 `[11:9]=3`（LVCMOS33），于是 **MIO49（UART1 RX）的输入阈值变成约 2.0V，
1.8V 的高电平收不进来** —— 现象是"寄存器全对、但一个字节都收不到"，**没有任何报错**。
这个坑当期绕了一大圈（详见 `arch/arm32/README.md` §8 与计划文档坑表）。

> ★ **别被 XSA 里的波特率误导**：`PCW_UART1_BAUD_RATE` 写的是 `115200`，
> 但**实际用的是 9600** —— 波特率由**内核自己重编程**（还有闭环自校准，
> 报告里 `Baud : requested=... actual=... err=... ppm` 就是这件事）。
> `config.py` 里的 `SERIAL_BAUD` 是**主机这一侧**的值，两边都是 9600 才对得上。

### 2.4 要求二：PL 侧（`AXI_GPIO_1` 那个工程）必须长这样

PL 里只需要一个 **AXI GPIO** 直接驱动 LED：

| 项 | 必须是 | 出处 / 怎么认 |
|---|---|---|
| IP | `axi_gpio`（双通道、8 位、**无中断**） | `IS_DUAL=1`、`GPIO_WIDTH=8`、`INTERRUPT_PRESENT=0` |
| 基地址 | `0x41200000` – `0x4120FFFF` | 与内核侧 `XPAR_AXI_GPIO_0_BASEADDR` 必须一致 |
| 连接 | PS 的 `M_AXI_GP0` → AXI SmartConnect → `axi_gpio` | `.hwh` 里找得到 `smartconnect` |
| 复位 | `proc_sys_reset`（由 PS 的 `FCLK_RESET0_N` 驱动） | `.hwh` 里找得到 `proc_sys_reset` |
| PL→PS 中断 | **不需要**（GPIO 不产中断） | `INTERRUPT_PRESENT=0` |

**这条"必须一致"是硬要求**：内核侧的地址写在 `arch/arm32/board/xparameters.h` 里
（从 XSA 导出后**固化进仓库**）。地址对不上，内核就会去点一个不存在的寄存器。

> ⚠ **同配置的前提在这里有个推论**：既然是"同一套设计"，你导出的 `xparameters.h`
> 应当与仓库里那份 `arch/arm32/board/xparameters.h` **一致**。
> 不一致 = 设计变了 ⇒ 那要连带重新生成设备表
> （`python tools/gen_board_desc.py arch/arm32/board/xparameters.h --out-c ... --out-h ...`），
> **这已经超出本节范围**，别只改 `config.py`。

### 2.5 解包以后：哪个文件、放到哪

**XSA 其实是个 ZIP。** 解开以后按用途分三类：

| XSA 内的文件 | 是什么 | 要吗 |
|---|---|---|
| `*.hwh`（`design_1.hwh` / `System.hwh`） | **BD 描述（XML）**。判断"有没有 PL IP"就看它 | 用来核对（§2.6），不用拷 |
| `*.bit` | 比特流 | ✅ **要**（只从**有 PL 设计**那个工程拿） |
| `ps7_init.tcl` | PS 初始化（**Tcl 版**；JTAG 加载器用的就是这种） | ✅ **要**（只从**UART1 已使能**那个工程拿） |
| `ps7_init.c` / `.h` / `ps7_init_gpl.c` / `.h` | 同一份配置的 **C 版**（给 BSP 编译用） | ❌ 不用 |
| `ps7_init.html` | 寄存器说明（1–3 MB） | ❌ 不用 |
| `xsa.json` / `xsa.xml` / `sysdef.xml` / `*.bda` | 元数据 | ❌ 不用 |

Vitis 工程里的**平台导出目录**（`<工程>\hw\sdt\`）是 XSA 解开后的形态：

```
<工程>\hw\
├─ System_wrapper.xsa          ← 导出的 XSA（ZIP）
└─ sdt\                        ← 解开后的目录（Vitis 生成，给 BSP 用）
   ├─ System_wrapper.bit       ← ★ 这个才是 BITSTREAM
   ├─ ps7_init.tcl             ← ★ 就在旁边，但多半**不是**串口流程要的那份
   ├─ ps7_init.c / .h / _gpl.* ← C 版
   ├─ ps7_init.html            ← 寄存器说明
   ├─ pcw.dtsi / pl.dtsi / system-top.dts / zynq-7000.dtsi   ← 设备树
   ├─ include\                 ← BSP 头文件
   └─ .Xil\ 、extracted\        ← 中间产物，别管
```

**放到哪 / 填进哪 —— 就这一张表**：

| 文件 | 放到哪 | 然后改 `config.py` 里哪个 |
|---|---|---|
| 比特流（`System_wrapper.bit`） | **留在你自己的目录就行**（几 MB，**不要提交**） | `BITSTREAM` = 它的**绝对路径** |
| `ps7_init.tcl`（UART1 那份） | **放进仓库**：`tmp-test/zynq/ps7_init_uart1.tcl` | `PS7_INIT`（默认就指这里，通常不用改） |
| `xparameters.h` | 只在你**改了设计**时才动 `arch/arm32/board/`（见 §2.4 的提醒） | — |

放好之后：

```bash
python config.py          # 自检:逐项告诉你哪个路径不存在 / 哪个还是占位值
                          # 顺手刷新 tmp-test/jtag/paths.tcl(Tcl 脚本从那里取路径)
```

### 2.6 导出后怎么核对（三条）

**① 它有没有 PL 设计？**（没有就只能当 PS 配置用，别指望 LED）

```python
import re, sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
hwh = [n for n in z.namelist() if n.endswith(".hwh")]
print("hwh:", hwh)
text = z.read(hwh[0]).decode("utf-8", "replace")
print("PL IP:", sorted(set(re.findall(r'"(axi_gpio|smartconnect|proc_sys_reset)"', text))) or "★ 没有 PL IP")
```

**② PS 配置对不对？**（UART1 + bank1 电压 + MIO48/49 复用）

```python
import re, sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
text = z.read([n for n in z.namelist() if n.endswith(".hwh")][0]).decode("utf-8", "replace")
for k in ("PCW_EN_UART1", "PCW_PRESET_BANK1_VOLTAGE", "PCW_PRESET_BANK0_VOLTAGE"):
    m = re.search(rf'"{k}"\s+VALUE="([^"]*)"', text)
    print(f"{k:26} = {m.group(1) if m else '(缺)'}")
tree = re.search(r'"PCW_MIO_TREE_PERIPHERALS"\s+VALUE="([^"]*)"', text)
sig = re.search(r'"PCW_MIO_TREE_SIGNALS"\s+VALUE="([^"]*)"', text)
for i in (48, 49):
    per = tree.group(1).split("#")[i] if tree else "?"
    sg = sig.group(1).split("#")[i] if sig else "?"
    print(f"MIO{i} -> {per} / {sg}")
```

要看到 `PCW_EN_UART1 = 1`、`PCW_PRESET_BANK1_VOLTAGE = LVCMOS 1.8V`，
以及 `MIO48 -> UART 1 / tx`、`MIO49 -> UART 1 / rx`。

**实测对照**（这段就是 §2.2 那个坑的直接证据 —— 两个工程本来就不同用途）：

| XSA | `PCW_EN_UART1` | `MIO48` / `MIO49` |
|---|---|---|
| `opjtmp.xsa`（PS 配置源） | **`1`** | `UART 1 / tx`、`UART 1 / rx` |
| `System_wrapper.xsa`（PL 比特流源） | **`0`** | `unassigned / unassigned` |

**③ 上板后由加载器硬校验**（不靠人眼）：
`tmp-test/jtag/run_kernel_uart.tcl` 在 `ps7_init` 之后会数 MIO16-53 里
`[11:9] != 1`（即不是 LVCMOS18）的引脚，只要有一个就 **exit 1** 并列出具体引脚：

```
FAIL:有 38 个 MIO16-53 不是 LVCMOS18(bank 1 应为 1.8V):
    MIO16 = 0x00001600  [11:9]=3
    ...
  这是 XSA 的 PS7 配置问题,不是内核问题。请重新导出 XSA 并把
  MIO bank 1 的电压设成 1.8V。
```

**换完之后，用这三样确认换对了**（`python tmp-test/verify_board.py --load --seconds 75`）：

| 看什么 | 正确的值 |
|---|---|
| `MIO_PIN_48` / `MIO_PIN_49` | `0x000012E0` / `0x000012E1` |
| `UART1 MR` | 非 0（说明外设活了） |
| 心跳槽 `uartok` | `1` |

任一条不对，**先别怀疑内核**，回去查 XSA（就是 ①②③）。

> ⚠ **别把 `0x16E0/0x16E1` 当成"期望值"。** 那正是**旧的、bank1 声明成 3.3V 的 XSA**
> 产生的值（`[11:9] = 3`）；能用的值是 **`0x12E0/0x12E1`**（`[11:9] = 1`），
> 两者只差这一位。加载器脚本以前把这对**错值**印成"(期望 ...)"，已改正 ——
> 但真正该信的永远是 `uartok` 与那条 MIO bank1 硬校验，不是任何"期望"注释。

> 最后：**`BITSTREAM` 与 `PS7_INIT` 是各自独立的**。只换 PS 配置 ⇒ 不用动 `BITSTREAM`；
> 只重做 PL ⇒ 不用动 `PS7_INIT`。

---

## 3. ★ 第二步：改 `config.py` ★

打开仓库根目录的 `config.py`，只改这块：

| 常量 | 含义 | 去哪找 | 例子 |
|---|---|---|---|
| `VITIS_DIR` | Vitis / Vivado 安装目录 | 安装时选的路径 | `C:\AMDDesignTools\2025.2`（AMD 统一安装器）<br>`C:\Xilinx\Vitis\2025.2`（Xilinx 安装器） |
| `SERIAL_PORT` | 板子 PS UART1 接到 PC 的哪个串口 | Windows 设备管理器 → 端口 | `"COM4"`、`"COM7"`（Linux 上是 `/dev/ttyUSB0`） |
| `SERIAL_BAUD` | 波特率 | 固定是 9600 8N1 | `9600`（一般不用改） |
| `BITSTREAM` | PL 比特流（**必须是带 PL 设计那个工程**的，见 §2.2） | Vitis/Vivado 工程导出的目录 | `<你的平台>\hw\sdt\System_wrapper.bit` |
| `SCHEMATIC_PDF` | 原理图 PDF（可选） | 只有 `tmp-test/sch_render.py` 用 | 留空 `""` 表示不用 |

`VITIS_DIR` 会被派生出这些，**不用手写**：

```
VITIS_DIR/Vitis/bin/xsdb.bat                       (Windows；Linux 上是 .../xsdb)
VITIS_DIR/gnu/aarch32/nt/gcc-arm-none-eabi/bin/arm-none-eabi-gcc.exe
VITIS_DIR/gnu/aarch32/nt/gcc-arm-none-eabi/bin/arm-none-eabi-{objcopy,nm,objdump}.exe
```

### 3.1 自检（改完一定要跑）

```bash
python config.py
```

它逐项打印解析结果并标注 `[存在]` / `[不存在]` / `[★ 占位值,必须改 ★]`，最后：

* 全绿 → 顺手生成 `tmp-test/jtag/paths.tcl`（JTAG 的 Tcl 脚本从那里取路径）；
* 有红 → 逐条列出**哪一项**、**应该改成什么**，并给非零退出码。

有需要单独取值的地方（例如 `tmp-test/led/build.ps1`）：

```bash
python config.py --get toolchain.dir       # 也会打印其它键名
```

### 3.2 不想把自己的路径提交进 git

`config.py` 本身是**要提交**的，里面只有通用/占位值。如果你不想让本机路径进版本库：

```bash
cp config.example.py config.local.py
# 然后只改 config.local.py —— config.py 末尾会自动读它覆盖
```

`config.local.py` 已被 `.gitignore` 忽略。

### 3.3 Windows 上 `python` 打不开？

个别机器上 `python` 是 Microsoft Store 的占位程序。`tmp-test/led/build.ps1` 支持用环境变量指一个解释器：

```powershell
$env:PYTHON = "C:\path\to\python.exe"
pwsh -File tmp-test/led/build.ps1
```

---

## 4. 第三步：构建

```bash
python tools/gen_ninja.py --out build-arm.ninja --arch arm32
ninja -f build-arm.ninja arm32
```

产物 `out/kernel-arm.elf`（ELF32 / ARM / EABI5 / hard-float）。

编译标志（写死在 `tools/gen_ninja.py` 里，不需要改）：

```
-mcpu=cortex-a9 -marm -mfpu=vfpv3 -mfloat-abi=hard -mno-unaligned-access
-ffreestanding -nostdlib -nostdinc -fno-builtin -Wall -Wextra -Werror -O2 -std=gnu11
```

链接会带 `-lgcc`（ARM 没有硬件整数除法，`__aeabi_uidiv` 之类在 libgcc 里）。

### 4.1 编译器是怎么找到的

`tools/gen_ninja.py` 按这个顺序找 ARM 编译器：

1. 环境变量 `ARM_CC` / `ARM_OBJCOPY` / `ARM_LIBGCC`；
2. `PATH` 里的 `arm-none-eabi-gcc`；
3. **`config.py` 里的 `VITIS_DIR`**（再兜一个厂商默认位置 `C:\Xilinx\Vitis\2025.2\...`）。

三条都不成时会报错，并把 `config.py` 认为应该在的位置打出来。所以正常情况**你只需要保证第 3 条对**。

> ⚠ 在 `bin/` 里找 `arm-xilinx-eabi-gcc.exe` 是**找不到**的 —— 那个名字只出现在
> `--version` 的输出里。文件名是 `arm-none-eabi-gcc.exe`。

---

## 5. 第四步：上板

板级脚本都在 `tmp-test/` 下。它们**都会自己 import config**，不用传路径参数。

```bash
# 只读：加载内核、抓串口、解析 97 项自检报告，退出码即结论
python tmp-test/verify_board.py --load --seconds 75

# 读写：加载内核、发命令、校验回显（10 项）
python tmp-test/shell_test.py --load

# 只要原始串口输出（排查"波特率/线路/静默"三类问题）
python tmp-test/run_and_capture.py <串口> 9600 60
```

> ★ **`--seconds` 一定要给够（75 秒）。** 完整报告要 50 秒以上的内核运行时间，
> 而抓取窗口是从 JTAG 加载返回之后才起算的。这个分支里 `verify_board.py` 的
> **默认值还是 8 秒**，直接跑会截断在半行，报错是"找不到 `SELF-TEST BEGIN`"——
> 那句话看起来像内核没跑到自检阶段，其实只是抓取窗口太短。

判断依据是 `tmp-test/verify_board.py` 的输出：

```
SELF-TEST: 97 passed, 0 failed
全部通过
```

外加两条硬要求：串口里必须出现 `=== SELF-TEST BEGIN ===` / `END` 框，以及报告之后的
` Boot complete:` 签名（"报告全绿但整机中途崩掉"会被判失败）。

### 5.1 JTAG 的 Tcl 脚本

`tmp-test/jtag/run_kernel_uart.tcl`（串口流程用这个）不自己写路径，它 `source` 一个由
`config.py` 生成的小文件：

```
python config.py          # 生成/刷新 tmp-test/jtag/paths.tcl
```

改了 `config.py` 之后要重跑一次。`paths.tcl` 被 git 忽略（里面是本机路径）。

| Tcl | 用途 | 用的 ps7_init |
|---|---|---|
| `run_kernel_uart.tcl` | 串口流程（内核自检/命令通道） | **仓库里那份** `ps7_init_uart1.tcl`（已启用 UART1） |
| `run_kernel.tcl` | 只关心心跳、不用串口 | `AXI_GPIO_1` 那份（**不路由 UART1**） |
| `run_led.tcl` | 加载 `tmp-test/led/out/led.elf`（PL LED 演示） | 同上 |

> ★ **串口流程必须用仓库里那份 `ps7_init_uart1.tcl`。** `AXI_GPIO_1` 旁边那份
> `ps7_init.tcl` **不把 MIO48/49 配成 UART1**。误用它时的现象极具误导性：内核
> **照常启动**（心跳、MMU、LED 自检全对），但串口**一个字节都没有** ——
> 看起来就像"内核没跑起来"。`config.py` 里的 `PS7_INIT` 已经指向正确的那一份。
>
> ⚠ **注意：脚本目前**没有**"这份 ps7_init 是不是 UART1 版"的自检。**
> 它加载前只做一件事：`source` 那份文件、`ps7_init`、然后**硬校验 MIO bank1**
> （见 §2.6 第 ③ 条）。**串口对不对由 `uartok` 说了算** —— 拿错了不会当场报错，
> 只会静默。这条是已知的粗糙处，别指望脚本拦住你。

---

## 6. 这次改掉的硬编码路径（共 37 处 / 17 个文件）

改动原则：**脚本逻辑不变**，只是把写死的值换成 `config.X`。

### 6.1 加的两行

```python
sys.path.insert(0, str(ROOT))     # 仓库根不在 sys.path 里(脚本在 tmp-test/ 或 tools/ 下)
import config                     # noqa: E402
```

### 6.2 替换对照表

| 原来写死 | 现在从哪来 | 出现在 |
|---|---|---|
| `C:\AMDDesignTools\2025.2\Vitis\bin\xsdb.bat` | `config.XSDB` | 9 个 `tmp-test/*.py` |
| `"COM4"` | `config.SERIAL_PORT` | 8 个 `tmp-test/*.py` |
| `9600` | `config.SERIAL_BAUD` | 同上 |
| `...\gcc-arm-none-eabi\bin\arm-none-eabi-nm.exe` | `config.ARM_NM` | `spurious_probe.py` |
| `...\bin\arm-none-eabi-objdump.exe` | `config.ARM_OBJDUMP` | `exc_frame_probe.py` |
| `...\gcc-arm-none-eabi`（工具链根） | `config.ARM_TOOLCHAIN_DIR` | `tools/gen_ninja.py`、`exc_frame_probe.py` |
| `...\out\kernel-arm.elf` | `config.KERNEL_ELF` | Tcl（经 `paths.tcl`） |
| `...\tmp-test\led\out\led.elf` | `config.LED_ELF` | `run_led.tcl` |
| `...\System_wrapper.bit` | `config.BITSTREAM` | 3 个 Tcl |
| `...\ps7_init_uart1.tcl` | `config.PS7_INIT` | `run_kernel_uart.tcl` |
| `...\ps7_mio_bank1_check.tcl` | `config.PS7_MIO_CHECK` | `run_kernel_uart.tcl` |
| `D:\<资料盘>\...\原理图.pdf` | `config.SCHEMATIC_PDF` | `sch_render.py` |
| `C:\AMDDesignTools\2025.2\gnu\...`（PowerShell 里） | `config.py --get toolchain.dir` | `tmp-test/led/build.ps1` |

### 6.3 故意**没有**改的

| 位置 | 为什么 |
|---|---|
| `tools/ninja_build.py`、`tools/gen_ninja.py` 里的 `/home/leon/.rustup/...` | **上游原有**的示例文本（讲 Rust target 目录），不是我们的机器路径 |
| `tools/gen_ninja.py` 里的 `C:\Xilinx\Vitis\2025.2\...` | **厂商默认安装位置**（兜底用），不是某个人的路径；`config.py` 里的值优先 |
| `third_party/**`、`resources/**`、`boot/include/bootlib.h`、`driver/serial/serial_port.cpp`、`arch/arm32/include/arch/uart_ps.h` | 上游代码 / 二进制里的 `COM1` 是**协议里的设备名**，不是本机串口 |
| 文档里描述"我手上这台机器"的历史记录 | 那是记录，不是配置 |

---

## 7. 排错

| 现象 | 多半是 |
|---|---|
| `python config.py` 报 `jtag.xsdb 不存在` | `VITIS_DIR` 不对（注意有的安装是 `C:\Xilinx\Vitis\2025.2`） |
| 构建报 `ARM toolchain not found` | 同上；或想用别的工具链就设 `ARM_CC` |
| 上板后**串口一个字节都没有**，但 JTAG 读到心跳正常 | ① `ps7_init` 用错（那份不路由 UART1，见 §2.2/§2.5）—— 检查 `paths.tcl` 里的 `PS7`；② 串口号不对（`config.py` 的 `SERIAL_PORT`） |
| 加载器报 `FAIL:有 N 个 MIO16-53 不是 LVCMOS18` | **XSA 把 MIO bank1 声明成 3.3V 了**（§2.3 那条）—— 重新导出 XSA 并设成 1.8V；**这不是内核问题** |
| 上板后 LED 流水灯不亮，但串口/自检都正常 | 比特流拿错了（用了**没有 PL IP** 那个工程的 `.bit`，见 §2.2） |
| `MIO_PIN_48/49` 读到 `0x16E0/0x16E1` 而不是 `0x12E0/0x12E1` | 同上：`[11:9]` = 3 而不是 1，即 bank1 被声明成 3.3V |
| `验证失败: 串口输出里找不到 '=== SELF-TEST BEGIN ==='` 且日志断在半行 | `--seconds` 太短，用 75 |
| `FAIL: cannot find .../paths.tcl` | 还没跑过 `python config.py` |
| `ModuleNotFoundError: config` | 脚本在 `tmp-test/` 或 `tools/` 下，`sys.path` 那两行被删了 |
| 串口里中文是乱的 (`U+FFFD`) | 已知问题：`run_and_capture.py` 用 `ascii` 解码打印串口数据。**hexdump 那一段是无损的**，从那里能还原 |
| 板子像"变砖"、每次重新加载都崩 | 先断电重上电（PS 可能停在半配置状态） |

---

## 8. 这套东西验证到什么程度

诚实说明（**本移植几乎完全由 AI 生成，人只做方向与拍板；所有测试与调试由 AI 完成，
没有人逐行评审**）：

* 上面每一条命令都在**本机**（Windows + Vitis 2025.2 + 一块 AC880-CB）实测跑过：
  `python config.py` 全绿、构建通过、`verify_board.py` **97 passed / 0 failed**、
  `shell_test.py` **10/10**、`tmp-test/led/build.ps1` 构建通过；
* **没有**在第二台机器、**没有**在 Linux、**没有**换过 Vivado 工程导出目录验证过 ——
  "换机器只改 `config.py`"这件事在结构上成立（自检脚本会把每个缺项指出来），
  但只有一台机器的证据；
* 宿主单元测试基线是 **9 failed / 74 passed**，那 9 项预先存在、与本移植无关
  （busybox 合规 / 许可清单 / DMA 计划 / QEMU 固件），已在基点 `08e5c9c` 上复现过。
* **§2 的适用范围是「同一套设计、你自己的板」** —— 它讲的是「导出 → 解包 → 放路径 →
  改 `config.py`」，**不是**「怎么在 Vivado 里搭 BD」的教程；§2.3/§2.4 是**验收要求**。
  设计本身要改（换 IP / 改地址）会连带改 `arch/arm32/board/xparameters.h` 与设备表，
  **超出这份指南**。
* §2 里每条事实都来自**实测**（解包 `opjtmp.xsa` 与 `AXI_GPIO_1/System_wrapper.xsa` 读 `.hwh`、
  逐字节比对三份 `ps7_init.tcl`、板上读回 `MIO_PIN_48/49`），但我**没有从零走过一遍
  Vivado GUI**，也没在第二块板 / 第二个工程上验证过。§2.6 那两段 Python 在本机对这两个
  XSA 跑过；换 XSA 后请自己再跑一遍。
