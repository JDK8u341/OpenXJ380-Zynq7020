# 与上游合流的路线：源仓库的代码将来在移植版里扮演什么角色

> **这份文件回答四个问题**（也是拿到 fork 的人最该先问的）：
>
> 1. 上游（x86_64 那套）的代码，将来会不会在 ARM 移植版里起作用？
> 2. 会的话，在**哪些层**？哪些 **ABI/API 会保持不变**（直接复用或小范围修改）？
> 3. `arch/arm32/**` 将来怎么接进上游那套上层抽象（而不是像今天这样"平行的一棵树"）？
> 4. 顺序是什么、每一步的判据是什么、有哪些还没拍板的决策？
>
> **分工**：
> - `docs/ZYNQ7020_PORT_STATUS.md` —— **今天**是什么状态、改了什么、怎么构建、怎么测、不能用在哪
> - `docs/ZYNQ7020_PORT_PLAN.md` —— 完整计划与逐阶段记录（本文大量引用它的 §4.4/§4.6/§5）
> - `docs/PTASK.md` —— 调度/线程/生命周期那几个决定的证据链
>
> ⚠ 本文是**路线**（计划），不是已完成的事实。凡是"已完成"的地方都会显式标注。

---

## 0. 结论先行

| 问题 | 答案 |
|---|---|
| 上游代码将来有用吗？ | ★ **有大用，而且是主体** ★ —— 系统调用处理体、进程/线程管理、内存分配器、用户态管理、VFS/文件系统、设备模型、块层、网卡/网络栈、用户态 XAPI 与示例程序，都在"**直接复用或小范围修改**"这一档 |
| 现在接上了吗？ | **没有**。今天是零共享的平行子树（见 `PORT_STATUS` §3/§4） |
| 需要重写的部分在哪里？ | **集中在四件事**：① 启动路径（UEFI → 裸机/JTAG）② 异常入口与寄存器帧（`handler.S` → ARM 向量/帧）③ 架构原语（MSR/CPUID/页表/缓存 → CP15/MMU/PL310）④ 平台驱动（PCI/xHCI/E1000/HDA → GIC/UART/定时器/SD/GEM） |
| 哪些东西**不许变**？ | `pcb_t`/`tcb_t` 的**指针 typedef 约定**、`pcb`/`tcb` 里**架构无关字段的布局**、`spin_t` 的 **irqsave 契约**、`mutex_*` 的**语义**、`errno` 的**数值**、`device_t`/`netdev`/块层的**回调形状**、`ELF` 的**已声明常量**、模块的 **`dlmain` + `EXPORT_SYMBOL`** 接口形状 |
| 最难的接口在哪？ | ★ **系统调用入口的寄存器约定**（`registers_t` 天然架构相关，x86 的 `ds/es/err_code` 在 ARM 上不存在）与 **syscall 号表**（SXAH 私有号 57 位，装不进 32 位 `r7`，**必须重编号**）★ |
| 谁来决定下一步？ | ★ **2026-09-18 三件事已拍板**（表见 §6）：**D2** 内存分配器 = 保留 ARM 那份 + 补上游形状的接口；**D4** 用户态 = **新增 M4A-4**（并改写 M5 验收：不在 M5 跑 `shell.elf`）；**D5** 心跳区 = **扩容 32 → 64 槽**。⇒ 下一站是 M4A-1.1 ★ |

---

## 1. 三层划分：复用 / 适配 / 不搬

判据是"**这份代码依赖不依赖 x86 特有的东西**"，而不是"它看起来重不重要"。

### 1.1 ★ 第一层：直接复用（不改或**小范围修改**）★ —— 上游代码的主体

