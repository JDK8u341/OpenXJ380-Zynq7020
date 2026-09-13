# 编译与上板指南（Zynq-7020 / ARM32 分支）

> 这份文档只讲**这一支**（`arch/arm32`，Zynq-7020 上的 Cortex-A9）。
> 上游 x86_64 的构建没被改动，用法见 `OLD_README.md` / `docs/BUILD.md`。
>
> **换一台机器要改的东西只有一个文件：仓库根目录的 `config.py`。**
> 其余脚本都只是 `import config`，逻辑没有变。

---

## 0. 一句话流程

```bash
# ① 改 config.py 顶部那段 → ② 自检 → ③ 构建 → ④ 上板
python config.py                       # 自检 + 生成 Tcl 要用的 paths.tcl
python tools/gen_ninja.py --out build-arm.ninja --arch arm32
ninja -f build-arm.ninja arm32         # 产物 out/kernel-arm.elf
python tmp-test/verify_board.py --load --seconds 75
```

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
| 平台导出 | 一个 Vivado 导出的目录，里面有 `hw/sdt/System_wrapper.bit` | 见下面 `BITSTREAM` |

> `xsdb` 的用法：加载 PS 配置、烧比特流、下载 ELF、读寄存器，全部走 JTAG。

---

## 2. ★ 第一步：改 `config.py` ★

打开仓库根目录的 `config.py`，只改这块：

| 常量 | 含义 | 去哪找 | 例子 |
|---|---|---|---|
| `VITIS_DIR` | Vitis / Vivado 安装目录 | 安装时选的路径 | `C:\AMDDesignTools\2025.2`（AMD 统一安装器）<br>`C:\Xilinx\Vitis\2025.2`（Xilinx 安装器） |
| `SERIAL_PORT` | 板子 PS UART1 接到 PC 的哪个串口 | Windows 设备管理器 → 端口 | `"COM4"`、`"COM7"`（Linux 上是 `/dev/ttyUSB0`） |
| `SERIAL_BAUD` | 波特率 | 固定是 9600 8N1 | `9600`（一般不用改） |
| `BITSTREAM` | PL 比特流 | Vitis/Vivado 工程导出的目录 | `<你的平台>\hw\sdt\System_wrapper.bit` |
| `SCHEMATIC_PDF` | 原理图 PDF（可选） | 只有 `tmp-test/sch_render.py` 用 | 留空 `""` 表示不用 |

`VITIS_DIR` 会被派生出这些，**不用手写**：

```
VITIS_DIR/Vitis/bin/xsdb.bat                       (Windows；Linux 上是 .../xsdb)
VITIS_DIR/gnu/aarch32/nt/gcc-arm-none-eabi/bin/arm-none-eabi-gcc.exe
VITIS_DIR/gnu/aarch32/nt/gcc-arm-none-eabi/bin/arm-none-eabi-{objcopy,nm,objdump}.exe
```

### 2.1 自检（改完一定要跑）

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

### 2.2 不想把自己的路径提交进 git

`config.py` 本身是**要提交**的，里面只有通用/占位值。如果你不想让本机路径进版本库：

```bash
cp config.example.py config.local.py
# 然后只改 config.local.py —— config.py 末尾会自动读它覆盖
```

`config.local.py` 已被 `.gitignore` 忽略。

### 2.3 Windows 上 `python` 打不开？

个别机器上 `python` 是 Microsoft Store 的占位程序。`tmp-test/led/build.ps1` 支持用环境变量指一个解释器：

```powershell
$env:PYTHON = "C:\path\to\python.exe"
pwsh -File tmp-test/led/build.ps1
```

---

## 3. 第二步：构建

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

### 3.1 编译器是怎么找到的

`tools/gen_ninja.py` 按这个顺序找 ARM 编译器：

1. 环境变量 `ARM_CC` / `ARM_OBJCOPY` / `ARM_LIBGCC`；
2. `PATH` 里的 `arm-none-eabi-gcc`；
3. **`config.py` 里的 `VITIS_DIR`**（再兜一个厂商默认位置 `C:\Xilinx\Vitis\2025.2\...`）。

三条都不成时会报错，并把 `config.py` 认为应该在的位置打出来。所以正常情况**你只需要保证第 3 条对**。

> ⚠ 在 `bin/` 里找 `arm-xilinx-eabi-gcc.exe` 是**找不到**的 —— 那个名字只出现在
> `--version` 的输出里。文件名是 `arm-none-eabi-gcc.exe`。

