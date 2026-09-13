# OpenXJ380 · Zynq-7020（ARMv7-A）移植分支

> ## 🤖 重要声明：本分支新增的内容**几乎完全由 AI 生成**
>
> **AI-GENERATED WORK —— READ THIS FIRST.**
>
> | | |
> |---|---|
> | **生成者** | **DeepSeek Harness**（agent 框架）+ **DeepSeek Flash 4.1**（模型） |
> | **AI 做了** | 本 fork 新增内容的**全部**：写码、调试、单元测试、上板验证、判据设计、文档 —— `arch/arm32/**`（69 个 C/汇编/链接脚本，约 **25,300 行**）、`tests/test_arm32_*.py`（16 个文件，约 **5,700 行**）、`tmp-test/**`（62 个文件，约 **5,400 行**）、本 fork 的文档（约 **7,500 行**） |
> | **人做了** | 给方向与需求、做取舍决策（例如"内核线程退出后不回收"）、在物理层面提供并连接开发板、按 AI 的指示执行上板加载 |
> | ★ **没有做** ★ | ★ **没有任何一行代码经过人类逐行评审** ★ |
>
> **因此，本分支的可信度不来自"人看过了"，只来自"板上判据"**：
> 97 项板级自检 + 8 组破坏性 A/B + 16 个纯逻辑层的宿主单元测试。
> ⚠ 而这些判据**同样是 AI 设计的** ⇒ 请把它们读作"**AI 自证**"，**不是第三方验证**。
> AI 会犯错：会写出看起来正确但判据写错的测试、会在注释里过度断言、
> 会把"修好 A 之后才暴露的旧 bug"当成新问题。已知的坑与未决项写在
> `docs/ZYNQ7020_PORT_PLAN.md` §0.5.5（**54 条**）与 §0.5.8，**但不保证列全了**。
>
> ⇒ **请按"未经人工评审的实验性 AI 生成代码"对待：不要用于任何真实用途。**
>
> ⚠ **上游 OpenXJ380（x86_64 部分）不是 AI 生成的**：那是人类作者的项目
> （见 [`LICENSE`](LICENSE) / [`CONTRIBUTOR.md`](CONTRIBUTOR.md)），本分支对它**一行未改**。
> 本声明只覆盖**本 fork 新增的部分**。
>
> ---
>
> ## ⚠️ 而且它**不完善、不可使用、正在开发中（WIP）**
>
> **This repository is a WORK IN PROGRESS. It is NOT complete and NOT usable as a product.**
>
> - 它是 [xingji-studio/OpenXJ380](https://github.com/xingji-studio/OpenXJ380)（x86_64 UEFI 内核）的一个 **fork**，
>   内容是"**把内核底座移植到 Xilinx Zynq-7020（双核 Cortex-A9 / ARMv7-A）**"的实验分支。
> - **没有用户态、没有系统调用、没有文件系统、没有 shell 程序、没有网络、没有 GUI**。
>   它**不是一个能跑应用的操作系统**，目前只是"内核 + 板上自检"。
> - **只在一台开发板（AC880-CB，XC7Z020）上验证过**，换板子基本不能直接跑。
> - **上游那套 x86_64 系统在本分支里一行未改，也没有被验证过**（本分支没有跑过上游构建/镜像/QEMU）。
> - 请当成**实验记录**；接口、行为、文档都可能随时改。
>
> 想知道"现在到底能做什么、不能做什么" ⇒ 先读 **[`docs/ZYNQ7020_PORT_STATUS.md`](docs/ZYNQ7020_PORT_STATUS.md)**。

---

## 一分钟速览

| 项 | 值 |
|---|---|
| 上游项目 | `https://github.com/xingji-studio/OpenXJ380`（Apache-2.0，x86_64 UEFI 内核） |
| 本分支 | `feat/zynq7020-arm-port`，基点 = 上游 `main` 的 **`08e5c9c`** |
| 目标硬件 | Xilinx **Zynq-7020**（XC7Z020，双核 Cortex-A9，ARMv7-A + VFPv3） |
| 当前状态 | **M0 – M4-11 已完成并板上验证**：板级自检 **97 项全绿**，**8 组破坏性 A/B 全部检出** |
| 目前能力 | JTAG 加载 `out/kernel-arm.elf` → UART 输出启动日志 + 97 项自检报告 + 串口命令通道（10/10） |
| 目前**不具备** | 用户态 / syscall / 文件系统 / 网络 / 块设备 / 模块加载 / 异常恢复 |
| 工具链 | Vitis 2025.2 的 `arm-xilinx-eabi-gcc` 13.3.0（**Windows**；Linux 未验证） |
| 上游代码改动 | **`kernel/`、`include/`、`driver/`、`user/`、`lib/`、`boot/`、`kmod/`、`resources/` = 0 个文件**（可机械复核，见下） |

---

## 与上游差在哪（可复核）

```bash
# 本分支相对上游基点的全部改动（数字截至本文写作时：155 个文件、+44494 / −4 行）
git diff --stat 08e5c9c HEAD

# 上游内核/用户态有没有被动过（答案：0 个文件）
git diff --name-only 08e5c9c HEAD -- kernel include driver user lib boot kmod resources
```

| 位置 | 性质 |
|---|---|
| `arch/arm32/**` | **全新**（70 个文件）：启动汇编、异常向量与帧、MMU/缓存、页分配器、内核堆、栈池 + guard page、调度器（纯逻辑层 + 内核侧）、互斥锁、SMP、GIC、UART、定时器、板级描述层、自检、串口 shell |
| `tests/test_arm32_*.py` | **全新**（16 个）：ARM 侧宿主单元测试（纯逻辑层在这里穷尽测） |
| `tmp-test/**` | **全新**（62 个）：板级验证脚本 + JTAG Tcl（加载、抓串口、解析报告、命令通道测试、故障注入） |
| `docs/ZYNQ7020_*.md`、`docs/PTASK.md` | **全新**：移植现状 / 集成路线 / 移植计划 / task 子系统专题分册 |
| `tools/gen_ninja.py` | **修改**（+261 / −4，纯新增）：加 `--arch arm32` 与独立的 ARM 构建图 |
| `tools/gen_board_desc.py` | **新增**（496 行）：从 XSA 导出的 `xparameters.h` 生成板级设备描述表 |
| `.gitignore` | **修改**（+11）：忽略 ARM 产物与调试日志 |
| **上游其余部分** | **一字未改**（`OLD_README.md` 就是上游 README 的逐字节备份） |

**为什么这么切**：移植遵循"**先在旁边把底座做起来，不碰上游**"。
好处是"哪些是移植新增的"永远可以用一条 `git diff` 回答；代价是**今天两边还没合流**
（怎么合流见 `docs/ZYNQ7020_INTEGRATION_PLAN.md`）。

---

## 构建（ARM 分支）

> ★ **换一台机器只需要改一个文件：仓库根目录的 [`config.py`](config.py)。**
> 完整步骤（要改哪几个值、去哪找、怎么自检、怎么排错）见
> **[`docs/BUILD_ARM32.md`](docs/BUILD_ARM32.md)**。★
>
> ★ **同一套设计换一块板：硬件那一步什么都不用做。** 仓库里已经带了
> **PL 比特流**与**两份 XSA**（[`arch/arm32/board/`](arch/arm32/board/README.md)），
> `ps7_init` 也在库里（`tmp-test/zynq/ps7_init_uart1.tcl`），`config.py` 默认就指着它们 ——
> clone 下来 `python config.py` 应当直接全绿。**只有换设计/换板时才需要自己导出**，
> 那一步见指南 **§2**。
>
> ⚠ 指南 §2 有一条防误导：**PS 配置（`ps7_init`）与 PL 比特流来自两个不同的工程**，
> 两边各有一个"看起来能用、其实不能用"的文件 —— 拿错了分别是
> "**串口一个字节都没有**"和"**LED 不亮**"，而且都不像配置问题。★

**环境**：Windows + Vitis 2025.2（自带 GNU ARM 工具链，文件名是 `arm-none-eabi-gcc.exe`；
`--version` 自报 `arm-xilinx-eabi-gcc` 13.3.0）+ Python 3 + Ninja。

```bash
python config.py                      # ★ 先改 config.py 顶部那段，再自检（会逐项指出缺什么）

# 生成 ARM 构建图（与上游 x86 图完全独立，互不影响）
python tools/gen_ninja.py --out build-arm.ninja --arch arm32

# 构建 → out/kernel-arm.elf（ELF32 / ARM / EABI5）
ninja -f build-arm.ninja arm32
```

编译选项与 AMD 官方 standalone BSP 对齐：

```
-mcpu=cortex-a9 -marm -mfpu=vfpv3 -mfloat-abi=hard -mno-unaligned-access
-ffreestanding -nostdlib -nostdinc -fno-builtin -Wall -Wextra -Werror -O2 -std=gnu11
```

> ⚠ 上游 x86_64 的构建流程**未受影响**（不带 `--arch` 仍是 `python3 tools/gen_ninja.py --out build.ninja`
> → `ninja -f build.ninja all` → `ninja -f build.ninja vdisk`），但**本分支没有验证过它** ——
> 上游构建/镜像/QEMU 的说明见 [`OLD_README.md`](OLD_README.md) 与 [`docs/BUILD.md`](docs/BUILD.md)。

---

## 上板与测试

| 层 | 命令 | 判据 |
|---|---|---|
| 宿主单元测试 | `python -m pytest tests/ -q` | **9 failed / 74 passed** —— 9 项**预先存在且与本移植无关**（busybox 合规/许可清单/DMA 计划/QEMU 固件） |
| 板级自检 | `python tmp-test/verify_board.py --load --seconds 75` | 97 项 `CHECK` 全绿 **+ 必须出现 ` Boot complete:`**（"报告全绿但整机中途崩掉"会被判失败）。★ `--seconds` **必须给够**：默认 8 秒会截断在半行，报"找不到 SELF-TEST BEGIN"（看着像内核坏了，其实只是窗口太短）★ |
| 串口命令通道 | `python tmp-test/shell_test.py --load` | **10/10** |
| 原始串口抓取 | `python tmp-test/run_and_capture.py <串口> 9600 60` | hex + ASCII 双份，用于排查"波特率/线路/静默"三类问题。★ hex 那份是无损的，正文那份会把非 ASCII 打成 `U+FFFD` ★ |
| 破坏性 A/B | 固件内置 8 组 | 搬帧 / VFP 现场 / 扫描唤醒 / 无饥饿 / 选核 / 不换栈 / 串口锁 / 线程退出 |

**两层分工（本项目最贵的一课）**：
**策略**（选取顺序、补偿、不变量）的判据在**宿主**（板上证明不了策略 —— 等权负载下任何策略都会通过）；
**机制**（切换、抢占、双核、锁、退出路径）的判据在**板上**，而且只认**破坏性 A/B**。

板级前提（换环境会踩）：PS UART1 @ `0xE0001000` / MIO48-49 / **9600 8N1** / 主机串口见 `config.py`；JTAG 用 Vitis `xsdb`；
PL LED 在 `0x41200000`；心跳区 OCM `0x00020000`（32 槽已满）。这些**因机器而异**的值全部集中在
[`config.py`](config.py)，改法与排错见 [`docs/BUILD_ARM32.md`](docs/BUILD_ARM32.md)。

---

## 文档索引（**先读哪个看这里**）

### 这个 fork 特有的（新增）

| 文件 | 回答什么问题 |
|---|---|
| ★ [`docs/ZYNQ7020_PORT_STATUS.md`](docs/ZYNQ7020_PORT_STATUS.md) ★ | **今天是什么状态**：能不能用、改了什么、怎么构建、怎么测、实现了什么、**明确没实现什么**、新增代码与上游代码的依赖关系 |
| ★ [`docs/ZYNQ7020_INTEGRATION_PLAN.md`](docs/ZYNQ7020_INTEGRATION_PLAN.md) ★ | **将来怎么与上游合流**：上游代码在**哪些层**会起作用、哪些 **ABI/API 保持不变**（复用或小改）、`arch/arm32` 怎么接进原有上层抽象、分阶段路线与待拍板决策 |
| [`docs/ZYNQ7020_PORT_PLAN.md`](docs/ZYNQ7020_PORT_PLAN.md) | **完整移植计划 + 逐阶段工作记录**（§0.5.1 一句话状态、§0.5.2 提交表、§0.5.5 坑表 54 条、§0.5.7 续接点、§0.5.8 未决项、§4.5/§4.6 分阶段） |
| [`docs/PTASK.md`](docs/PTASK.md) | **task 子系统专题分册**：源 OS 的调度/线程/生命周期事实（带 `文件:行号`）、与移植版的差异、"不回收"那次决定的完整推演 |
| [`arch/arm32/README.md`](arch/arm32/README.md) | **ARM 侧每个模块的设计与踩坑记录** + 退化/偏离清单（D1…D17） |
| [`OLD_README.md`](OLD_README.md) | **上游根 README 的逐字节备份**（本文件替换了它） |

### 上游原有的（本分支未改）

| 文件 | 内容 |
|---|---|
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | 上游架构边界与分层 |
| [`docs/BUILD.md`](docs/BUILD.md)、[`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md) | 上游构建/开发环境（Linux/WSL、QEMU、镜像） |
| [`AGENTS.md`](AGENTS.md) | 代码风格与工程规矩（4 空格、120 列、`#pragma once`…；本分支同样遵守） |
| [`CONTRIBUTOR.md`](CONTRIBUTOR.md)、[`LICENSE`](LICENSE)、[`LICENSES.md`](LICENSES.md)、[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) | 贡献者、许可与第三方组件声明 |
| `tests/`、`tmp-test/` | 宿主单元测试 / 板级验证脚本（见上表） |

---

## 许可

沿用上游 **Apache License 2.0**（见 [`LICENSE`](LICENSE)）。第三方组件的许可与分发要求见
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) 与 [`LICENSES.md`](LICENSES.md)。
本分支新增的代码（`arch/arm32/**`、`tests/test_arm32_*.py`、`tmp-test/**`、本 fork 的文档）
按同一许可发布；版权与来源说明保留在上游文件与 `THIRD_PARTY_NOTICES.md` 中。

---

## 反馈

- **上游 x86_64 的问题** → 提给 [上游仓库](https://github.com/xingji-studio/OpenXJ380)。
- **ARM 移植的问题** → 提到本 fork，并带上：板型号、`verify_board.py` 的完整串口输出
  （脚本会把原文存到 `verify_board_raw.txt`）、以及你是否改过 `arch/arm32/**`。
- ⚠ 再次提醒：这是**实验分支**，接口与行为不保证稳定；**不要用在任何真实用途上**。
