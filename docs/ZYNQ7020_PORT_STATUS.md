# Zynq-7020（ARMv7-A）移植分支：现状、改动范围与"不可用"声明

> **本文的读者**：拿到这个 fork 仓库、想知道"这到底是什么、能不能用、跑到哪一步了、
> 与上游 `OpenXJ380` 差在哪"的人。
>
> **相关文档**（索引也见仓库根 `README.md`）：
> - `docs/ZYNQ7020_INTEGRATION_PLAN.md` —— ★ **未来怎么与上游代码合流**（哪些层复用、
>   哪些 ABI/API 保持不变、`arch/arm32` 怎么接进原有上层抽象）★
> - `docs/ZYNQ7020_PORT_PLAN.md` —— 移植计划与**逐阶段工作记录**（含 §0.5.2 提交表、
>   §0.5.5 坑表）
> - `docs/PTASK.md` —— task 子系统（调度/线程/生命周期）专题分册
> - `arch/arm32/README.md` —— ARM 侧每个模块的设计与踩坑记录
> - `OLD_README.md` —— 本文件替换掉的上游根 README 原文（备份）

---

## 0. ★★ 先说清楚：这个仓库处于什么状态 ★★

> ### 🤖 AI 生成声明（与根 `README.md` 同一份声明）
>
> 本 fork **新增的全部内容**（`arch/arm32/**` 约 25,300 行、`tests/test_arm32_*.py` 约 5,700 行、
> `tmp-test/**` 约 5,400 行、本 fork 的文档约 7,500 行）由
> **DeepSeek Harness + DeepSeek Flash 4.1** 生成 —— **写码、调试、单元测试、上板验证、
> 判据设计与文档全部由 AI 完成**；人类只负责方向、取舍决策与硬件。
> ★ **没有任何一行代码经过人类逐行评审** ★，而且**判据本身也是 AI 设计的** ⇒
> 本文里所有"已验证"都应读作"**AI 自证**"，不是第三方验证。
> 上游 OpenXJ380（x86_64）**不是** AI 生成的，本分支对它一行未改。

**WIP / 开发中 / 不完善 / 不可作为产品使用。**

| | |
|---|---|
| **能做什么** | 在一块 **Zynq-7020 开发板**上，通过 JTAG 加载 `out/kernel-arm.elf`，从 UART 看到启动日志与一份 **97 项板级自检报告**（当前全绿），以及一个串口命令通道 |
| **不能做什么** | **没有用户态、没有系统调用层、没有文件系统、没有 shell 程序、没有网络、没有块设备驱动、没有 GUI**。它**不是一个能跑应用的操作系统**，只是一份"内核底座 + 自检" |
| **目标硬件** | **只有**本移植作者手上的两块板（AC880-CB / AC850-CORE，XC7Z020）。**换板子基本不能直接跑**（时钟、MIO 电压、DDR 参数、PL 比特流、JTAG 脚本都是按这两块板配的） |
| **目标工具链** | Vitis 2025.2 自带的 `arm-xilinx-eabi-gcc`（GCC 13.3.0）**在 Windows 上**；构建脚本里有 Windows 路径假设。没有在 Linux 上验证过 |
| **上游代码** | `kernel/`、`include/`、`driver/`、`user/`、`lib/`、`boot/`、`kmod/`、`resources/` **一行未改**（可机械复核，见 §2），所以**上游那套 x86_64 系统在这个分支里既没被删、也没被适配**；本分支**没有**验证过上游构建是否可用 |
| **稳定性** | 自检全绿 ≠ 稳定。它没有内存保护的用户态、没有异常恢复（除诊断停机）、没有中断嵌套；已知未决项见 `docs/ZYNQ7020_PORT_PLAN.md` §0.5.8 |

**一句话**：这是一次"**内核底座移植**"的中间产物，用来证明"源 OS 的调度/线程/锁语义
能在 Cortex-A9 上以同样的形状跑起来"。**请当成实验记录，不要当成可用的系统。**

---

## 1. 这个仓库与上游的关系

| 项 | 值 |
|---|---|
| 上游 | `https://github.com/xingji-studio/OpenXJ380`（Apache-2.0，x86_64 UEFI 内核） |
| 本分支基点 | 上游 `main` = **`08e5c9c`**（提交日期 2026-09-04；2026-09-13 复核时上游 `main` 仍停在这里） |
| 本分支 | `feat/zynq7020-arm-port`（**不是** `main`；按项目规矩不直接推 `main`） |
| 本分支相对基点的规模 | 截至写这份文档时：**155 个文件、+44494 / −4 行、101 个提交**（数字会随每次提交变化，请以 §2 的复核命令为准） |
| 许可 | 沿用上游 **Apache License 2.0**；第三方组件声明见 `THIRD_PARTY_NOTICES.md` 与 `LICENSES.md` |