---

## 4. 第三步：上板

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

### 4.1 JTAG 的 Tcl 脚本

`tmp-test/jtag/run_kernel_uart.tcl`（串口流程用这个）不自己写路径，它 `source` 一个由
`config.py` 生成的小文件：

```
python config.py          # 生成/刷新 tmp-test/jtag/paths.tcl
```

改了 `config.py` 之后要重跑一次。`paths.tcl` 被 git 忽略（里面是本机路径）。

| Tcl | 用途 | 用的 ps7_init |
|---|---|---|
| `run_kernel_uart.tcl` | 串口流程（内核自检/命令通道） | **仓库里那份**，已启用 UART1 |
| `run_kernel.tcl` | 只关心心跳、不用串口 | 平台自带那份 |
| `run_led.tcl` | 加载 `tmp-test/led/out/led.elf`（PL LED 演示） | 平台自带那份 |

> ★ **串口流程必须用仓库里那份 `ps7_init_uart1.tcl`。** 平台自带的 `ps7_init.tcl`
> **不把 MIO48/49 配成 UART1**。误用它时的现象极具误导性：内核**照常启动**
> （心跳、MMU、LED 自检全对），但串口**一个字节都没有** —— 看起来就像"内核没跑起来"。
> `config.py` 里的 `PS7_INIT` 已经指向正确的那一份；`run_kernel_uart.tcl` 加载前
> 还会检查文件身份，不对就 `FAIL` 退出。

---

## 5. 这次改掉的硬编码路径（共 37 处 / 17 个文件）

改动原则：**脚本逻辑不变**，只是把写死的值换成 `config.X`。

### 5.1 加的两行

```python
sys.path.insert(0, str(ROOT))     # 仓库根不在 sys.path 里(脚本在 tmp-test/ 或 tools/ 下)
import config                     # noqa: E402
```

### 5.2 替换对照表

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
| `D:\BaiduNetdiskDownload\...\原理图.pdf` | `config.SCHEMATIC_PDF` | `sch_render.py` |
| `C:\AMDDesignTools\2025.2\gnu\...`（PowerShell 里） | `config.py --get toolchain.dir` | `tmp-test/led/build.ps1` |

### 5.3 故意**没有**改的

| 位置 | 为什么 |
|---|---|
| `tools/ninja_build.py`、`tools/gen_ninja.py` 里的 `/home/leon/.rustup/...` | **上游原有**的示例文本（讲 Rust target 目录），不是我们的机器路径 |
| `tools/gen_ninja.py` 里的 `C:\Xilinx\Vitis\2025.2\...` | **厂商默认安装位置**（兜底用），不是某个人的路径；`config.py` 里的值优先 |
| `third_party/**`、`resources/**`、`boot/include/bootlib.h`、`driver/serial/serial_port.cpp`、`arch/arm32/include/arch/uart_ps.h` | 上游代码 / 二进制里的 `COM1` 是**协议里的设备名**，不是本机串口 |
| 文档里描述"我手上这台机器"的历史记录 | 那是记录，不是配置 |

---

## 6. 排错

| 现象 | 多半是 |
|---|---|
| `python config.py` 报 `jtag.xsdb 不存在` | `VITIS_DIR` 不对（注意有的安装是 `C:\Xilinx\Vitis\2025.2`） |
| 构建报 `ARM toolchain not found` | 同上；或想用别的工具链就设 `ARM_CC` |
| 上板后**串口一个字节都没有**，但 JTAG 读到心跳正常 | `ps7_init` 用错（平台那份不路由 UART1）—— 用 `run_kernel_uart.tcl`，或检查 `paths.tcl` 里的 `PS7` |
| `验证失败: 串口输出里找不到 '=== SELF-TEST BEGIN ==='` 且日志断在半行 | `--seconds` 太短，用 75 |
| `FAIL: cannot find .../paths.tcl` | 还没跑过 `python config.py` |
| `ModuleNotFoundError: config` | 脚本在 `tmp-test/` 或 `tools/` 下，`sys.path` 那两行被删了 |
| 串口里中文是乱的 (`U+FFFD`) | 已知问题：`run_and_capture.py` 用 `ascii` 解码打印串口数据。**hexdump 那一段是无损的**，从那里能还原 |
| 板子像"变砖"、每次重新加载都崩 | 先断电重上电（PS 可能停在半配置状态） |

---

## 7. 这套东西验证到什么程度

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