| 子系统 | 上游位置 | 规模/说明 | 为什么能复用 |
|---|---|---|---|
| **系统调用处理体** | `kernel/syscall/sys.cpp`(5,313 行)、`kernel/syscall/xapi/*.cpp` | 纯 POSIX 语义逻辑 | 只依赖 `regs` 的**字段名**，不依赖寄存器布局本身 ⇒ 换架构只需换 `regs` 的来源（见 §3 的 `syscall_regs.h`） |
| **进程/线程管理** | `kernel/task/{pcb,scheduler,mutex,poll,ipc,reaper}.cpp` | EEVDF 调度策略、锁协议、队列、fork/clone 记账 | **每个文件里约 40 行**的寄存器/内存状态管线是架构相关的，其余是策略与簿记。★ 而且 ARM 侧已经把这套语义**照抄并在板上验过**（M4-8…M4-11） ⇒ 合流时两边说的是同一种语言 |
| **内存分配器** | `kernel/memory/{bitmap,buddy,heap,lazyalloc,vma}.cpp`（1,222 行） | **无汇编** | 纯算法；ARM 侧目前是**自己写的一份**（`arch/arm32/src/{palloc,heap,vmap}.c`，宿主穷尽测）—— 合流时要决定"用上游那份 + 换页表后端"还是"保留 ARM 那份 + 对齐接口"，见 §6 决策 D2 |
| **用户态管理** | `kernel/user/*.cpp`（1,595 行） | 无汇编 | 同上 |
| **VFS 与文件系统** | `driver/fs/**` 全部（VFS、FATFS、proc/dev/tmp/pipe/pty/socketfs） | 建立在平台中立的 `device_t` 上 | 平台无关；ARM 侧只要提供块设备与描述层 |
| **设备模型** | `include/device.h` + `driver/device.cpp` | 256 项扁平数组 + 函数指针回调 | 与总线无关；ARM 侧已有对应的 `plat_device_t` 描述层作为**数据来源**，接口形状要向它对齐（就是计划里的 **B5/M4A-3**） |
| **块层** | `driver/device.cpp` 的 `rw_device`/`blk_device_read`/`blk_device_write` | 已处理非整扇区与物理连续透传 | SD 驱动只需实现 4 个回调 |
| **网卡 ABI 与网络栈** | `include/netdev.h`(5 个字段)、`kernel/netdev.cpp`、`kmod/netserver/lwip/**` | lwIP 本身架构中立 | 只需换网卡驱动（GEM）与 `sys_arch` 胶水（胶水里只有 4 处 x86 汇编） |
| **用户态运行时/API** | `user/xapi/**`（含 `constart.cpp`） | POSIX-like 封装 | `constart.cpp` 几乎不用改（去掉 x86 属性）；**目标是在 ARM 上编出 32 位 ELF 并跑起来** |
| **ELF / 动态链接** | `include/elf.h`、`kernel/dlinker.cpp` | ★ `include/elf.h` **已含 `EM_ARM`、`ELFCLASS32`、全部 `R_ARM_*`** ★；C++ 名字改编是 Itanium ABI | 架构中立；缺的只是"ARM 上的重定位应用"与模块加载的落地 |
| **FATFS 配置** | `driver/fs/fatfs` 配置 | `FF_VOLUMES 10`、512 字节扇区、exFAT/LFN 就绪 | SDXC 可直接用 |
| **第三方源码** | `third_party/{talc,libutf,libvterm,lexbor,linux-uapi,mikanos-hankaku,busybox-source,mbedtls-src}`、`font/hankaku.bin` | 架构中立 | 直接复用 |
| **`include/mm/uaccess.h`** | — | 翻译后 `memcpy` 的设计 | 天然可移植 |
| **文档/工具链脚本** | `tools/ninja_build.py`、`tools/stage_image_base.sh` 等 | 镜像与打包流程 | ARM 的镜像打包还没做，这些是现成模板 |

### 1.2 第二层：适配（接口形状不变，**换平台实现**）

