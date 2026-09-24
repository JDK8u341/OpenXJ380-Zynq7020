# Zynq-7020 移植：项目宪章与重排计划

> 立宪日 **2026-09-19**。本文是这次移植的**最高约束**：与它冲突的既有计划
> （`ZYNQ7020_PORT_PLAN.md`）与既有实现，一律以本文为准，按 §八 处置。
> 所有数字都是 2026-09-19 当日**实测**，取证命令见 §三 末尾。

---

## 一、核心目的（唯一目标）

**把源 OS（XJ380）搬到 Xilinx Zynq-7020 上：让源 OS 自己的代码在 ARM 上跑起来，
行为与 API 与源 OS 一致。**

三条推论，用来裁决一切分歧：

| # | 推论 | 反面（本项目**不做**的事） |
|---|---|---|
| 1 | **移植 = 让上游原文运行** | 不是"另写一个能在 Zynq 上跑的 OS" |
| 2 | **修改上游 = 移植失败的一种** | 不许为了"能编过"改上游；不许顺手改上游的 bug |
| 3 | **源 OS 没有的东西 = 不该有** | 不许造源 OS 没有的框架、子系统、功能；不许"超越" |

一句话的判据：**任何一段代码，都要能回答"它在源 OS 里的对应物是什么"。**
答不出来，它就不该存在于这个仓库里。

---

## 二、五条不变式（可检查，每阶段必须报数）

| # | 不变式 | 检查方法 | 当前基线 |
|---|---|---|---|
| **I1** | **上游零修改**：`kernel/ driver/ lib/ include/ boot/ user/` 改动行数 = 0 | `git diff --numstat main HEAD` 过滤上游路径 | ❌ **330 行 / 17 文件** |
| **I2** | **不做源 OS 没有的东西**：内核里不得出现"指不到源 OS 角色"的模块 | §四 对应表逐项核对 | ❌ 发明物 ≈1,799 行 + 心跳区 |
| **I3** | **不新造 API**：移植侧提供的服务必须与源 OS **同名同语义** | 符号名比对（如必须是 `alloc_frames` 而不是 `palloc_alloc`） | ⚠ 部分违反（`vmap_*`/`palloc_*`/`console_*`） |
| **I4** | **验证脚手架不许进内核**：内核只"打印事实"，不"判定通过/失败" | 内核里不得有判定/汇总/自检计数 | ❌ 148 个判据在内核里 |
| **I5** | **不隐藏缺口**：不许用链接宽容（`-z muldefs`）或空实现掩盖缺失 | 期望重复符号必须列清单；拒绝型桩必须带"解开条件" | ⚠ `-z muldefs` 已在用（与源 OS 一致，但须列清单） |

两条度量（不是不变式，但每阶段必须报，用来防止慢性漂移）：

| 度量 | 含义 | 当前基线 |
|---|---|---|
| **M1（主指标）** | 上游原文在 ARM 上运行的代码行 / 上游适用代码行 | **3,343 / 32,454 = 10.3%** |
| **M2（反指标）** | 移植侧自有代码行 / 内核内发明物行数 | **12,625 / 1,711** |

> 口径（两个数字都由 `tmp-test/port_metrics.py` 复现，不靠人手数）：
> 目录取 `kernel/ driver/ lib/ include/ boot/ user/` 的 `.c/.cpp`，代码行 = 非空非注释；
> M1 分母再减去 vendored FatFs（`driver/fs/fatfs/*`，5 个文件）。
> M2 的"内核内发明物"= §八 点名的 25 个文件 + `kmain.c` 的自检段；
> `kmain.c` 其余启动/主循环约 1,800 行**未计入**，所以 M2 是**低估**。

> M1 上升、M2 不涨，才叫移植在推进。M1 不涨而 M2 猛涨，就是漂移。

### I1 的唯一例外：平台相关行（必须逐处登记）

**硬件指令级的代码无法"原样"移植** —— 上游 35 个文件里有 **106 处内联 x86 汇编**
（`int $32`、`cli`、`movq %0,%%fs`、`.intel_syntax`、`in/out` 端口 I/O）。
这类只能改。为避免"例外"变成"随便改"，规则如下：

1. 例外**逐处登记**在 `docs/ZYNQ7020_PORT_PLAN.md` 的「平台相关行清单」：`文件:行` + 为什么不能原样 + **解开条件**；
2. 只允许改**那几行指令**，函数签名、语义、结构体布局一字不动；
3. **优先用同名覆盖头**：`arch/arm32/include/upstream/**` 里的头文件按**同路径同名**放在
   `-I` 前面，即可覆盖上游头而不动上游一个字。这条是"零修改"的主力工具
   （已用它解决 `cpu/lock.h`、`cpu/regio.h`）。