⚠ 说明：上游公开仓库是**整库重新上传**的（维护者在 issue #16 里自述"新建的开源仓库，
所有源码都是重新上传的，故没有完整的 commit 历史记录"），所以本分支的"基点"
是上游的**当前快照**，而不是某个历史版本。

---

## 2. 相对上游：到底改了什么（可复核）

```bash
# 1) 本分支 vs 上游基点的全部改动
git diff --stat 08e5c9c HEAD

# 2) 上游那些目录有没有被动过（答案：0 个文件）
git diff --name-only 08e5c9c HEAD -- kernel include driver user lib boot kmod resources

# 3) 唯一被修改的上游文件（+261 / −4 行，全部是"新增一条 ARM 构建图"的加法）
git diff 08e5c9c HEAD -- tools/gen_ninja.py
```

| 位置 | 性质 | 内容 |
|---|---|---|
| `arch/arm32/**` | **全新**（70 个文件） | 整个 ARM 侧：启动汇编、异常向量与帧、MMU/缓存、页分配器、内核堆、栈池 + guard page、调度器（纯逻辑层 + 内核侧）、互斥锁、SMP、GIC、UART、定时器、板级描述层、自检与串口 shell |
| `tests/test_arm32_*.py` | **全新**（16 个文件） | ARM 侧的宿主单元测试（纯逻辑层在这里穷尽测） |
| `tmp-test/**` | **全新**（62 个文件） | 板级验证脚本（串口抓取、报告解析、命令通道测试、JTAG Tcl、故障注入） |
| `docs/ZYNQ7020_PORT_PLAN.md`、`docs/PTASK.md` | **全新** | 移植计划与 task 子系统专题分册 |
| `tools/gen_ninja.py` | **修改**（+261 / −4） | 新增 `--arch arm32` 与 `arm32_graph()`：独立的 ARM 工具链解析、编译/汇编/链接规则、产物 `out/kernel-arm.elf`。改动是**纯新增**（4 处删除行是帮助文本与一条规则签名的同义替换） |
| `tools/gen_board_desc.py` | **新增**（496 行） | 从 Vitis 导出的 `xparameters.h` 生成板级设备描述表 `plat_device_t[]`（"单一真值来源 = XSA"） |
| `.gitignore` | **修改**（+11） | 忽略 ARM 构建产物与板级调试日志 |
| **`kernel/`、`include/`、`driver/`、`user/`、`lib/`、`boot/`、`kmod/`、`resources/`** | **0 个文件改动** | 上游内核与用户态**一字未改** |

**为什么这样切分**：移植的规矩是"**先在旁边把底座做起来，不碰上游**"。
好处是任何时候都可以 `git diff 08e5c9c -- kernel include` 得到空输出，
把"哪些是移植新增的"这件事变成一条**机械判据**，而不是靠人回忆。
代价见 §4（现在两边还没有合流）。

---

## 3. 目前是否集成 / 适配？

**没有集成。** ARM 侧现在是**完全独立的一套构建与运行实体**：

| 维度 | 现状 |
|---|---|
| 构建图 | 独立。`tools/gen_ninja.py --arch arm32` 生成 `build-arm.ninja`，与上游的 `build.ninja` **不共享任何编译规则**（`arm32_graph()` 里写死了这条理由） |
| 源文件 | **零共享**：ARM 目标只编译 `arch/arm32/src/*.c`（**31 个**）与 `arch/arm32/boot/*.S`（**3 个**），**不编译任何上游 `.c/.cpp/.S`** |
| 头文件 | **零共享**：ARM 侧的 `-I` 只有 `arch/arm32/include`，并且是 `-nostdinc -ffreestanding`；连 `errno` 都是**副本**（`arch/arm32/include/arch/errno.h`，靠一个逐值比对测试钉住与上游一致） |
| 链接 | 独立 ELF `out/kernel-arm.elf`，自带 `arch/arm32/boot/kernel.ld`；**不与 `kernel.krl`、`xapi`、`shell.elf` 链接** |
| 启动路径 | 独立：JTAG 直载 + 板级 `ps7_init`（不是上游的 UEFI `BOOTX64.efi`） |
| 运行形态 | 只有内核 + 串口自检 + 命令通道；**没有任何用户态** |