| 子系统 | 上游位置 | 要做什么 | 落点 |
|---|---|---|---|
| **启动路径** | `boot/**`（UEFI）、`kernel/main.cpp` 的初始化顺序 | 换成本板的"JTAG 直载 + `ps7_init` + 裸机入口"（**已完成**）；将来若要 SD 启动则接 FSBL + `bootgen`（工具已实测可用） | ✅ M0/M1 已完成（BSP 路径）；SD 启动未做 |
| **异常入口与寄存器帧** | `kernel/intr/handler.S`、`handler.h` | ARM 向量表 + 异常帧（帧**必须建在任务栈上**，因为 `rfeia sp!` 从帧地址反推 SP）。**已完成**（M4-6/M4-7） | ✅ 已完成 |
| **架构原语** | `kernel/cpu/**`（MSR/CPUID/FPU/FSGSBASE）、`kernel/memory/**` 的页表部分 | 换成 CP15/MMU/PL310/每核寄存器（`arch/arm32/src/*_hw.c`）。**已完成**（M2/M4-2…M4-5） | ✅ 已完成 |
| **SMP 启动** | `kernel/smp/**` | 换成 SGI/IPI + `SEV` 唤醒 CPU1。**已完成**（AM3/M4-10） | ✅ 已完成 |
| **中断控制器** | `kernel/pctable/**`（IDT/APIC） | 换成 GIC-390（PL390）+ 每核私有定时器。**已完成**（M1/M4-8） | ✅ 已完成 |
| **平台驱动** | `driver/**` 的 PCI/xHCI/E1000/HDA/PS2… | 本板是 **PS + PL**，没有 PCIe 枚举、没有 xHCI。要写：UART/GIC/定时器（做成描述设备）、SD（arasan）、GEM（网卡）。**只有 AXI GPIO 一个驱动** | ⏳ M4A-2 起 |
| **系统调用入口** | `kernel/intr/handler.S:295-374`（进入内核切到 `task->syscall_stack`，退出读回 `syscall_user_rsp`） | ARM 侧要做**等价的两件事**：`SVC` 入口换栈到任务的内核栈、退出前恢复用户 SP/PC；TCB 里要有对应的两个字段（x86 是 `0xb48`/`0xb50`，ARM 侧**按自己的布局**，但**字段角色一致**） | ⏳ 用户态阶段（M4A-4 或 M7，待拍板） |
| **用户态切换** | `switch_task_to_user_mode`、`pcb.cpp:725-758` | ARM 侧等价物：设置用户模式 CPSR、页表 AP/XN、用户栈 | ⏳ 同上 |
| **进程（PCB）** | `kernel/task/pcb.cpp` 的进程组/`fork`/`execve` | ARM 侧**现在只有线程（TCB）**，没有进程层；要按上游语义补 | ⏳ 用户态阶段 |

### 1.3 第三层：不搬（x86 专有，与本平台无关）

- `kernel/cpu/**` 里的 MSR/CPUID/`swapgs`/FSGSBASE 一类；
- `kernel/pctable/**`（IDT/GDT/TSS/APIC）、`boot/**` 的 UEFI 协议栈；
- `driver/` 里面向 PC 平台的：AHCI/SATA、xHCI/USB、E1000、HDA、PS/2、PCI/PCIe 枚举；
- `graphics/`、`font/`（历史代码，已不参与上游构建）；
- `kmod/xhci/**` 等 PC 外设模块（除非板上有对应硬件）。

⚠ 注意"不搬"是**针对 ARM 目标**说的：这些文件在上游构建里当然仍然有效，
本分支也一行没删。

---

## 2. ★ ABI：哪些保持不变，怎么钉住 ★

"ABI 不变"分三类，**不能笼统地说"对齐布局"**（这是计划 §4.7.6 那节的结论）：

| 类别 | 例子 | 兼容要求 | 现在的锚点 |
|---|---|---|---|
| **架构无关** | `device_t` 的全部字段；`pcb`/`tcb` 里的 pid / status / `task_level` / fd 表 / cwd / 队列节点 | **布局逐字段一致** | ARM 侧 `arch/arm32/include/arch/tcb.h` 每个字段带上游 `pcb.h:NN` 出处 + `_Static_assert` 钉住偏移与 `sizeof`（`tests/test_arm32_tcb.py`） |
| **架构相关** | `registers_t`（x86 是 `ds/es/rax…rip/rflags/rsp/ss`）、异常帧、`syscall_stack`/`syscall_user_rsp` 偏移 | **布局按设计不同**；只共享**名字与角色**，各自实现 | ARM 侧 `arm_exc_frame_t`（`include/arch/taskctx.h`，汇编与 C 共用同一份定义）+ 它的布局自检 |
| **约定** | `pcb_t` / `tcb_t` 是**指针 typedef**（`typedef struct … *tcb_t`） | **必须沿用**，否则所有调用点都要改 | ARM 侧已沿用同名同义（`tcb_t` = 指针） |

逐项清单：