---

## 三、三类处置 + 两类不适用（每个上游文件必须归入其一）

| 类 | 含义 | 判据 | 例子 |
|---|---|---|---|
| **P-A** | **原样编译** | 出现在构建图的 `ARM32_UPSTREAM_CXX` 里，且**一个字都没改** | `vfs.cpp`、`fatfs/*`、`dev.cpp`、`pipefs.cpp`、`pty.cpp`、`lock_queue.cpp`、`id_alloc.cpp` |
| **P-B** | **同名角色重写**（硬件边界） | 对外 API 与源 OS **同名同语义**，且**位置对应**（源 OS 的 `kernel/memory/page.cpp` ↔ 移植侧 arch 层） | x86 页表/IDT/APIC/8259/PS2/AHCI/HDA/UEFI boot |
| **P-C** | **源 OS 无对应，必须新写** | 必须能指出源 OS 的**同类角色**；指不出来 = I2 违例 | Zynq PS 外设驱动（同类角色 = `driver/serial/serial_port.cpp`、`kernel/intr/*`）、PS 初始化/比特流加载（同类角色 = `boot/`）、ARM 架构服务 |
| **N-A** | **不适用**（硬件不存在） | 板上没有这个硬件，不是"还没做" | `ahci/ nvme/ ide/ ps2/ hda/ sb16/ pci/ dma/ power`、COM1 |
| **N-B** | **暂时排除**（工具/编译器限制） | 必须写"解开条件" | 见 §七 第 1 条：`procfs.cpp` |

> N-A 与 N-B 必须分开写。"硬件不存在"和"还没做"混在一起，会让完成度永远说不清。

### 取证命令（本文所有数字的来源）

```bash
git diff --numstat main HEAD                      # I1 + 改动行去向
python3 tmp-test/measure_arm_cxx.py <文件...>      # 上游文件能否编过（用 -c，不用 -fsyntax-only）
python3 tmp-test/port_metrics.py                  # M1/M2 与不变式体检（P0 落地，见 §六）
```

---

## 四、对应关系表（源 OS 角色 ↔ 移植侧实现 ↔ 现状）

**每一个移植侧文件都必须在这张表里有位置。** "现状"列取值：✅原样 / ⚠同名角色 / ❌改写（待回退）。

| 源 OS 角色 | 源 OS 代码行 | 移植侧实现 | 现状 |
|---|---|---|---|
| `kernel/memory/*.cpp`（页表/物理帧/HHDM） | 2,046 | `mmu.c` `mmu_hw.c` `vmap.c` `palloc.c` | ⚠ **API 不同名**（`vmap_*`/`palloc_*`）→ 必须改成 `page_map_range`/`alloc_frames`/`phys_to_virt` |
| `kernel/memory/heap.cpp`（+ Rust liballoc） | 49 | `heap.c` `kmalloc.c` | ⚠ 见 §七 第 3 条（上游是 Rust 预编译库） |
| `kernel/task/*.cpp`（进程/线程/调度） | 3,232 | `sched.c` `sched_kern.c` `kstack*.c` `taskctx_hw.c` `percpu*.c` | ❌ **改写** → 目标：原样编译 `pcb.cpp`/`scheduler.cpp` |
| `kernel/task/mutex.cpp` | 143 | `mutex.c` `mutex_kern.c` | ⚠ 实测可原样编译（缺 6 符号）→ 可回收 |
| `kernel/intr/*` + `kernel/pctable/*` | 674 | `gic.c` + `boot/vectors.S` | ⚠ 同名角色（须提供 `init_idt_desc`/`send_eoi`/`open_interrupt` 等同名 API） |
| `kernel/smp/*.cpp` | 361 | `smp.c` | ⚠ 同上 |
| `driver/serial/serial_port.cpp`（658 行） | 658 | `console.c` `uart_ps.c` `uart_baud.c` | ❌ **改写** → 目标：原样编译 `serial_port.cpp`，只换 `write_serial(char)` 的底层 |
| `driver/device.cpp`（设备管理 + 块层） | 480 | `device.c` | ❌ **改写** → **实测可原样编译（缺 28 符号）**，补齐后删 `device.c` |
| `driver/fs/partition.cpp` | — | 无 | 待编（实测可原样编译，缺 34 符号） |
| `lib/*` | 155 | `krlibc.c` | ⚠ |
| `boot/`（UEFI） | 1,896 | `arch/arm32/boot/*` + stage0（未做） | ⚠ 同名角色（`BOOT_CONFIG` 布局保持） |
| `kernel/netdev.cpp` | — | 无 | 待编（实测可原样编译，缺 3 符号） |
| `driver/rtc.cpp` | — | 落地层抄了 `mktime` | ⚠ 板无 CMOS RTC（P-C 语义） |
| `kernel/syscall/*`、`user/xapi/*`、`dlinker.cpp` | 大头 | 无 | 未开始（P4 的硬判据） |
| — | — | `board.c` `board_devices.c` `plat_device.c` `axi_gpio.c` `led.c` `fault_test.c` `shell.c` `*_check.c` `selftest.c` | ❌ **源 OS 无对应角色 → I2 违例，按 §八 处置** |

