# Zynq-7020 (AC850-CORE + AC880-CB) 首板上板验证记录

**日期**：2026-05（对应 OpenXJ380 移植计划 M0/M1 的前置验证）
**目标**：在真实硬件上验证「工具链 → JTAG 直载 → 裸机程序运行 → LED 输出」整条链路，
为后续 OS 移植建立可复用的调试闭环。

---

## 1. 结论：全部打通

从**冷复位**开始的完整流程一次性成功，无需 SD 卡、无需 BOOT.BIN、无需 FSBL：

```
connect
rst -system
ps7_init / ps7_post_config      -> LVL_SHFTR_EN 由 0x0 变为 0x0000000F
fpga -f System_wrapper.bit
dow led.elf                     -> PC = 0x00100000
con
轮询心跳 -> 0x00020000 == 0x4C454431 ("LED1")
RESULT: RUNNING
```

运行期实测（间隔 2 秒两次采样）：

| 量 | 第 1 次 | 第 2 次 | 说明 |
|---|---|---|---|
| `tick` | 0x00000567 | 0x000005A1 | 在推进 → 程序活着 |
| 全局定时器 | 0x9625BB58 | 0xBF407E97 | Δ=689,673,023 |
| LED 图案 | 0x40 | 0x01 | 跑马灯在走 |
| AXI GPIO ch2 | 0x00000040 | 0x00000001 | 与 LED 图案一致，写入生效 |

**标定结果：Cortex-A9 全局定时器 ≈ 333.3 MHz（Δ/2s），推算 CPU ≈ 666.7 MHz。**
该值可直接用于 OS 移植时 `nanoTime()` 的实现。

---

## 2. 硬件事实（来自原理图 + 约束文件核实）

### 板卡
- 核心板：**AC850-CORE**，SoC = **xc7z020**（JTAG 已确认）
- 底板：**AC880-CB**
- DDR：**1 GB**，可用区间 `0x00100000` 起（`0x00100000 + 0x3FF00000`）
- OCM：`0x00000000` 起 192 KB（`0x0 + 0x30000`），镜像区 `0xFFFF0000`

### LED（PL 侧，8 个）

| LED | 引脚 | LED | 引脚 |
|---|---|---|---|
| LED0 | N22 | LED4 | P20 |
| LED1 | P22 | LED5 | P21 |
| LED2 | R18 | LED6 | R20 |
| LED3 | T18 | LED7 | R21 |

### AXI GPIO @ `0x41200000`（**双通道，各 8 位**）⚠️

| 通道 | 端口名 | 用途 | 引脚 |
|---|---|---|---|
| ch1 | `gpio_rtl_0` | **拨码开关**（输入） | H15 N18 N17 N20 N19 M20 M19 M16 |
| ch2 | `gpio_rtl_1` | **8 个 LED**（输出） | N22 P22 R18 T18 P20 P21 R20 R21 |

寄存器：

```
+0x00  ch1 DATA        +0x04  ch1 TRI
+0x08  ch2 DATA        +0x0C  ch2 TRI
```

- `TRI` 写 0 = 输出，写 1 = 输入；上电默认 `0xFFFFFFFF`（**输入**）。
- **ch1 必须保持输入**，否则会与拨码开关对顶。
- 8 位 GPIO 会把 `0xFFFFFFFF` 掩码成 `0x000000FF`（实测），属正常。

### 其他地址（来自导出的设备树）

| 外设 | 地址 |
|---|---|
| GIC Distributor / CPU I/F | `0xF8F01000` / `0xF8F00100` |
| PL310 L2 | `0xF8F02000` |
| Cortex-A9 全局定时器 | `0xF8F00200` |
| SLCR | `0xF8000000` |
| SLCR LVL_SHFTR_EN | `0xF8000900` |
| UART0 / UART1 | `0xE0000000` / `0xE0001000`（`stdout-path` = serial0:115200n8） |
| SD0 | `0xE0100000` |

---

## 3. 踩过的坑（对 OS 移植有直接参考价值）

### 3.1 `ps7_post_config` 不可省
复位后实测 `LVL_SHFTR_EN (0xF8000900) = 0x00000000`、`FPGA0_CLK_CTRL = 0x00101800`（CLKACT=0）。
**PL 电平转换器与 FCLK 均未使能**，此时访问 AXI GPIO 不会工作。
执行 `ps7_post_config` 后 `LVL_SHFTR_EN = 0x0000000F`。
→ 对应移植计划里「平台初始化必须显式完成，不能依赖复位默认值」。