| ABI | 上游 | 移植版 | 状态 |
|---|---|---|---|
| `tcb_t` / `pcb_t` 的指针约定 | `include/task/pcb.h` | 同名同义 | ✅ 已一致 |
| `tcb` 架构无关字段布局 | `include/task/pcb.h` | `arch/arm32/include/arch/tcb.h`，逐字段带上游行号 + 静态断言 | ✅ 已一致（差 `syscall_stack` 一类用户态字段，未做） |
| `spin_t` 契约（**irqsave**：进锁保存并关中断，出锁原样恢复） | `include/cpu/lock.h:19-50` | `arch/arm32/include/arch/cpu.h` 的 `spin_lock/unlock` | ✅ 契约一致，载体不同（x86 存 RFLAGS，ARM 存 CPSR） |
| `mutex_*` 语义（yield 型、递归、`-EDEADLK`/`-EPERM`/`-EBUSY`/`-EINVAL`） | `kernel/task/mutex.cpp` | `arch/arm32/src/mutex.c`（**纯状态机**，宿主穷尽测） | ✅ 语义一致（M4-11.1），⚠ 等锁那一跳是偏离（D17，有理由） |
| `errno` 数值 | `include/errno.h` | `arch/arm32/include/arch/errno.h`（副本） | ✅ 有**逐值比对测试**（`tests/test_arm32_krlibc.py`） |
| `TaskLevel` 取值 | `include/task/pcb.h`（宏） | 同 | ✅ |
| 调度常量（片长 / 补偿 / 权重） | `scheduler.cpp:17-24` | 同 | ✅ |
| `device_t` 字段与 `regist_device` 形状 | `include/device.h` | `plat_device_t` 描述层（数据来源不同，形状要收敛） | ⏳ **M4A-3 / B5** |
| 块层回调（`rw_device` 的 4 个回调） | `driver/device.cpp` | 待接 SD/ramdisk | ⏳ M4A-1.3 |
| 网卡 ABI（`netdev` 5 字段） | `include/netdev.h` | 待接 GEM | ⏳ M5+ |
| ELF 常量（`EM_ARM`/`ELFCLASS32`/`R_ARM_*`） | `include/elf.h` | 直接复用 | ✅ 已就绪（加载器未写） |
| 模块接口（`dlmain` + `EXPORT_SYMBOL` + Itanium 改编名解析） | `kernel/dlinker.cpp`、`include/openxj380/**` | ARM 侧**没有**动态链接器 | ⏳ 未排期；见 §6 决策 D3 |
| **syscall 寄存器约定** | `registers_t` + `handler.S` 的 `0xb48/0xb50` | ARM 侧按 `r0-r6/r7/…` 自行定义，**名字对齐**（`syscall_regs.h`） | ⏳ 用户态阶段 |

---

## 3. ★ API：哪些保持不变 ★

| API | 上游 | 移植版计划 | 状态 |
|---|---|---|---|
| **XAPI 用户态 API**（POSIX-like 封装） | `user/xapi/**`、`user/xapi/include/**` | **目标是原样在 ARM 上编译并运行**（32 位 ELF + 硬浮点 ABI） | ⏳ |
| **示例程序** | `user/cli_shell.cpp`（`shell.elf`） | 目标：M5 的验收"挂载 `/system` 并跑 `shell.elf`"在 ARM 上成立 | ⏳ |
| **用户态启动逻辑** | `constart.cpp` | 几乎不用改（去掉 x86 属性） | ⏳ |
| **syscall 号表** | Linux x86_64 号 + SXAH 私有号 | ★ **硬阻塞**：`SXAH_SYSCALL_RETURN 128956723895689201 > 2^56`，**装不进 32 位 `r7`，必须重编号**。好消息：全部用户态二进制本来就是 x86-64 机器码、**本来就必须全部重建**，所以重编号是免费的（计划 §3 的 R1） | ⏳ 决策见 §6 D1 |
| **调用点兼容** | `kernel/syscall/**` 里约 **679 处** `regs->r*` | 引入 `include/arch/<arch>/syscall_regs.h`，把 ARM `r0-r6` 映射到**既有字段名** ⇒ 把"大改"降级成"机械替换" | ⏳ 用户态阶段的第一件事 |
| **模块（`.sys`）接口** | `dlmain`、`EXPORT_SYMBOL(_OBJECT)`、Socket ABI（`include/openxj380/**`） | ARM 侧若要支持模块，需一套 ARM 动态链接器 + 导出面 | ⏳ 未排期 |
| **驱动接口（B5）** | `include/device.h` + `regist_device` | ARM 侧 `plat_device_t`/`plat_driver_t` 向它收敛 | ⏳ M4A-3 |
| **文件系统接口** | VFS/`diskio`/`FF_*` | 直接复用 | ⏳ M4A-1 |
| **内核内部 C/C++ 风格与构建目标** | `AGENTS.md`、`ninja format`/`check` | ARM 侧遵守同一份风格；`arch/arm32` **刻意不在 `ninja format` 范围内**（注释对齐会被打散，见 `arch/arm32/README.md`） | ✅ 已知偏离，已记录 |