⇒ 结论：**现在是"并行的一棵子树"，不是"上游的一个新架构后端"。**
把它变成后者的路线写在 `docs/ZYNQ7020_INTEGRATION_PLAN.md`。

---

## 4. 新增代码与上游代码的依赖关系（这一节最容易被误解）

### 4.1 代码层面：**零依赖**

- ARM 目标的编译单元只有 `arch/arm32/**`（见 §3 的复核方式）；
- 它**不 `#include` 任何上游头文件**（上游 `include/` 不在 ARM 的 `-I` 里）；
- 它**不引用任何上游符号**（没有链接上游 `.o`/`.a`，唯一的外部库是工具链自带的 `libgcc`）；
- 上游的 `liballoc-x86_64.a`、`third_party/**`、`kmod/**` 在 ARM 构建里完全不参与。

**因此：你可以把 `arch/arm32/**` 整目录拷到另一个仓库、只带去 `tools/gen_ninja.py`
里那段 `arm32_graph()` 与 `tools/gen_board_desc.py`，它照样能构建。**
（这不是设计目标，只是"零依赖"的必然结果。）

### 4.2 语义层面：**强依赖**（照抄源 OS，并用测试钉住）

ARM 侧没有发明自己的调度/线程/锁语义，而是**逐条照抄上游**，并用两类手段把
"抄对了"变成可检查的东西：**宿主单元测试**（纯逻辑层）与**板级判据**（机制）。

| 照抄的东西 | 上游出处 | 我们怎么钉住它 |
|---|---|---|
| 调度选取语义（FIFO 名册 + 全表扫描 + `avg_vruntime` 闸门 + fallback + idle 兜底；**唤醒发生在扫描里**） | `kernel/task/scheduler.cpp` | `tests/test_arm32_sched.py`（含"无饥饿"穷尽扫 K=2..8）+ 板上相 5 |
| 常量（时间片 4 tick、`WAKEUP_CREDIT` 4ms、`SLEEPER_CREDIT` 8ms、权重 1024…） | `scheduler.cpp:17-24` | 头文件逐个照抄 + 宿主逐值断言 |
| `vruntime_delta()` 是**恒等式**（源 OS 没有权重） | `scheduler.cpp:232` | 照抄，**不擅自补全**，并记进退化清单 |
| `TaskLevel` 语义（`KERNEL 0 / IDLE 1 / APPLICATION 2`）与"应用级永远落 CPU0" | `include/task/pcb.h`、`scheduler.cpp:537-549` | 板上相 6 造一个应用级线程验它 |
| 互斥锁语义（**yield 型**、递归计数、`-EDEADLK`/`-EPERM`/`-EBUSY`/`-EINVAL`、`trylock` 不让出） | `kernel/task/mutex.cpp` | `tests/test_arm32_mutex.py`（12 节） |
| TCB 字段含义与"队列是全部线程的名册"模型 | `include/task/pcb.h`、`scheduler.cpp` | `arch/arm32/include/arch/tcb.h` 每个字段带上游行号 + `tests/test_arm32_tcb.py`（布局静态断言） |
| `spin_lock` 的 **irqsave 契约** | `include/cpu/lock.h` | `arch/arm32/include/arch/cpu.h` 同名契约 + 板上双核自检 |
| `errno` 数值 | `include/errno.h` | ARM 侧副本 + **逐值比对测试**（`tests/test_arm32_krlibc.py` 解析两边文件比对） |
| 内核线程栈大小 1 MiB | `include/proto.hpp` / `pcb.cpp` | 栈池按同一尺寸开（`KSTACK_STACK_PAGES=256`） |
| `wfi`/"永久停住"的语义（`while(true) hlt`） | `pcb.cpp:501-504` | 退出路径照它的形状实现（`DEATH` + 让出 + `wfi`） |

⇒ **准确的说法**：代码上零依赖，**契约上强依赖**。
将来合流时（`docs/ZYNQ7020_INTEGRATION_PLAN.md`），这些契约就是"两边能接上"的接口面。

---

## 5. 目前的编译流程（照抄即可）

**环境**：Windows + Vitis 2025.2（自带 `arm-xilinx-eabi-gcc` 13.3.0）+ Python 3 + Ninja。
（Linux 未验证；Linux 上需要自己指向一套 `arm-none-eabi` 工具链并改 `resolve_arm_toolchain()`。）