### 3.2 双通道 GPIO 的方向必须区分
最初把两个通道都设为输出。查完整约束文件后发现 **ch1 接的是拨码开关**，
设成输出会与开关驱动对顶。已修正为 ch1 输入、ch2 输出。

### 3.3 自检延时导致固定等待误判
第一版脚本用固定 `after 3000` 读心跳，结果全 0，一度误判为「程序没跑」。
实际是程序开头的上电自检（4 段延时）耗时约 4~5 秒。
**改为轮询等待 magic 后一次通过。**
→ 上板验证脚本一律用「轮询状态位」而非「固定 sleep」。

### 3.4 ARM 链接必须显式 `-lgcc`
`-nostdlib` 不会自动带入 libgcc，而 ARM 无硬件整数除法，
32/64 位除法会产生 `__aeabi_uidiv` / `__aeabi_uldivmod`。
本项目 x86 侧用 `ld -T linker.ld --static` 不含 libgcc，搬到 ARM 会直接链接失败。
（详见 `docs/ZYNQ7020_PORT_PLAN.md` §2.15）

### 3.5 工具链
- 编译器：`C:\AMDDesignTools\2025.2\gnu\aarch32\nt\gcc-arm-none-eabi\bin\arm-none-eabi-gcc.exe`（GCC 13.3.0）
- libgcc：`...\aarch32-xilinx-eabi\usr\lib\arm-xilinx-eabi\13.3.0\libgcc.a`
- 编译选项：`-mcpu=cortex-a9 -marm -mfpu=vfpv3 -mfloat-abi=hard`
- 产物：`ELF32 / ARM / Version5 EABI / hard-float ABI`

---

## 4. 目录结构

```
tmp-test/
├── README.md              本文件
├── led/
│   ├── led.c              LED 测试程序（含开关读取与 OCM 心跳）
│   ├── start.S            裸机启动：设栈、清 BSS、跳 main
│   ├── link.ld            链接脚本（加载到 DDR 0x00100000）
│   ├── build.ps1          构建脚本
│   └── out/led.elf        产物
└── jtag/
    ├── probe.tcl          探测 JTAG 链路与目标
    ├── read_clocks.tcl    读 SLCR/PLL 寄存器
    ├── diag.tcl           下载后核心状态诊断（PC/CPSR/内存/GPIO）
    ├── run_led.tcl        ★ 完整加载运行流程
    └── verify.tcl         两次采样确认程序在推进
```

## 5. 如何复现

```powershell
# 构建
pwsh -File tmp-test/led/build.ps1

# 复用已运行的 hw_server（可选）
& 'C:\AMDDesignTools\2025.2\Vitis\bin\hw_server.bat'

# 一键上板运行
& 'C:\AMDDesignTools\2025.2\Vitis\bin\xsdb.bat' -no-ini tmp-test\jtag\run_led.tcl

# 确认存活（两次采样对比）
& 'C:\AMDDesignTools\2025.2\Vitis\bin\xsdb.bat' -no-ini tmp-test\jtag\verify.tcl
```

**硬件前提**：板子处于 **JTAG 启动模式**（BootROM 会等待 JTAG 命令）。
当前实测 `BOOT_MODE (0xF800025C) = 0x00000000`。

---

## 6. 对 OS 移植的直接意义

本次验证证明移植计划中 M1 阶段的关键前提全部成立：

1. **工具链可用**：Vitis 的独立 `arm-none-eabi-gcc` 能编出可运行的 Cortex-A9 裸机程序（hard-float）。
2. **调试闭环可用**：JTAG 直载 + OCM 心跳，**不需要串口、不需要 SD 卡**就能获得程序状态反馈。
   这比计划里设想的「M1 先做串口」更快——串口可以留到后面。
3. **平台初始化路径明确**：`ps7_init` + `ps7_post_config` 的来源与必要性已确认，
   对应计划 §2.2 的「用静态板级描述符替代 ACPI」——现在有了可用的参考实现。
4. **关键硬件事实已获取**：CPU/定时器频率、DDR 布局、外设基址、INTID 相关寄存器。
5. **缓存/勘误仍待处理**：本次是纯 PS 侧 + AXI GPIO 访问，未涉及 DMA，
   所以还没有触碰缓存一致性问题（计划 §2.11 / §2.15 的内容仍然必要）。