---

## 五、当前基线（实测，供对照）

### 5.1 七个数字

| 项 | 值 |
|---|---|
| 分支规模 | **159 提交 / 7 天**（09-13 → 09-19），vs main **211 文件、+55,722 −159 行** |
| 改动行去向 | `arch/arm32` 33,471（59.9%）· `tests`+`tmp-test` 14,760（26.4%）· `docs` 5,505 · `tools` 1,048 · 根文件 767 · **上游 330（0.6%）** |
| 移植侧体量 | 84 文件 / 29,826 行 / 注释 45.5% / **代码 12,625 行** |
| 上游在跑 | 12 文件 / 28,524 代码行（其中 vendored FatFs 25,181 = **100%**） |
| **M1** | 源 OS 自有代码 **3,343 / 32,454 = 10.3%**（在跑的上游文件 12 / 97） |
| **M2** | 移植侧自有代码 12,625（≈3.8 × 跑起来的源 OS 代码） |
| 内核内发明 | **1,711 行**（工具口径：§八 点名 25 文件 + `kmain.c` 自检段 216 行）；其中描述层框架 477 · shell 305 · 故障注入 172 · 验收程序 303 · `selftest` 73 · 自检段 216 |

### 5.2 漂移趋势（arch/arm32 代码新增行分类）

| 区间 | 硬件子系统（正当） | 落地层/桥 | 发明 | kmain |
|---|---|---|---|---|
| 较早 40 提交 | 39.1% | 32.1% | 13.0% | 15.8% |
| **最近 30 提交** | **19.4%** | **48.6%** | **22.9%** | 9.1% |

最近 30 提交：`arch/arm32` 代码 4,682 行、`tests/tmp-test` 2,186 行、**上游 43 行**。

### 5.3 结构性对比

- 上游 `kernel/main.cpp` **538** 代码行 ↔ 移植侧 `kmain.c` **2,451** 代码行（4.6×）。
- 验证脚手架 `tests/` 6,718 + `tmp-test/` 5,686 = **12,404 行 ≈ 移植侧全部自有代码**。
- 上游注释占比 2–3%，移植侧 45.5%。

---

## 六、重排后的阶段（按"解锁上游原文"排序，不按功能）

> 原计划按功能里程碑（M0→M7）排；它的问题是把"另写一份"当成了完成。
> 新顺序的唯一排序依据：**这一步能让多少上游原文跑起来、能消掉多少违例。**