---

## 4. ★ `arch/arm32/**` 将来怎么接进上层抽象 ★

### 4.1 目标形状

上游的架构相关性今天是散在 `kernel/**` 与 `boot/**` 里的（x86 只有一套，所以没人被逼着抽）。
移植版要做的其实是**上游欠的一笔结构账**：

```
include/arch/<arch>/          ← 架构抽象面（现在不存在；ARM 侧已经在按这个形状长）
        boot.h      启动入口/早期栈/异常向量安装
        exc.h       异常帧布局（汇编与 C 共用）+ 帧存取
        cpu.h       每核寄存器/中断开关/屏障/自旋锁契约
        mmu.h       页表与地址转换
        percpu.h    每核数据（宿主与目标布局一致的定宽字段）
        syscall_regs.h  把架构寄存器映射到 registers_t 的既有字段名（★ 关键接口）
        taskctx.h   上下文/切换原语
arch/<arch>/                 ← 各架构的实现（ARM 侧今天就是 arch/arm32/**）
```

### 4.2 ARM 侧**已经**为这件事准备好的东西

- 已在用 `include/arch/<name>.h` 的命名与分层（`arch/arm32/include/arch/*.h`，`arch/` 前缀是刻意的）；
- **纯逻辑层 / 平台半**的强制分工：`sched.c`、`mutex.c`、`heap.c`、`palloc.c`、`vmap.c`、`kstack.c`、`mmu.c`、`cache.c`、`percpu.c` **不含 MMIO/CP15/内联汇编**，因此宿主可编译、可穷尽测；
  对应的 `*_hw.c` 放架构动作（`percpu_hw.c`、`mmu_hw.c`、`cache_hw.c`、`kstack_hw.c`、`taskctx_hw.c`、`mutex_kern.c`、`sched_kern.c`）；
- **异常帧与汇编的单一真值来源**：`include/arch/taskctx_asm.h` 的偏移被 `.S` 与 C 同时包含，改布局必然触发重编（上游 x86 的 `handler.S` 是硬编码偏移，没有编译期检查 —— 这是移植侧反超上游的一点）；
- **`errno` 副本 + 逐值比对测试**：把"两边必须一致"从口号变成测试；
- **接线式钩子**（`kstack` 的 TLB 失效、`console` 的排他、`mutex` 的四个钩子）：让"平台动作"成为可断言被调用过的东西；
- **每核数据布局断言**（`sizeof(percpu_t)==80` 等）：宿主与目标布局一致这件事被静态钉住。

### 4.3 还缺的（按顺序）

1. **描述层与驱动接口统一（B5/M4A-3）**：`plat_device_t`/`plat_driver_t` → 上游 `device_t` + `regist_device`。
   **必须排在文件系统之后**，因为 `regist_device` 依赖 devfs（计划 §4.6 的理由）；
2. **外设收进描述层（M4A-2）**：现在 UART/GIC/定时器是**硬编码直调**，描述表里只有 1 个驱动；
3. **用户态与 syscall 层（= M4A-4，2026-09-18 拍板）**：`syscall_regs.h` + `SVC` 分发 + ELF32 加载 + 用户页表属性；
   ★ 判据是**手写**的静态 32 位测试程序，**不含** busybox/musl（那是 M7）；
4. **进程层**：把上游的 PCB/进程组/`fork`/`execve` 语义接到 ARM 的 TCB 上；
5. **模块（`.sys`）**：ARM 动态链接器与导出面（可选，优先级最低）。

---

## 5. 分阶段路线（每步都有上板判据）