```bash
# 1) 生成 ARM 构建图（与上游 x86 图完全独立，互不影响）
python tools/gen_ninja.py --out build-arm.ninja --arch arm32

# 2) 构建
ninja -f build-arm.ninja arm32          # 或 ninja -f build-arm.ninja kernel.arm

# 3) 产物
out/kernel-arm.elf                      # ELF32 / ARM / EABI5
```

编译选项（`tools/gen_ninja.py` 的 `ARM32_*_FLAGS`，与 AMD 官方 standalone BSP 对齐）：

```
-mcpu=cortex-a9 -marm -mfpu=vfpv3 -mfloat-abi=hard -mno-unaligned-access
-ffreestanding -nostdlib -nostdinc -fno-builtin -Wall -Wextra -Werror -O2 -std=gnu11
```

**上板**（JTAG + 串口，9600 8N1）：

```bash
python tmp-test/verify_board.py --load            # 加载 + 抓串口 + 解析自检报告，退出码即结论
python tmp-test/shell_test.py  --load            # 串口命令通道 10 项
python tmp-test/run_and_capture.py COM4 9600 60  # 只抓原始串口（hex + ASCII）
```

**上游 x86_64 构建未受影响，但本分支没有验证过它**：
`python3 tools/gen_ninja.py --out build.ninja`（不带 `--arch`）仍是上游流程，
用法见 `OLD_README.md`、`docs/BUILD.md`、`docs/DEVELOPMENT.md`。

---

## 6. 目前有的测试

### 6.1 宿主（`python -m pytest tests/ -q`）

| | |
|---|---|
| 结果 | **9 failed / 74 passed** —— 9 项**预先存在**且与 ARM 移植无关（busybox 合规、许可清单、DMA 计划、QEMU 固件回退；在本分支基点上就是红的） |
| ARM 相关 | `tests/test_arm32_*.py` 共 **16 个文件**：缓存、控制台、堆、krlibc/errno、栈池、MMU、**互斥锁**、页分配器、每核数据、板级描述、**调度策略**、串口命令解析、任务上下文、TCB 布局、UART 波特率、vmap |

分工（这条分工是本项目最贵的一课）：

> **纯逻辑层（无 MMIO / CP15 / 内联汇编）的判据在宿主**，因为"等权 + 纯占用负载下
> 任何策略都会通过"—— 板上证明不了策略；**机制**（切换、抢占、无饥饿、锁、双核）
> 的判据在板上。

### 6.2 板上

| 层 | 工具 | 判据 |
|---|---|---|
| 自检报告 | `tmp-test/verify_board.py` | 97 项 `CHECK`，**外加一条硬要求**：串口里必须出现 ` Boot complete:` 签名（报告全绿但整机在中途崩掉 ⇒ 判失败，见计划坑 53） |
| 命令通道 | `tmp-test/shell_test.py` | 10/10（`help`/`ver`/`uptime`/`dump`/`probe`/`selftest`/`peek`/未知命令/前缀/空行） |
| 破坏性 A/B | 固件内 8 组 | 搬帧、VFP 现场、扫描唤醒、无饥饿、选核、不换栈、**串口锁**、**线程退出** —— 每组都"故意关掉一件事，看判据是否检出" |
| 故障注入 | `tmp-test/fault_test.h` + JTAG 选择器 | Data Abort / Undefined / Prefetch Abort 的诊断路径 |

**证据等级**（本项目对"证明了什么"的规矩）：
① 软件自报 → ② 硬件读回（JTAG/OCM 心跳）→ ③ **破坏性 A/B**（只有它能证明"这件事承重"）。

---

## 7. 已经实现的功能（M0 – M4-11）