| 阶段 | 内容 | 判据（板上 + 宿主） | 度量预期 |
|---|---|---|---|
| **P0 立宪与度量** | 提交本文；落地 `tmp-test/port_metrics.py`（M1/M2 + I1–I5 体检，一条命令出报告）；把现有实现逐项登记进 §四 表 | 工具能复现 §五 的每个数字；跑一次即出"违例清单" | 不改内核 |
| **P1 去发明** | ①描述层框架删掉 → 改成"生成常量头 + 上游风格 `xxx_setup()` 序列"；②内核态 shell 删掉（等 P4 的上游用户态 shell）；③148 判据搬出内核 → 内核只用 `write_serial_*` 打印事实，**判定由主机脚本做**；④`fault_test` 移到 JTAG 工具侧；⑤心跳区逐槽改成串口日志 | 板子照常启动；串口日志含全部关键事实；主机脚本复现原有**全部**判据；`port_metrics.py` 的 **I2 归零**（1,711 行） | I2/I4 归零 |
| **P2 上游原文编译化** | 补齐**同名**平台服务（`alloc_frames/free_frames/page_map_range/unmap_page_range/phys_to_virt/driver_phys_to_virt/driver_virt_to_phys/get_current_directory/page_virt_to_phys/translate_address/lazy_tryalloc`），把实测可编的 5 个上游文件编进镜像（`netdev.cpp` −3 符号、`task/mutex.cpp` −6、`rtc.cpp` −8、`driver/device.cpp` −28、`fs/partition.cpp` −34）；**删掉 `device.c`** | 上游文件在 `ARM32_UPSTREAM_CXX` 且**零修改**；`device.c` 不存在；块层行为与今天逐项相同（宿主单测 + 板上 A/B） | **M1 ↑ 至 ~14%**，M2 ↓ |
| **P3 进程与调度原文** | 原样编译 `kernel/task/pcb.cpp` + `scheduler.cpp`（先处置 §七 第 2 条的 9 处内联汇编，逐处登记）；移植侧 `sched*.c`/`kstack*.c` 退场 | 调度器行为与今天逐项相同（八组破坏性 A/B 全过）；`pcb.cpp`/`scheduler.cpp` 出现在构建图 | M1 ↑ 显著 |
| **P4 用户态与 XAPI 原文** | 原样编译 `kernel/syscall/*`、`user/xapi/*`、`dlinker.cpp`；跑起源 OS 的 `user/cli_shell.cpp` | **源 OS 自己的用户程序在板上跑起来并交互** —— 这是"移植完成"的硬判据 | M1 ↑↑ |
| **P5 驱动按上游契约** | SD 驱动（`device_t` + `regist_device`，`DEVICE_BLOCK` 自动触发分区扫描）、UART 作为 `DEVICE_STREAM`；分区/挂载用上游 `partition.cpp` 原文 | 卡上 FAT 卷可挂载可读写，**FATFS 代码零改动** | M1 ↑ |
| **P6 自举** | stage0：由 XSA 生成 PS 初始化 + devcfg PCAP 加载比特流 + 装载内核 ELF（先替代 JTAG，后做 BOOT.BIN 从 SD/QSPI 启动） | **不用 Vivado/Vitis 任何导出物**，只用仓库 + XSA 就能把内核跑起来 | I1 不受影响（新写属 P-C/同名角色） |

**顺序不是任选的**：P1 必须先做 —— 带着发明物去补平台服务，会把发明物焊死在上游接口上；
P2 必须先于 P3/P4 —— 上游原文编进来时暴露的缺口，正是接下来要补的服务清单。

---

## 七、硬约束与现实（不写清这些，计划就是假的）

### 1. `procfs.cpp` 是**编译器**限制，不是移植问题

实测（2026-09-19）：`driver/fs/vfs/procfs.cpp:595` 用 C99 稀疏指定初始化器，
**GCC 的 C++ 前端直接 "sorry, unimplemented"**（clang 接受，x86 侧就是 clang）。
⇒ 三条路，**需要拍板**：(a) ARM C++ 改用 clang 编（要先验证 clang 能否配我们的 ARM 目标与链接）；
(b) 永久留在 N-B 排除表；(c) 改上游（**违反 I1，除非登记为例外**）。

### 2. 106 处内联 x86 汇编在 35 个上游文件里

分布（实测）：`kernel/cpu/fsgsbase.cpp` 9、`kernel/main.cpp` 10、`boot/bootx64.c` 7、
`kernel/smp/smp.cpp` 6、`driver/power.cpp` 6、`include/cpu/regio.h` 6、
`kernel/memory/page.cpp` 5、`kernel/task/pcb.cpp` 5、`kernel/task/scheduler.cpp` 4、
`kernel/syscall/*` 6、其余零散；头文件 4 个（`cpu/lock.h`、`cpu/msr.h`、`cpu/regio.h`、`krlibc.h`）
—— **头文件那部分已经可以用同名覆盖头零修改解决**（现在解决了 2 个）。
⇒ P-B 的规模是**可枚举且有上限的**，不是无底洞；但"上游 100% 零修改"在原理上做不到，
所以 I1 的定义是"**零修改 + 可枚举例外清单**"。

### 3. 源 OS 的 `malloc` 是 **Rust 预编译库**，ARM 上拿不到

`liballoc-x86_64.a`（552 KB，SHA-256 已登记）= Rust `alloc` + `compiler_builtins` + `talc`，
**x86_64 专用，仓库里没有 Rust 源码**（只有许可证与 `user/xapi/include/liballoc/alloc.h`）。
⇒ 三条路，**需要拍板**：(a) 找到并交叉编译同一套 Rust 运行时到 armv7-none-eabi（最贵、最"源 OS 优先"）；
(b) ARM 上永久用 C 分配器并登记为**永久偏离**；(c) 先登记、等 P2 之后再定。

### 4. 上游与 Zynq 的根本落差（P-B 的清单）

x86 页表 / IDT / APIC / IOAPIC / 8259 / PS2 / PCI / AHCI / NVMe / IDE / HDA / SB16 / USB-xHCI /
CMOS RTC / UEFI / MSR / GS base / 端口 I/O —— **板上全都不存在**，这些文件是 P-B 或 N-A。