| 阶段 | 复用什么（上游） | 要写什么（移植侧） | 上板判据 |
|---|---|---|---|
| ✅ M0–M4-11（已完成） | 语义/契约照抄：调度、TCB、锁、errno、常量 | 整个 `arch/arm32/**` 底座 | **97 项自检全绿 + 8 组破坏性 A/B**（`PORT_STATUS` §7） |
| **M4A-1 文件系统** | `driver/fs/**`（VFS/FATFS/tmpfs/proc/dev/pipe/pty）、`diskio` 路径、FATFS 配置 | device manager + `regist_device` + devfs 骨架；VFS + tmpfs；块设备（先 RAM 盘，后 SD） | 建/读/写/列目录；FATFS 挂载 + 与主机侧比对 |
| **M4A-2 外设进描述层** | `device_t` 形状 | 用**当前 XSA** 重新生成描述表；UART/GIC/定时器 写成描述驱动 | `Board probe: probed` 从 1 涨到 4+，且**逐项功能不回退** |
| **M4A-3 B 系列（= B5）** | `include/device.h`、`driver/device.cpp`、块层 | 驱动接口对齐上游；`.dts`/`.dtb` 生成与加载（若采用 FDT 路线） | "换 DTB 不重编译"；旧驱动不改代码即可编 |
| **M4A-4 用户态与 syscall**（2026-09-18 拍板新增，合流决策 D4） | `kernel/syscall/**`、`kernel/user/**`、`user/xapi/**` | `syscall_regs.h` + `SVC` 分发 + `switch_to_user` + ELF32 加载 + 用户栈/页表属性 | 能跑一个**手写的**静态链接 32 位用户程序（最小 syscall：`write`/`exit`）。★ **不需要 musl/busybox** —— 这是它和「跑 `shell.elf`」的关键区别 |
| **M5 / M6** | VFS/FATFS、网络栈、模块框架 | SD 驱动、GEM 网卡、镜像打包（`bootgen`/SD 启动） | **M5：内核侧挂载 `/system` 并读文件 + 跑 M4A-4 的用户态程序**（原话「跑 `shell.elf`」**2026-09-18 移给 M7** —— 它要 ARM 用户态**加**重建 busybox/musl）；**M6：网络收发；模块加载** |
| **M7** | syscall 处理体（5,313 行）、XAPI、`shell.elf` | 进程层（PCB/进程组/fork/execve）、syscall 号表落地 | **跑起 `shell.elf`** —— 那时"上游代码在 ARM 上真的活了" |

---

## 6. ★ 合流决策 ★（写在这里，别在实施时才发现）

> ⚠ **编号警告**：这里的 **D1–D6** 与 `docs/ZYNQ7020_PORT_PLAN.md` §3.1 的 **D1–D6**
> 是**两套完全不同的编号**（那边是工具链/浮点 ABI/页表形式/描述层/双核/PL 范围）。
> 引用时请写「**合流决策 D#**」。