| 阶段 | 内容 | 板上状态 |
|---|---|---|
| M0–M1 | 工具链/JTAG 直载/串口闭环自标定/OCM 心跳/中断与 GIC | ✅ |
| M2 | MMU（段 + 4KB 小页细粒度映射）、L1/L2 缓存与维护、PL310 | ✅ |
| M3 | 板级描述层（从 XSA 生成）、AXI GPIO/LED、**串口命令通道** | ✅ |
| AM3 | **双核**（CPU1 启动、每核栈、SGI/IPI、原子自旋锁契约） | ✅ |
| M4-1…M4-5 | krlibc 子集、物理页分配器、内核堆、vmap、**内核栈池 + guard page** | ✅ |
| M4-6/M4-7 | 寄存器帧、异常入口（帧建在任务栈上）、协作式切换 | ✅ |
| M4-8 | **调度器**（纯逻辑层 + 内核侧）：FIFO 名册、全表扫描、`svc` 陷阱式让出、周期状态线程、**无饥饿判据 + 常驻监视器** | ✅ |
| M4-9/M4-9.5 | **抢占（搬帧）**、VFP 现场随切换保存/恢复 | ✅ |
| M4-10 | **SMP 调度**：每核 idle/队列/计数器、挑最短队列、VFP 每核化 | ✅ |
| M4-11 | **串口排他换成源 OS 的 yield 型互斥**（D13）、**线程退出路径**（`DEATH`+让出+`wfi`，D14，**决定不回收**）、`sched_park_self` 退场 | ✅ |

逐阶段的细节、提交号、判据与踩过的坑：
`docs/ZYNQ7020_PORT_PLAN.md` §0.5.2（提交表）与 §0.5.5（坑表，目前 54 条）。

---

## 8. 明确**没有**实现的东西（别指望）

- **用户态 / 系统调用层**：完全没有。没有 `syscall` 分发，没有 `shell.elf`，没有 ELF 加载；
  ARM 侧遇到 `SVC` 只打一行诊断然后返回。
- **文件系统**：没有 VFS、没有 FATFS、没有块设备驱动（SD/arasan 一行没写）。
- **网络 / USB / 显示**：没有。
- **进程（PCB）**：没有。ARM 侧只有"线程（TCB）"这一层；上游的进程组/`fork`/`execve`
  语义还没接。
- **异常恢复**：除"打现场 + 停机"外没有恢复路径；没有中断嵌套。
- **`arch/arm32` 之外的架构**：只有 arm32；上游 x86_64 的 `arch` 层代码仍在
  `kernel/`、`boot/` 里（未改、未适配 ARM）。
- **可加载模块（`.sys`）**：ARM 侧没有动态链接器，也没有模块加载。

---

## 9. 硬件与调试前提（换环境会踩的坑）

| 项 | 值 |
|---|---|
| 板 | AC880-CB + AC850-CB（XC7Z020-1CLG400）；PL 比特流来自 `AXI_GPIO_1` 工程 |
| 串口 | PS UART1 @ `0xE0001000`，MIO48/49，**9600 8N1**，主机侧 `COM4` |
| JTAG | Vitis `xsdb`；脚本 `tmp-test/jtag/run_kernel_uart.tcl`（`reset system` → `ps7_init` → 比特流 → `dow`） |
| LED | PL 侧 AXI GPIO @ `0x41200000`（跑马灯，用来肉眼确认"内核在跑"） |
| 心跳 | OCM `0x00020000` 起 32 槽（**已满**，新的诊断量走串口/自检报告） |
| 已知硬故障 | XSA 若把 MIO bank1 声明成 3.3V，UART1 RX 会**静默收不到任何字节**（脚本里有硬断言拦它） |

---

## 10. 想自己复核的话，按这个顺序读

1. `docs/ZYNQ7020_INTEGRATION_PLAN.md` —— 与上游怎么合流（**将来**）
2. `docs/ZYNQ7020_PORT_PLAN.md` §0.5.1（一句话状态）→ §0.5.3（构建与验证命令）
   → §0.5.4（硬件事实）→ §0.5.5（坑表）→ §0.5.7（续接点）
3. `arch/arm32/README.md` —— 每个模块"为什么这么做、踩过什么"
4. `docs/PTASK.md` —— 调度/线程/生命周期那几个决定的完整证据链
5. 想直接看结论：`out/kernel-arm.elf` 上板跑一遍 `tmp-test/verify_board.py --load`

---

## 11. 贡献与反馈

- 这是**实验性 fork**，不要指望 API/行为稳定；上游与本分支都可能随时改。
- 与**上游**代码有关的问题请提到上游仓库；与**ARM 移植**有关的问题提到本 fork。
- 提 issue 时请带上：板型号、`verify_board.py` 的完整串口输出（脚本会把它存到
  `verify_board_raw.txt`）、以及你是否改过 `arch/arm32/**`。

---

**最后再强调一次**：本仓库**不完善、不可用作产品、正在开发中（WIP）**。
它能证明的事情只有一件 —— **上游那套调度/线程/锁语义，能在 Cortex-A9 上以同样的
形状跑起来，并且每一步都有板上判据。**