### 5. 硬件侧的两份 XSA 各缺一半（阻塞 P5）

实测：`opjtmp.xsa` 的 ps7_init `SDI0_CPU_1XCLKACT=0`（SD 无时钟、MIO40-45 未复用给 SD0），
`System_wrapper.xsa` 的 `UART1_CPU_1XCLKACT=0`（串口不存在）。
原理图已确认 microSD（底板 J8）走 **MIO40-45 = SDMMC**。
⇒ 需要**用户**在 Vivado 导出**一份** XSA：UART1(MIO48/49, bank1 1.8V) + SD0(MIO40-45) + PL(AXI GPIO)。

---

## 八、明确撤销 / 降级的东西（I2 违例的处置）

| 撤销对象 | 行数 | 处置 | 替代（源 OS 对应角色） |
|---|---|---|---|
| `plat_device.[ch]` `board.[ch]` `board_devices.[ch]` `axi_gpio.[ch]` `led.[ch]`（描述层 + probe 框架 + PL 覆盖表） | 555 | **删** | 生成常量头 + 上游风格 `xxx_setup()` 序列（对应 `main.cpp:466-508`） |
| 内核态 `shell.[ch]` | 305 | **删** | 源 OS 的 shell 是用户态 `user/cli_shell.cpp`（P4） |
| `*_check.[ch]`（vfs/fatfs/pipe/pty 验收程序） | 390 | **并入主机侧断言** | 宿主单测 + 串口日志 |
| `fault_test.[ch]` | 172 | **移到 JTAG 工具侧** | 源 OS 无故障注入 |
| `selftest.[ch]` | 73 | **搬出内核** | 内核用 `write_serial_*` 打印事实；判定在主机脚本 |
| `kmain.c` 自检段（148 个 `selftest_report()`） | 216 | **搬出内核** | 同上 |
| **I2 合计** | **1,711** | | |
| OCM 心跳区 | — | **逐槽改为串口日志**（保留 JTAG 只读用途作为辅助） | 源 OS 的观测通道就是串口 |
| `device.c` + 落地层的块层 | 236 | **删，换上游 `driver/device.cpp` 原文** | 上游 `driver/device.cpp` |
| `sched*.c` `kstack*.c` | ~1,660 | **最终退场**，换上游 `pcb.cpp`/`scheduler.cpp` | 上游 `kernel/task/*.cpp` |
| `vmap_*`/`palloc_*` API | — | **改名/改造成上游同名服务** | `page_map_range`/`alloc_frames`/`phys_to_virt` |

**不撤销、且必须保留的**：ARM 架构服务（MMU/缓存维护/上下文切换/GIC/定时器/UART 底层）、
Zynq 板级事实（常量从 XSA 生成）、宿主单测与板上 A/B 验证本身（它们不是内核功能）。

---

## 九、代价与风险（诚实条款）

1. **P1 会暂时降低板级验证强度**：148 个判据搬出内核的过程中，有一段"判据尚未等价迁移"的窗口。
   缓解：P1 分两步（先并行打印 + 主机断言同时可用，再删内核判定），且每步都上板。
2. **P2/P3 会暴露大量真实缺口**（实测：仅 `device.cpp` 就缺 28 个符号）。这正是移植本来的工作量 ——
   现有替代实现把这些缺口藏住了，所以"看起来做完了"。
3. **"上游零修改"可能永远达不到 100%**（106 处 asm）。所以 I1 的可达定义是
   "**0 行未登记的上游改动**" —— 登记的每一行都要能说出为什么、以及什么条件下能撤销。
4. **M1 可能长期停在低位数**：P4 之前，源 OS 的主体（调度/进程/系统调用/用户态/网络/输入）都不在 ARM 上。
   这是真实进度，不许用 M2 的增长去掩饰。

---

## 十、需要拍板的四件事

| # | 问题 | 选项 |
|---|---|---|
| 1 | 接受"**上游零修改 + 可枚举例外清单**"作为硬规矩（含 P-B 同名角色重写）？ | 接受 / 修改定义 |
| 2 | P1 的验证强度置换是否接受（自检框架搬出内核 → 主机断言）？ | 接受 / 保留内核自检 |
| 3 | 源 OS 的 `malloc`（Rust 预编译、ARM 无源码）怎么办？ | 找源码交叉编译 / 永久用 C 分配器并登记 / 先登记后定 |
| 4 | ARM C++ 是否改用 clang（解开 `procfs.cpp` 的 GCC 限制）？ | 换 clang / 永久排除 / 登记为上游例外 |