| # | 决策 | 选项 | 状态 / 结论 |
|---|---|---|---|
| **D1** | **syscall 号表策略**（计划 §3 的 R1） | (a) 保留 Linux x86_64 号 + 号翻译层 (b) ARM 独立号表 | ⏳ 未拍板（M4A-4 开工前要定）。SXAH 私有号**无论如何都要重编号**（`pxapi.h:25` = 128956723895689201，57 位装不进 `r7`），而全部用户态本来就要重建 ⇒ 代价比原估计小 |
| **D2** | **内存分配器用哪一份** | (a) 上游 `kernel/memory/**` + 换页表后端 (b) 保留 ARM 侧自写那份（宿主已穷尽测）+ 对齐接口 | ✅ **2026-09-18 选 (b)**。取证：上游那套 8 文件 2,196 行，其中 `page.cpp` 871 行拉 `cpu/longm.h`/`regio.h`/`fsgsbase.h`，`mm/page.h`→`mm/memory.h`→`<efi/efi.h>` ⇒ 等于重做计划 §2.3 自称"**单项风险最高**"的工作包；ARM 侧 1,363 行已有 **705 行宿主测试** + 板上 97/0。而"对齐接口"的真实成本很小：M4A-1 会拉的 19 个文件里堆 API 只有 **4 个符号 / 257 处调用**（`free` 187、`malloc` 55、`calloc` 10、`realloc` 5），另加 `PAGE_SIZE` 51 / `MIN` 19 / `MAX` 4 / `PADDING_UP` 3 ⇒ ~40 行兼容头 + 一个 `heap_alloc` shim |
| **D3** | **是否支持 `.sys` 模块** | (a) 支持（需 ARM 动态链接器 + 导出面） (b) 暂不支持，驱动先内建 | ⏳ 未拍板（M4A-2/M6 前要定）。上游的模块 ABI 只导出了约 76 个符号，且**缺** `spin_lock`/`mutex_*`/EOI/中断注册 ⇒ 直接照搬会导致驱动只能轮询（计划 §5 末尾） |
| **D4** | **用户态归属哪个阶段** | (a) 新增 M4A-4（建议） (b) 并进 M7，并把 M5 验收降级 | ✅ **2026-09-18 选 (a)**：**新增 M4A-4**，放在 M4A-3 与 M5 之间。★ 并且**改写 M5 的验收** —— 拍板时查明 M4A-4（SVC 分发 + `switch_to_user` + ELF32 + **手写**静态 32 位测试程序）**不需要** musl/busybox，而「跑 `shell.elf`」需要，故**移给 M7**。全部证据见计划 §0.5.7 末尾 |
| **D5** | **心跳区扩容** | (a) 扩 OCM 布局 (b) 删旧槽 | ✅ **2026-09-18 选 (a)：32 → 64 槽**。取证：34 个 `HB_SLOT_*` 名字**每个都有写者**，没有一个死槽可删；而心跳是"串口不可用时唯一还能观测的通道"，且**新诊断量不能改放 DDR**（写回可缓存 ⇒ JTAG 直读物理内存会读到陈旧值）⇒ 扩容是唯一正确选项。代价常数级：两个常量 + **7 行 / 5 个文件的硬编码**（`tmp-test/guard_trip.py:92`、`jtag/inject_fault.tcl:18,21`、`jtag/run_kernel_uart.tcl:206,212`、`arch/arm32/include/arch/fault_test.h:11-19`），有两条 `_Static_assert` 在编译期护栏，**不用动页表** |
| **D6** | **与上游的同步方式** | (a) 持续 rebase 到上游 `main` (b) 到某个里程碑再合 | ⏳ 未拍板。因为**上游文件几乎没改**（唯一共享改动点是 `tools/gen_ninja.py`，+261/−4），冲突面很小；将来可把 ARM 构建图拆成 `tools/gen_ninja_arm.py` 让这个面变成零 |

---

## 7. 三条"合流纪律"（不变量）

1. **不改上游的 ABI 形状**：`pcb`/`tcb` 的架构无关字段、`device_t`、块/网卡回调、
   `errno` 数值、`spin_t` 契约、`mutex_*` 语义 —— 要改就**两侧一起改**，并在宿主测试里
   加"逐值/逐字段"的比对（ARM 侧已有两个先例：`errno` 逐值比对、TCB 字段带上游行号）。
2. **架构相关的东西一律落在 `arch/<arch>/**`**，不往 `kernel/**` 里塞 `#ifdef`。
   ARM 侧今天能做到"`git diff 08e5c9c -- kernel include` 为空"，合流之后也要能保持
   "上游文件只在**平台中立重构**时改动，且每处都有理由"。
3. **每一步都要有上板判据**，而且判据要区分**机制**（板上）与**策略**（宿主）：
   板上只证明"接上了、真的跑了"，策略的穷尽测在宿主（`tests/test_arm32_*.py`）。

---

## 8. 想快速核对本文的说法

```bash
# 上游代码有没有被改过（答案：kernel/include/... 0 个文件）
git diff --name-only 08e5c9c HEAD -- kernel include driver user lib boot kmod resources

# ARM 侧到底编译了哪些文件（答案：只有 arch/arm32/**）
grep -A40 '^ARM32_C_SOURCES' build-arm.ninja

# 契约一致性的现成证据
python -m pytest tests/test_arm32_krlibc.py tests/test_arm32_tcb.py \
                 tests/test_arm32_mutex.py tests/test_arm32_sched.py -q

# 板级判据（需要板子 + JTAG）
python tmp-test/verify_board.py --load
```

**再重复一次**：本文是**路线**。今天这个仓库里，上游代码与 ARM 代码**还没有接上**，
状态见 `docs/ZYNQ7020_PORT_STATUS.md`。
