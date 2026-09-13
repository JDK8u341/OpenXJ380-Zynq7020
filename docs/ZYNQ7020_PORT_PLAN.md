# OpenXJ380 → Zynq-7020 移植方案

本文基于对当前代码库（`08e5c9c`）的逐文件审计，给出把 OpenXJ380 从 x86_64 / UEFI
移植到 Xilinx Zynq-7020（XC7Z020，双核 ARM Cortex-A9，ARMv7-A）所需的工作分解、
风险点和分阶段路径。所有引用均带 `文件:行号`，可直接核对。

---

## 0. 结论先行

**这不是一次"加个架构宏"的移植，而是一次架构层重写加平台层重建。**

三条硬事实决定了工作量：

1. **项目里没有任何 HAL / 架构抽象层。** 全仓库搜索 `__x86_64__|CONFIG_ARCH|ARCH_X86|__arm__|__aarch64__`
   在一方代码中**零命中**（只命中 `third_party/**` 里的 mbedTLS、dr_mp3）。不存在 `arch/` 目录、
   不存在 `struct arch_ops`、不存在弱符号回退。仅有的两处条件编译
   （`kernel/pctable/gdt.cpp:1`、`kernel/memory/frame.cpp:50`）**都没有非 x86 分支**——
   在 ARM 上 `gdt.cpp` 会编译成空目标文件，导致 `init_gdt()`（`kernel/main.cpp:445` 调用）成为未定义符号。

2. **x86 假设是散落的，不是集中的。** `kernel/**` 约 95 处内联汇编、`include/**` 约 28 处，
   直接映射基址字面量 `0xffff800000000000` 被重复硬编码在 **14 个以上文件**里。

3. **全部用户态二进制都是 x86-64 机器码**，无法执行。已逐个读取 ELF 头确认：
   `resources/apps/busybox`、`resources/apps/fastfetch`、`resources/musl/ld-musl-x86_64.so.1`、
   `libc.so`、`libc.musl-x86_64.so.1`、`libgcc_s.so.1` 全部是 ELF64 `EM_X86_64`；
   `liballoc-x86_64.a` 是 x86_64 的 Rust `alloc`+`compiler_builtins`+`talc` 归档，
   且**被链接进内核本身**（`tools/gen_ninja.py:699`）。

**规模量级**：审计面约 19,500 行，其中约 **3,300–3,600 行是不可约的 x86 代码**必须重写；
另有约 1,200 行"可移植但依赖 x86 原语"（`page.cpp` 页表遍历、`uaccess.h`、`frame.cpp`）。

**一个重要的好消息**：内核的**业务逻辑层基本是可移植的**——
`kernel/syscall/sys.cpp`（5,313 行 POSIX 语义处理）、`kernel/user/*`（1,595 行）、
`kernel/task/` 的 EEVDF 调度与队列逻辑、`kernel/memory/{bitmap,buddy,heap,lazyalloc,vma}.cpp`
（1,222 行，无汇编）、整个 VFS/FATFS、以及 `device_t` 设备抽象
（`include/device.h:21-45`，函数指针式 read/write/ioctl，本来就是平台中立的）。
这些不需要重写，只需要下面这些"地基"换掉。

---

## 1. 目标平台落差清单

| 维度 | 现状（x86_64） | Zynq-7020（ARMv7-A） | 影响面 |
|---|---|---|---|
| 位数 | 64 位 | 32 位 | 地址空间、页表、用户 VA 布局、`liballoc` |
| 内核链接基址 | `0xFFFFFFFF80000000`（`linker.ld:11`） | 32 位可达地址（如 `0xC0000000` 或低端物理） | `linker.ld`、引导加载 |
| 直接映射 | `0xffff800000000000`（`kernel/memory/hhdm.cpp:15` + 14 处字面量） | 需重新规划 | 大量调用点 |
| 分页 | 4 级 PML4，`>>39/30/21/12` | 短描述符 2 级 或 LPAE 3 级 | `include/mm/page.h:3-13`、`page.cpp`、`uaccess.h` |
| 中断控制器 | LAPIC/IOAPIC/x2APIC + 8259A | PL390 GIC（Distributor + CPU Interface） | `kernel/intr/*` 重写 |
| 定时器 | ACPI HPET + TSC | Cortex-A9 global timer + private timer | `kernel/intr/hpet.cpp`、`cpu/delay.cpp` |
| 系统调用 | `syscall`/`sysret`，MSR STAR/LSTAR | `SVC`，VBAR 向量表 | `handler.S`、`syscall.cpp:173-213` |
| 每 CPU 数据 | `%gs` base + `swapgs` | `TPIDRPRW`（无需 swap，反而更简单） | `smp.cpp`、`scheduler.cpp`、`pcb.cpp` |
| 用户 TLS | `%fs` base + `arch_prctl` | `TPIDRURO` | `sys.cpp:1509-1528`、`pcb.cpp` |
| 浮点上下文 | `fxsave64` 512 字节 | VFP d0–d31 + FPSCR | `cpu/fpu.cpp`、`task/pcb.cpp` |
| 原子/屏障 | `lock btsq`/`mfence`/`pause`（TSO 模型） | `LDREX/STREX`/`DMB`/`WFE`（弱内存模型） | `include/cpu/lock.h`、`atom_queue.cpp` |
| ELF 类 | ELF64，`EM_X86_64` | ELF32，`EM_ARM` + `EF_ARM_EABI_VER5` | `user/user.cpp`、`dlinker.cpp` |
| 重定位 | `R_X86_64_*`，强制 `DT_RELA` | `R_ARM_*`，ARM 默认产出 `DT_REL` | `dlinker.cpp:1285-1462` |
| 启动 | UEFI `BOOTX64.efi`（PE/COFF） | BootROM → FSBL → U-Boot → ELF | `boot/` 全部 |
| 存储 | PCI AHCI / NVMe / IDE | SD/SDIO、QSPI | 驱动层 |
| 控制台 | 16550 @ I/O 端口 `0x3f8` | Cadence UARTPS @ MMIO `0xE0000000` | `driver/serial/serial_port.cpp:23` |
| 网络 | PCI e1000 | Cadence GEM | `kmod/e1000`、`netserver` |
| 输入 | PS/2 + xHCI USB | 无（需 USB 或 PL 侧扩展） | `driver/ps2`、`kmod/xhci` |

**关于 PCIe**：Zynq-7020 属于 cost-optimized 器件，PS 侧不带高速串行收发器（GTP），
因此**没有 PCIe 硬核**（PCIe Gen2 x4 出现在带 GTP 的 Z-7015/7030/7035/7045/7100 上；
请按你手上的具体器件与板卡再确认一次）。这意味着 **PCI/AHCI/NVMe/IDE/xHCI/HDA/e1000
这一整条 x86 PC 设备链在本平台上全部失效**，而不是"改改就行"。
`pci_setup(BootConfig.MCFG)`（`kernel/main.cpp:467`）依赖 ACPI MCFG，在 Zynq 上没有意义。

### 1.1 目标平台寄存器基址与中断号（实现时直接要用）

| 外设 | 基址 | 关键点 | GIC INTID |
|---|---|---|---|
| GIC Distributor | `0xF8F01000` | `ICDDCR` 使能、`ICDISER/ICDICER` 使能位、`ICDIPTR` 目标 CPU、`ICDICFR` 电平/边沿、`ICDICPR` 清挂起 | — |
| GIC CPU Interface | `0xF8F00100` | `ICCICR`、`ICCPMR` 优先级掩码、`ICCIAR` 应答、**`ICCEOIR` 结束中断** | — |
| SCU | `0xF8F00000` | CPU 索引、一致性控制 | — |
| Cortex-A9 Global Timer | `0xF8F00200` | 64 位、递增频率 = `CPU_3x2x / 2`（667MHz 的 Zynq 上是 333MHz） | 30 |
| Cortex-A9 Private Timer | `0xF8F00600` | 32 位，需做回绕累加 | 29 |
| UART0 / UART1 (UARTPS) | `0xE0000000` / `0xE0001000` | `CHANNEL_STS` @ `+0x2C` bit4 = `TFUL`（TX FIFO 满） | 59 / 82 |
| GEM0 / GEM1 | `0xE000B000` / `0xE000C000` | 自带 MDIO | 54 / 61 |
| SD0 / SD1 | `0xE0100000` / `0xE0101000` | SDHCI + ADMA2 | 56 / 45 |
| GPIO | `0xE000A000` | 常用于 PHY 复位、卡检测 | 52 |
| OCM | `0x00000000`（256KB） | AP 跳板、共享数据 | — |

**UART 波特率**：UARTPS 需要真实初始化（`UART_BAUDGEN`/`UART_BAUDDIV` 由 `CPU_1x` 推算，
`UART_CR` 置 TXEN|RXEN|RST_TX|RST_RX）。注意当前内核**从不调用 `init_serial()`**
（`include/proto.hpp:33` 有声明，`driver/serial/serial_port.cpp:25` 有定义，但无人调用），
COM1 是假定固件已初始化好的——在 Zynq 上必须在第一条日志之前显式初始化。

---

## 2. 工作分解

### 2.1 前置：建立架构抽象层

这是**第一件要做的事**，否则后面每一步都会变成"改一处、炸十处"。

建议新建 `arch/arm32/` 与 `arch/x86_64/`（或 `include/arch/`），提供统一接口：

| 接口 | 替代对象 |
|---|---|
| `cpu_relax()` | `pause`（`include/cpu/lock.h:100` 等 240 处） |
| 自旋锁/屏障 `spin_*`、`barrier_*` | `include/cpu/lock.h:19-101`、`kernel/atom_queue.cpp` |
| `irq_save/irq_restore`、`irq_disable/enable` | `pushfq`/`cli`/`sti`（`cpu/common.cpp:78-87`） |
| `ttbr_read/write`、`tlb_flush*` | `get_cr3`/`set_cr3`/`invlpg`（`include/cpu/regio.h:5-45`） |
| `page_table_*`（索引提取、标志编解码、表分配） | `kernel/memory/page.cpp` 的 `>>39/30/21/12` 与 `PTE_*` |
| `percpu_self()` | `get_current_cpu()`（`smp.cpp:256-269` 的 O(n) LAPIC-ID 扫描） |
| `context_switch` / `set_kernel_stack` | `scheduler.cpp:42-93`、`smp.cpp:287-314` |
| `irq_controller_ops`（mask/unmask/eoi/route/ipi） | `kernel/intr/apic.cpp`、`8259a.cpp` |
| 定时器 `nanoTime()` 提供者 | `kernel/intr/hpet.cpp`（已 `EXPORT_SYMBOL`，是最好的切入点） |
| `syscall_enter` / `user_mode_enter` 桩 | `handler.S:306-374`、`pcb.cpp:725-825` |

**同时必须解决一个高危 ABI 问题**：`kernel/intr/handler.S:29-33` 把 C 结构体偏移量**硬编码**了——

```asm
CPU_CURRENT_TASK     = 0x4c0   # PROCESSOR_INFO.current_task
CPU_SYSCALL_USER_RSP = 0x4e0
CPU_SYSCALL_USER_RAX = 0x4e8
THREAD_SYSCALL_STACK = 0xb48   # thread_control_block.syscall_stack
THREAD_SYSCALL_USER_RSP = 0xb50
```

这些偏移目前与 `include/smp/smp.h:35-49`、`include/task/pcb.h:158-219` 精确吻合，
但任何字段重排都会静默破坏系统调用路径。移植时应改为**由 `offsetof` 生成汇编头文件**。

### 2.2 启动链与链接布局

| 工作 | 说明 |
|---|---|
| 废弃 `boot/` UEFI 引导 | `boot/bootx64.c`（1,258 行）+ `boot/bootlib.c`（231 行）是纯 x86：端口 I/O、`cpuid`、MTRR 探测（`:1198`）、手工构造 PML4（`:836-901`）。`tools/gen_ninja.py:631` 用 `x86_64-w64-mingw32-gcc` + `--subsystem,10` 产出 PE/COFF，无 ARM 对应物。 |
| 新启动路径 | 两条可选：**(a)** FSBL + `BOOT.BIN`（bootgen: FSBL.elf + system.bit + u-boot.elf）→ U-Boot `bootelf` 加载内核 ELF；**(b)** 裸机 Zynq 启动头（汇编，禁 MMU，设栈，跳 `KernelMain`）。推荐 (a)，因为它顺带解决 DTB/镜像加载。 |
| 新 `linker.ld` | `OUTPUT_FORMAT("elf32-littlearm")`，32 位基址。注意 Cortex-A9 复位时 MMU 关闭、处于物理寻址。 |
| 内核入口约定 | `ENTRY(KernelMain)`（`linker.ld:2`）当前按 SysV AMD64 传参（`const FrameBufferConfig&, EFI_SYSTEM_TABLE&, BOOT_CONFIG&`）。ARM 是 AAPCS，且前两个参数在 Zynq 上不存在，需要重新设计 `KernelMain` 签名与 `BOOT_CONFIG` 的来源（DTB 或 U-Boot 环境变量替代 UEFI 协议）。 |
| `BOOT_CONFIG` 交接 | `include/efi/boot.h`（64 行）承载 MADT/HPET/MCFG/内存映射/MTRR/临时栈指针。Zynq 上这些字段大多无意义，需要重新定义：内存映射来自 DTB，CPU 数量固定为 2，无 ACPI。**注意 `boot/bootx64.c:1170-1187` 在没有 FADT/MADT 时会 `while(1)` 死循环**——这条路径必须移除。 |

### 2.3 内存管理与页表（**最大的横切改动**）

这是**单项风险最高**的工作包，因为 `PTE_*` 标志被多个子系统读取。

- `include/mm/page.h:3-13` 的页表标志词表必须按 ARMv7-A 重新设计：
  `PTE_PRESENT/WRITEABLE/USER/HUGE` 是 x86 位语义；
  `PTE_FRAME_ALLOCATED = 1ULL<<62`（软件位）和 `PTE_NO_EXECUTE = 1ULL<<63`
  在 ARMv7 短描述符里**没有对应位置**，LPAE 下 XN 是第二个字的 bit 0、AP 编码也不同。
  需要引入软件 PTE 影子表或复用位。
- `page_table_t = 512 × uint64_t` 的形状、`PML4 index 256` 内核/用户分界
  （`page.cpp:660,703,721`）、以及 `hhdm.cpp:50-84` 的 1GB/2MB 大页处理全部失效。
- 页错误处理读取 **CR2**（`page.cpp:345`）并解码 x86 错误码位（`:347-352`）；
  ARM 的 DFAR/IFAR + DFSR/IFSR 语义完全不同。
- `kernel/main.cpp:544` 的 `memset(phys_to_virt(get_cr3()), 0, PAGE_SIZE / 2)`
  直接擦除活动 PML4 的低半区——这是"知道自己是 x86 4 级页表"才敢写的代码，需要重写。
- 用户 VA 布局常量全部 > 4GiB，必须重塑：
  `CONFIG_USER_MMAP_START 0x400000000000`、`CONFIG_USER_BRK_START 0x700000000000`、
  `CONFIG_USER_BRK_END 0x7ffff0000000`、`CONFIG_USER_ELF_HEADER_START 0x300000000000`
  （`kernel/build_settings.h:84-97`、`include/task/pcb.h:142-150`）。
  使用点见 `page.cpp:866-890`、`pcb.cpp:1577`、`sys.cpp:1876,2116-2132,2364-2371`。
- 模块加载基址 `0xffffffffb0000000`（`dlinker.cpp:1158`）在 32 位空间不可达。
- 相关：`CONFIG_MAX_CPU_NUM 256`（`build_settings.h:73`）会静态分配
  `GDT_LENGTH = TSS + 256*2 + 1` 的每 CPU GDT 池（`include/pctable/gdt.h:22`），
  在 Zynq 上应降到 2。

**可保留**：`include/mm/uaccess.h` 用的是"页表翻译 + `phys_to_virt` + `memcpy"，
而不是 x86 的容错解引用/fixup 表（无 `stac/clac`），这个设计**天然可移植**，
只需替换 `PTE_*` 与 `translate_address` 的实现。

### 2.4 中断：IDT/APIC → GIC

- `kernel/pctable/idt.cpp`（84 行）、`gdt.cpp`（58 行，含裸描述符常量
  `0x00a09a0000000000` 等，`:33-37`）、`include/pctable/*` 全部废弃，改为
  VBAR + 8 项向量表 + CPSR 模式切换。
- `kernel/intr/apic.cpp`（238 行）、`8259a.cpp`（38 行）废弃，改为 GIC
  Distributor/CPU Interface 初始化、优先级、SPI/PPI 路由、EOI。
- **IRQ 注册目前是"直接调符号"而不是表驱动**：驱动自己调 `init_idt_desc`
  （`driver/hda/hda.cpp:590` 用 `0x20+irq`，`driver/sb16.cpp:39` 用向量 40），
  而 IOAPIC 重定向是**硬编码的 4 项表**（`apic.cpp:124-127`：32→GSI0、33→GSI1、
  34→GSI12、40→GSI5）。移植时应改成真正的 IRQ 描述符表
  （`include/openxj380/socket.h` 的键鼠钩子也依赖中断路径稳定）。
- `kernel/wsod/wsod.cpp`（163 行）是 x86 异常文本与 `CR0.EM/TS/MP` 改写，
  `backtrace.cpp:16` 走 `%rbp` 链并用 `0xFFFF800000000000` 判定用户/内核栈，均需重写。

### 2.5 定时器

- `kernel/intr/hpet.cpp`（57 行）基于 ACPI HPET MMIO 结构（`include/apic/hpet.h:46-57`），
  改为 Cortex-A9 Global Timer（`0x00F8F00200`）+ Private Timer + SCU。
- **`nanoTime()` 是 `EXPORT_SYMBOL` 导出的**（`hpet.cpp:16`），被调度器、
  `cpu/delay.cpp`（`:36-108`）、APIC 定时器校准（`apic.cpp:144-172`）、RNG 使用。
  重写它，大部分计时代码就跟着工作——**这是最划算的一个切入点**。
- `cpu/delay.cpp:12` 的 `rdtsc` 无 ARM 等价物（PMCCNTR 在 A9 上默认未使能且非架构保证），
  需改用 global timer。
- 注意 `driver/fs/vfs/procfs.cpp:653` 也直接读 `rdtsc` 暴露给 `/proc`。

### 2.6 SMP：INIT-SIPI → `sev` + OCM

> **阶段归属：本节内容已从 M3 拆出，后移到 AM3（After M3）执行。**
> 顺序理由见 §4：SMP 的风险随系统复杂度成倍放大，应当放在
> "描述层做完、调度器还没进来"的位置做，两个核面对的是同一份简单代码。
>
> **已提前完成的陆基**（在 M1/M2-5 期间顺手做掉了，AM3 不必再做）：
> - SCU 已使能（`cortexa9_coherency_init()`，M2-5b）—— 它当初是为了让 L1 能缓存
>   Shareable 的 DDR 而加，但正是 SMP 的前置条件；
> - ACTLR 的 SMP 位（bit6）与缓存/TLB 维护广播位（bit0）已置；
> - L1 + L2 缓存已使能并经基准验证（6~11x / 1.42x）；
> - `arch/cpu.h` 里 `spin_t` / `arch_ldrex` / `arch_strex` / `spin_lock` 原语已写好，
>   但**目前零调用者**——AM3 是它们的第一个用户。

- `kernel/smp/smp_trapo.S`（163 行）是**16 位实模式 → 32 位保护模式 → 64 位长模式**
  的完整 AP 引导跳板：`.code16` 起点、`smsw`/`lmsw` 设 CR0.PE、`lidt/lgdt` 私有 GDT、
  `rdmsr 0xC0000080` 置 EFER.LME、最后 `jmpq *%r8`。**在 Cortex-A9 上零复用价值。**
- `smp.cpp:186-254` 用 LAPIC ICR 发 INIT/SIPI，`APU_BASE_ADDR 0x10000`（`:43`）拷贝跳板，
  轮询 ready 标志**且没有超时**（`:237-251`）。
- ARMv7 方案：CPU0 写 OCM 中的跳板 + 设置 `0xFFFFFFF0`（或 Zynq SLCR 的 CPU1 复位向量），
  发 `sev`，CPU1 从 OCM 启动，开 SCU、设自己的 VBAR/TTBR、开 MMU、再 `sev` 回。
- `PROCESSOR_INFO`（`include/smp/smp.h:35-56`，255 项数组）需要去掉
  `gdt_entries_t[7]`、`gdt_pointer`、`tss0`、`tss_stack`（约 1,140 字节/CPU），
  换成 SCU/GIC 状态。
- 每 CPU 寻址从 GS base（`write_kgsbase`）改为 `TPIDRPRW`；`swapgs`（`handler.S:309,373`）
  整个消失——**这是 ARM 侧少有的"变简单"的地方**。

**AM3 才开始、但必须在第一天就定的一个设计点：PPI 是每核银行化的。**
GIC Distributor 里管 0–31 号的 `GICD_ISENABLER0`，**每个核看到的是自己那一份**——
CPU0 调 `gic_enable_irq(29)` 只影响 CPU0 的 bank。私有定时器正是 INTID 29。
现在 `gic.c` 的 `g_irq_table[96]` 是全局的，且 29 号只在 CPU0 上使能过。AM3 要决定：
PPI 的处理函数表按核分开，还是整表 per-CPU、SPI 注册时同步到所有核。
**这个决定必须在写 CPU1 引导之前做，否则后面返工。**

### 2.7 上下文切换与浮点

- `kernel/task/scheduler.cpp:42-93` 的 `save_registers()` 是 `naked` + `.intel_syntax` 内联汇编，
  它把 `%rsp` 交给 C 代码、再用返回的 `%rsp` 做 `iretq`——**"交换中断返回帧"来实现任务切换**，
  这是典型 x86 设计。ARMv7 应改为 `cpu_switch_to` 式的被调用者保存寄存器切换，
  这个技巧整体消失。
- `kernel/task/pcb.cpp:725-825` 与 `:829-861` 手工压 `SS/RSP/RFLAGS/CS/RIP` 再 `iretq` 进入用户态，
  且用户入口约定是 **SysV AMD64**（`argc→rdi`、`argv→rsi`、`envp→rdx`，`:761`）。
  ARM 改为设置 `SPSR`/`SP_usr` + `movs pc, lr`，参数进 `r0/r1/r2`。
- `create_user_thread()`（`pcb.cpp:2265-2402`）硬编码 `context0.cs = 0x8`、
  `ss = es = ds = 0x10`、`rflags = 0x202`（`:2313-2314,:2370-2371`）——段寄存器概念在 ARM 不存在。
- 浮点：`fxsave64`/`fxrstor64`（`cpu/fpu.cpp:5,10`）与 512 字节 `fxsave_area`
  （`include/cpu/fpu.h:7`，用于 `pcb.cpp:1231,2089,2375`）改为 VFP `vstm/vldm` d0–d31 + FPSCR，
  并需要开 CPACR/FPEXC（替代 `main.cpp:295-302` 的 `CR0.EM/TS/MP` 与 `wsod.cpp:41-45`）。
  `xsetbv`/XCR0（`main.cpp:309-319`）直接消失。
- 注意 **Zynq-7020 的 NEON/VFP 是可选配置**，需与 bitstream 一致，并在 `AT_HWCAP`
  里如实上报（见 2.9）。

### 2.8 系统调用 ABI

**入口/出口**：
- 用户侧唯一的系统调用指令是 `user/xapi/libsys.cpp:3-15` 的 `syscall`，
  `%rax`=号，参数 `rdi/rsi/rdx/r10/r8/r9`，返回 `%rax`，硬件破坏 `rcx/r11`。
- 内核侧 `kernel/syscall/syscall.cpp:173-213` 用
  `wrmsr(MSR_LSTAR, syscall_handler)` + `EFER.SCE` + `STAR` + `SYSCALL_MASK`，
  并且**在使能前校验 GDT 选择子关系**（`:182-197`）——纯 x86 sysret 语义。
- `kernel/intr/handler.S:306-374` 的 `swapgs`/`sysretq` 路径需要整体重写为 SVC 处理器。
- 由于 `SYSCALL` 不压栈，C 侧要**合成**返回帧：`regs->rip = regs->rcx`、
  `regs->rflags = regs->r11`（`syscall.cpp:216-222`）。ARM 的 SVC 会给出真实异常帧，
  这段合成逻辑消失。

**寄存器帧**：`struct X64_REGS`（`include/cpu/longm.h:9-35`，24×`uint64_t`）是内核的 `pt_regs`，
被 `handler.S:5-28` 按位置镜像。仅 `kernel/syscall/syscall.cpp` 一个文件就有
**约 679 处 `regs->r*` 访问**。建议引入 `include/arch/<arch>/syscall_regs.h`，
把 ARM `r0-r6` 映射到既有字段名，让 dispatch 与 handler 体基本不动——
这是把"大改"降级为"机械替换"的关键。

**系统调用号（必须做决策）**：
- `include/syscall/syscall.h:226-411` 与 `user/xapi/include/libsys.h:8-149` 就是
  **Linux x86_64 的 `__NR_*` 编号**（`SYS_READ 0`、`SYS_WRITE 1`、`SYS_MMAP 9`、
  `SYS_ARCH_PRCTL 158`、`SYS_CLONE3 435`…）。ARM EABI 完全不同
  （`write 4`、`open 5`、`mmap2 192`、`set_tls 0x0f0005`）。
- **硬阻塞**：SXAH 私有号是 **57 位**（`include/syscall/pxapi.h:25-43`）：
  `SXAH_SYSCALL_RETURN 128956723895689201`（≈1.29e17 > 2^56）。
  **装不进 32 位的 `r7` 寄存器，必须重新编号。**
- 需要决策：是保留 Linux x86_64 号（并为一个 ARM 用户态做号翻译层），
  还是给 ARM 定义独立号表（放弃直接运行现成 ARM Linux 二进制）。
  建议后者 + 把 SXAH 重编到高位区间，代价是需要重新构建全部用户态。

**信号帧**：`struct user_signal_frame`（`signal.cpp:7-14`）内嵌完整 `X64_REGS`，
`include/syscall/signal.h:153-181` 的 `signal_frame_t` 是**逐字段照抄的 x86_64 Linux `sigcontext`**
（含 `trapno`、`cr2`、`fpstate`）。ARM 的 `sigcontext` 是 `r0-r15` + `cpsr` + VFP 状态，
且需要 `__NR_rt_sigreturn`（173）路径。

**fork/clone 里的 x86 泄漏**：`init_fork_child_context()`（`pcb.cpp:1857-1881`）
写 `child->context0.rip = frame->rcx`、`rflags = frame->r11`——
这是 `sysretq` 寄存器契约渗进了 fork 语义。`CLONE_SETTLS`（`:1236`）设 `fs_base`，
ARM 下应设 `TPIDRURO`。

### 2.9 用户态、ELF32 与预置二进制

- **ELF 加载器只认 ELF64**：`kernel/user/user.cpp:555-683` 全程 `Elf64_Ehdr`/`Elf64_Phdr`，
  且用 `e_phentsize != sizeof(Elf64_Phdr)`（`:563`）作为事实上的 64 位闸门。
  **它从不检查 `e_machine`，也从不检查 `EI_CLASS`。**
  需要引入按 `EI_CLASS` 选择的 `Elf32_*`/`Elf64_*` typedef 层。
  固定装载基址 `USER_DYN_MAIN_BASE 0x40000000`、`USER_INTERP_BASE 0x60000000`
  （`user.cpp:482-483`）在 32 位下要重新分配。
- `include/elf.h` **已经具备一切**：`ELFCLASS32`（`:92`）、`EM_ARM 40`（`:164`）、
  `Elf32_Sym`（`:460`）、完整的 `R_ARM_*`（`:2521-2651`）。**不需要改头文件。**
- **预置二进制必须全部重建**（已逐个读 ELF 头确认均为 ELF64 `EM_X86_64`）：
  busybox（`ET_EXEC`，2.4MB）、fastfetch、`ld-musl-x86_64.so.1`、`libc.so`、
  `libc.musl-x86_64.so.1`、`libgcc_s.so.1`。busybox 源码包已随仓库
  （`third_party/busybox-source/`，1.31.1，含 `build.sh`/`.config`），可重建。
- **用户态启动极简**：`user/xapi/arch/x86_64/crt0.S` 只有 9 行，
  做 `endbr64; jmp xapi_start`。栈和参数全部由内核准备好，所以 ARM 版几乎一样简单
  （`b xapi_start` + `.syntax unified` + Thumb 决策）。需要去掉
  `endbr64`（x86 CET）和 `__attribute__((force_align_arg_pointer))`
  （`user/xapi/constart.cpp:23`，x86 专有；AAPCS 只要 8 字节对齐，内核已按 16 对齐，
  兼容）。
- **栈上传参需改**：`build_user_stack()`（`pcb.cpp:539-723`）里有
  `static const char platform[] = "x86_64";`（`:584`）要改成 `"armv7l"`；
  `AT_HWCAP` 被**硬编码为 0**（`:666`），必须填真实 ARM HWCAP（VFP/NEON/Thumb），
  否则 musl 不会启用 VFP 路径。
- 用户态**没有树内 libc**：`user/xapi/include/xposix/*` 只有声明，
  `__xposix_ret`/`__xposix_ptr`（`xposix/syscall_ret.h:10-11`）在全仓库只有声明、无定义。
  而且 `xposix/sys/stat.h:48-63` 编码的是 **x86_64 Linux `struct stat` 布局**（含 `char _pad[24]`），
  32 位 ARM 上 `long` 是 32 位、布局不同，需要架构分叉。

### 2.10 动态链接器与可加载模块

- `kernel/dlinker.cpp:224-239` 的 `elf_test_head()` 是**全仓库唯一的 `EM_*` 检查**，
  只接受 `EM_X86_64` / `EM_386`——所有 `.sys` 模块与动态用户对象在 ARM 上会被直接拒绝。
- **最大阻塞**：`:1317-1324` **强制要求 `DT_RELA`**，遇到 `DT_REL` 直接失败。
  而 32 位 ARM 的 `ld` 生成共享对象时**默认产出 `DT_REL`/`Elf32_Rel`**（8 字节，无 addend）。
  必须在 `process_dynamic_relocations()`（`:1285-1462`）里补 `DT_REL` 支持，
  或在链接时强制产出 RELA。
- 需要新增的 `R_ARM_*` 类型（至少）：
  `R_ARM_NONE/ABS32/REL32/COPY/GLOB_DAT/JUMP_SLOT/RELATIVE/GOTOFF/GOTPC/GOT32/PLT32/
  CALL/JUMP24/PREL31/MOVW_ABS_NC/MOVT_ABS/MOVW_PREL_NC/MOVT_PREL/GOT_PREL/IRELATIVE`，
  若允许 Thumb-2 模块还需 `R_ARM_THM_JUMP24`、`THM_MOVW_ABS_NC`、`THM_MOVT_ABS` 等。
  当前 `:1432-1434` 对未识别类型是**硬失败**。
- GNU hash 的 bloom 字宽写死了 `uint64_t`（`:708`）——ARM 是 32 位字。
- 模块基址 `0xffffffffb0000000`（`:1158`）在 32 位不可达；
  且模块通过 `.ksymtab` 按符号名调用内核函数，要保证 `B`/`BL` 的 ±32MiB 跳转范围。
- **好消息**：C++ 名字改编（`dlinker_mangled_global_matches()`，`:462-474`）
  是 Itanium ABI，架构中立，可原样保留。
- lwIP 胶水 `kmod/netserver/arch/sys_arch.cpp` 有 4 处 x86 汇编
  （`pushfq`/`popfq`/`pause`/`cli`），量很小。

### 2.11 平台驱动（全新代码）

#### 先明确一件事：这个项目没有驱动模型

全仓库**不存在** `struct bus` / `struct driver` / `probe()`/`match()` / 设备树 /
resource descriptor 中的任何一种。唯一的"发现机制"是 PCI（ACPI MCFG ECAM，
否则 legacy `0xCF8/0xCFC`），其余全是固定 ISA 端口探测。

**唯一可供新驱动挂接的通用抽象只有两个**，它们都是平台中立的，这是好消息：

| 抽象 | 位置 | 用途 |
|---|---|---|
| `device_t` + `regist_device()` | `include/device.h:21-45`、`driver/device.cpp:163-179` | 块设备/字符设备。256 项扁平数组 `device_ctl[256]`（`device.cpp:12`）。`DEVICE_BLOCK` 注册后**自动触发分区扫描**（`device.cpp:174-176`）。 |
| `netdev_t` + `regist_netdev()` | `include/netdev.h:4-23`、`kernel/netdev.cpp:6-35` | 网卡。只有 `mac[6]`、`mtu`、`desc`、`send`、`recv` 五个字段。 |

**块层契约很干净**，实现 SD 驱动就是填回调：

```
FATFS → vfs_read → devfs_read → blk_device_read → rw_device → device_t.read/write
```
- `rw_device()`（`driver/device.cpp:238-270`）在内核侧分配回弹缓冲区，
  按 `SECTORS_ONCE=8`（`include/device.h:3`）分块调用驱动。
- `blk_device_read/write`（`device.cpp:318-528`）处理首尾非整扇区，
  并在调用者缓冲区物理连续时直接透传（`blk_direct_span_bytes`，`:113-140`）。
- **传给驱动的缓冲区是物理连续的**，其物理地址由 `driver_virt_to_phys()` 得到
  （注意有两个直接映射窗口：`phys_to_virt` = phys + `0xffff800000000000`，
  `driver_phys_to_virt` = phys + `0xffffb00000000000`，`hhdm.cpp:9-11,38-49`），
  所以 ADMA2 描述符可以直接指向它。
- FatFs 侧：`FF_VOLUMES 10`、512 字节逻辑扇区固定（`driver/fs/fatfs/diskio.cpp:72,81,144`）、
  已开 exFAT + LFN，SDXC 卡可用。**不需要改 FATFS。**

**新增 Zynq SD 驱动的实际工作量**：内核集成部分很小（一天量级），
主要成本在 SDHCI/ADMA2 控制器逻辑**和缓存维护**（见下）。

#### 缓存维护：一个完全缺失的 API，也是 ARM 上 DMA 的成败点

x86 是缓存一致的，所以代码里**没有任何缓存维护 API**——全仓库只有 x86 专有的
`wbinvd`（`driver/hda/hda.cpp:354,527`、`kernel/cpu/mtrr.cpp:63,110`）。
Cortex-A9 与 PS/PL 侧 DMA 之间**不是自动一致的**，所以 SD 和 GEM 驱动必须：
- RX：在读取缓冲区前 `invalidate`；TX：在触发门铃前 `clean`，描述符环同样处理。
- 或者把 DMA 缓冲/描述符环映射为**非缓存**属性。但注意：x86 侧用的是
  `PTE_PRESENT|PTE_WRITEABLE|1<<3|1<<4`（PWT/PCD，见 `kmod/xhci/xhci.h:20`），
  **这套页属性在 ARMv7 上不存在**——而页表层本来就要重写（见 2.3），
  所以这项工作要并入 MMU 阶段一起做，不能拖到驱动阶段。

> ✅ **更新：这块不用从零写。** Xilinx BSP 已经提供了经过验证的实现
> （`Xil_DCacheFlushRange` / `Xil_DCacheInvalidateRange`），
> 并且内含 **PL310 勘误 588369 与 727915** 的处理——后者正是
> "Background Clean and Invalidate by Way 导致数据损坏"，DMA 场景的典型杀手。
> MIT 许可，可直接 vendor。详见 **§2.15**。

#### 各驱动对照表

| 驱动 | 现状 | Zynq 需要 |
|---|---|---|
| 串口控制台 | 16550 @ `0x3f8`，`driver/serial/serial_port.cpp:23`，纯 port I/O | Cadence UARTPS @ `0xE0000000`，MMIO。**只有 `write_serial(char)` 一个函数必须改**（`include/proto.hpp:34`）：轮询 TX FIFO 满再写。上层 `write_serial_string/_fmt/printk/pr_*` 全部保持。另需新增 `init_serial()` 调用。**注意目前完全没有串口 RX 路径**，若要把 UART 做成 tty 需新写一个 `DEVICE_STREAM` 设备。 |
| 块存储 | AHCI/NVMe/IDE（全部 PCI） | SD/SDIO 控制器驱动（ADMA2 描述符）→ 注册 `device_t`；QSPI 作为备选 |
| 网络 | PCI e1000（`kmod/e1000`）+ lwIP | Cadence GEM 驱动（非 PCI，自带 DMA 描述符环）替换 e1000。**因为 `netdev_t` 契约与硬件无关，`netserver.cpp` 可以零改动** |
| 输入 | PS/2 键鼠 + xHCI | 平台无 PS/2；USB 需 xHCI（Zynq PS 是 USB 2.0 OTG，不是 xHCI）或 PL 侧扩展 |
| 音频 | HDA + SB16 | 无对应，建议直接禁用 |
| RTC | `driver/rtc.cpp`（CMOS） | Zynq 无 RTC，需外部芯片或去掉 |
| 定时/中断 | APIC/HPET/8259A | GIC + Cortex-A9 定时器（见 2.4/2.5） |
| 电源 | `driver/power.cpp` 用 EFI runtime + `lidt`/`int3` 触发崩溃（`:282-283`） | 需重写（PSCI 或 SLCR 软复位） |

**建议先做 headless**：项目本身支持 `OPENXJ380CONFIG_CLEAR_RUN` 宏
关闭图形/控制台/输入输出（见 `README.md`，`include/openxj380/config.h`）。
用它把图形、键鼠、USB、HDA 全部关掉，只保留串口 + 存储，能显著降低第一阶段的面积。

### 2.12 开工前必须先修的三处既有地雷

这三处是**当前代码里就存在的缺陷**，在 x86 上被"值恰好非零"掩盖住了，
一旦脱离 UEFI/ACPI 环境就会立刻变成死机。它们的共同特征是**用零值当哨兵，
却又解引用它**。必须在做 Zynq bring-up 之前修掉，否则会把移植问题误判成移植 bug。

| # | 问题 | 位置 | 后果 |
|---|---|---|---|
| 1 | `init_hpet(BootConfig.HPET)` 在 `HPET == 0` 时仍然解引用该指针：`info = (HpetInfo *)(((Hpet *)hpet_ptr)->base_address.address + 0xFFFF800000000000)` | `kernel/intr/hpet.cpp:47-51`（调用点 `kernel/main.cpp:446`） | `struct hpet` 头部 36 字节 + `event_block_id` 4 → 会读取**物理偏移 44 处**的 64 位值当 MMIO 基址。`BootConfig` 是清零的（`boot/bootx64.c:1077`），且**只有 MADT 是强制的**（`:1179`），所以 HPET 确实可能为 0。 |
| 2 | `pci_setup(0)` 把 `mcfg` 设成 `0xffff800000000000` 而不是 NULL | `driver/pci/pci.cpp:621-624`，消费点 `:626-651` | `pci_init()` 因此**永远不会走 legacy `0xCF8/0xCFC` 回退路径**，转而按垃圾 `mcfg->Header.Length` 做 ECAM 枚举。"没有 PCI 总线"这个状态目前不被支持。 |
| 3 | `mount_root()` 找不到根设备时 `while(1) pause;` 死循环 | `driver/fs/partition.cpp:658-689` | Zynq 上缺 SD 驱动 = 启动直接挂住无任何提示（且 `pause` 是 x86 指令）。应改为打印诊断并进入可调试的失败路径。 |

另外顺带记录一个设备层既有缺陷：`driver/ahci/ahci.cpp:559` 与 `driver/nvme/driver.cpp:539`
使用了**未初始化的栈上 `device_t`**，其 `path` 字段是野指针，会在
`driver/device.cpp:194-198` 被 `free()`。新写的 SD 驱动**务必先 `memset` 清零**再填字段
（`docs/DRIVER_GUIDE.md:213-238` 有说明）。

### 2.13 构建系统与镜像

> 📌 **编译器的选择与完整标志集已单独整理在 §6.7「工具链建议（实测决策清单）」**，
> 结论是 ARM 侧用 Vitis 的 GNU 工具链（clang 的集成汇编器编不了 BSP 的
> `asm_vectors.S` / `boot.S`）。本节只讲构建图与镜像流程要怎么改。

**构建图完全未参数化**——`tools/gen_ninja.py` 里没有任何 `ARCH`/`--target` 变量。

- 需要逐条替换的 x86 专有标志（`tools/gen_ninja.py:580-589`）：
  `-m64`、`-mno-red-zone`、`-mstackrealign`、`-mno-80387`、
  `-mcmodel=large`（在规则体里，`:627-629`）、
  NASM `-f elf64 -DX86_64_TARGET -DUEFI`（`:580`，注意当前树里实际**没有 `.asm` 文件**，
  kernel/driver/lib 是 100 个 `.cpp` + 2 个 `.S`）、
  `objcopy -O elf64-x86-64`（`:632`）、
  `-Ttext=0x200000`（`:647`）。
  目标侧应加 `--target=armv7a-none-eabi -march=armv7-a -mcpu=cortex-a9 -marm
  -mfpu=vfpv3 -mfloat-abi=hard -mlong-calls`（浮点选项对齐 AMD 官方 BSP，见 §6.1）。
- ⚠️ **内核链接必须补 `-lgcc`**：ARM 没有硬件整数除法，32/64 位除法会生成
  `__aeabi_uidiv` / `__aeabi_uldivmod`，而当前 `ld -T linker.ld --static`
  （`:633-638`）不链 libgcc。x86 上没问题，ARM 上会**直接链接失败**。
  且 `-nostdlib` 不会自动带入 libgcc，必须显式写。详见 §2.15。
- 工具链不对称问题：内核用 clang/clang++，但**用户态 C++ 默认用宿主 `g++`**（`:535`），
  且都直接用裸 `ld`（`:531,:536`）。交叉编译必须统一。
- `out/BOOTX64.efi` 的 `boot` 规则（`:631`）用 `x86_64-w64-mingw32-gcc` + `--subsystem,10`
  产出 PE/COFF，**无 ARM 对应物**，整条规则删除。
- `liballoc-x86_64.a` 需要为 `armv7a-none-eabi` 重建（`RUST_TARGET` 默认
  `x86_64-unknown-none`，`:518`；`RUST_FLAGS` `:586`），
  并同步更新 `third_party/compliance-manifest.json`、`licenses/liballoc.txt`。
- **两处硬编码的开发者个人路径**：`gen_ninja.py:140` 与 `ninja_build.py:510`
  都含 `/home/leon/.rustup/...`。
- 磁盘镜像：`ninja_build.py:296-310` 用 `sgdisk -t 1:ef00` + `mkfs.vfat` + `mcopy` 造
  GPT/ESP 镜像，把 `BOOTX64.efi` 放到 `\EFI\BOOT\`、内核放 `\system\kernel.krl`。
  Zynq 需要改为生成 `BOOT.BIN`（bootgen）+ `uEnv.txt`/`boot.scr`，
  FAT 分区本身可保留（U-Boot 能读 FAT）。
- QEMU 目标：`qemu_cmd()`（`ninja_build.py:355-381`）整函数重写为
  `qemu-system-arm -M xilinx-zynq-a9 -cpu cortex-a9 -kernel <elf> -serial mon:stdio`；
  去掉 `-bios OVMF`、`--enable-kvm -cpu host`、`qemu-xhci`、`e1000`、
  `ich9-intel-hda`、AHCI/IDE 磁盘，改用 `-drive if=sd`。
  **QEMU 的 `xlnx-zynq` 机器模型本身不含 PCIe/PS2/ISA**，与真机一致。
  注意这里用的是**主线 QEMU**，而本机 Vitis 里并没有可用的 QEMU——
  详见 §6.3。
- `check.tools`（`ninja_build.py:547-607`）与两个 CI workflow 需要 ARM 分叉
  （`arm-none-eabi`/clang ARM target、`qemu-system-arm`、`bootgen`、`dtc`；
  `nasm` 与 mingw 变成死重量）。
- 注意 `out/` 与 `XJ380.img` 是**扁平命名、无架构后缀**，两套构建会互相覆盖。
- **另一处已有问题**（顺带提醒）：`tools/` 实际只有 5 个脚本，但 `ninja_build.py`
  引用了 `stage_elf_deps.sh`、`stage_image_toolchain.sh`、`stage_image_xbps.sh`、
  `stage_prepared_root.sh`、`stage_xbps_bootstrap.sh`、`make_pak.py`、
  `make_installer_iso.sh`，且 `Bf/` 整个目录不存在。
  所以 `ninja complete|installer.iso|prepare` 现在就是坏的，只有 `ninja vdisk` 可用。
  移植时不要把工作建立在那些目标上。

### 2.14 测试与 CI

会被架构绑定打断的测试：

- `tests/test_license_provenance.py:77-104` — 对 `liballoc-x86_64.a` 跑 `nm -a`
  并断言 **x86_64 Rust 改编符号** `_RNvMs0_NtCs1PPGXRNTyC3_4talc4talc` 存在。
- `tests/test_busybox_compliance.py:13-38,62` — 钉死 busybox 的 sha256，
  并且**真的执行该二进制**（`subprocess.check_output([busybox, "--list"])`）。
  ARM 版 busybox 无法在 x86_64 CI 宿主上运行。
- `tests/test_license_compliance.py:85-110` — 同样的 busybox 断言。
- `tests/test_gen_ninja.py:42-62` — 断言规则名（`xapi_cxx`/`user_cxx_custom`/`user_ld`）
  与 `user/` 目录集合；`tests/test_makefile_logs.py:9-20` 断言
  `rule nasm|root_as|root_cc|root_cxx`。**改规则名会打断它们。**
- `.github/workflows/build.yml:34-51,54,63,69-74` 与 `codeql.yml:25-45` —
  x86 专有工具链 + `mdir` 检查 `BOOTX64.efi`/`kernel.krl`。
- `tests/test_ninja_build.py:66-95` — 断言 OVMF `-bios` 字符串。

**CI 只构建、不启动系统**（`AGENTS.md` 明示 "Do not assume root CI boots the OS"），
所以移植后必须**自己补一个 QEMU 冒烟启动测试**，否则回归无法发现。

### 2.15 Xilinx BSP 可复用件与 ARM 链接注意点（实测）

本节回答两个具体问题：**链接到底正常吗？Xilinx 那些中断/定时器库能不能用？**

#### 结论一：链接正常，但必须补 `-lgcc`

用 Vitis 自带的 `arm-none-eabi-g++` 按本项目的风格（`-ffreestanding -nostdlib
-fno-exceptions -fno-rtti` + 自定义 linker script）实测：

| 配置 | 结果 |
|---|---|
| `-nostdlib` 直链（照搬 x86 的做法） | **失败**：`undefined reference to '__aeabi_uldivmod'` |
| 追加 `-lgcc` | **成功**，且只引入 `__aeabi_uldivmod`（外加弱符号 `__aeabi_idiv0`/`__aeabi_ldiv0`） |

产物校验：`ELF32 / ARM / Entry 0x10004c / Flags 0x5000400 = Version5 EABI + hard-float ABI`，
`.text`、`.bss` 段地址正确。

**这是 ARM 特有的坑**：ARM 没有硬件整数除法指令，32 位除法生成 `__aeabi_uidiv`、
64 位生成 `__aeabi_uldivmod`，它们由 `libgcc` 提供。当前 x86 构建用的是
`ld -T linker.ld --static`（`tools/gen_ninja.py:633-638`），**不链接 libgcc**——
在 x86 上没问题（有 `div` 指令），搬到 ARM 会直接链接失败。

⚠️ **注意 `-nostdlib` 会阻止 `libgcc` 自动加入**，必须显式写 `-lgcc`
（或自己实现这几个 `__aeabi_*`）。这条要一并写进 §2.13 的 ARM 标志集。

#### 结论二：Xilinx 的库能用，而且比预期更值得用

**许可：全部是 MIT**（`SPDX-License-Identifier: MIT`），与项目 Apache 2.0 兼容，
可以放心 vendor，只需在 `THIRD_PARTY_NOTICES.md` 登记
（项目有 `tests/test_license_provenance.py` 会做校验）。

驱动源码位置（本机）：
`C:\AMDDesignTools\2025.2\data\embeddedsw\XilinxProcessorIPLib\drivers\`
BSP 板级支持（含 cache/errata/CP15）：
`C:\AMDDesignTools\2025.2\data\embeddedsw\lib\bsp\standalone_v9_4\src\arm\cortexa9\`

| 可复用件 | 规模 | 建议 |
|---|---|---|
| `scutimer_v2_7`（SCU 私有定时器） | `xscutimer.c` **仅 267 行，且只 include 自己的头文件，零外部依赖** | ✅ **直接 vendor**。几乎零成本，省掉 M3 的一块工作 |
| `xreg_cortexa9.h`（CP15 寄存器全集） | 578 行 | ✅ **直接 vendor**。省掉大量查手册时间，写 MMU/缓存/MPIDR 都要用 |
| `xil_cache.c` + `xil_cache.h` + `xil_cache_l.h`（缓存维护） | 1,550 + 107 + 75 行 | ✅ **最高价值**。见下 |
| `xl2cc.h`（PL310 L2 缓存控制器定义） | 155 行 | ✅ vendor |
| `xil_errata.h` + 勘误实现 | 101 行 | ✅ **必读**。见下 |
| `xtime_l.c/.h`（`XTime_GetTime()` 全局定时器） | 100 + 86 行 | ✅ 可参考/复用，对应方案的 `nanoTime()` 替换 |
| `xil_mmu.c/.h`（段式页表） | 208 + 92 行 | ⚠️ 可作起点，但本项目要的是完整 VM 系统，只能借鉴 |
| `scugic_v5_6`（SCU GIC） | `xscugic.c` 1,586 + `xscugic_hw.c` 1,120 + `xscugic_intr.c` 163 + 头 665 行 | ⚠️ **可用但要挑**，见下 |
| `libxil.a`（190KB）/ `libxilstandalone.a`（106KB） | — | ❌ **不要整体链接**。按需 vendor 源码 |

#### 最重要的一块：缓存维护 + 勘误

§2.11 指出本项目**完全没有缓存维护 API**（只有 x86 的 `wbinvd`），
而 Cortex-A9 与 SD/GEM 的 DMA 之间不是自动一致的。**Xilinx 已经把这块写好了**：

`xil_cache.h` 提供的正是需要的东西：
```c
void Xil_DCacheFlushRange(INTPTR adr, u32 len);       // TX / 描述符：clean
void Xil_DCacheInvalidateRange(INTPTR adr, u32 len);  // RX：invalidate
void Xil_DCacheEnable(void);  void Xil_DCacheDisable(void);
void Xil_ICacheEnable(void);  void Xil_ICacheInvalidateRange(INTPTR adr, u32 len);
```

**比 API 更值钱的是里面已处理的勘误**。`xil_errata.h` 明确列出并实现了：

| 勘误 | 后果 | 实现在 |
|---|---|---|
| **ARM 775420** | 数据缓存维护操作若发生中止，**可能死锁** | `asm_vectors.S:147,162`（数据中止处理） |
| **ARM 794073** | MMU 关闭时的推测取指可能不符合架构要求 | `xil_mmu.c:165` |
| **PL310 588369** | Clean & Invalidate 操作**不会使 clean 行失效** | `xil_cache.c:1473` |
| **PL310 727915** | Background Clean and Invalidate by Way **会导致数据损坏** | `xil_cache.c:150` |

其中 **727915 和 588369 正是 DMA 场景下会踩的**——SD 卡和网卡的收发缓冲正好是
"clean/invalidate by way" 的高频调用者。Xilinx 注释也说明这些实现
"follows ARM guidelines and is based on the open source Linux support for these errata"，
即已是经过 Linux 长期验证的写法。**自己从零踩这几个坑，代价是几天量级的
"神秘挂死/数据损坏"排查。**

#### 关于 `xscugic`：可以用，但要挑着用

一个关键的好消息：**`xscugic.c` 完全不引用 `Xil_Exception`**（已实测确认）。
这意味着：

- ✅ 可以只用 `XScuGic_*` 做 Distributor / CPU Interface 的寄存器操作，
  **同时保留本项目自己的向量表（VBAR）与 SVC/IRQ 分发**——
  这正是 §2.4 与 §2.8 要求的（OS 必须自己拥有异常向量，否则系统调用和上下文切换没法做）。
- ❌ 不要引入 `Xil_Exception` / `Xil_ExceptionRegisterHandler`，
  它会接管异常向量，与本项目的 syscall 入口直接冲突。
- ❌ 不需要 `xscugic_selftest.c`、`xscugic_sinit.c`、`xscugic_g.c`
  （后两者是"按 device ID 查配置表"的样板，OS 里没有 BSP 那套 config table）。
- 🤔 但要有心理准备：`xscugic` 合计约 3,500 行，而 OS 真正需要的
  GIC 操作（初始化、enable/disable、ack/EOI、优先级、SPI 路由）大约 200–300 行。
  **建议：把 `xscugic_hw.h` 当寄存器字典参考，自己写精简的 GIC 层**，
  这样能顺带把 §5 提到的 `INTID → handler` 注册表和导出需求一并设计进去。

#### 落地建议

在 `arch/arm32/`（或 `third_party/xilinx-bsp/`）下按 MIT 许可 vendor：

```
xil_cache.c/.h/_l.h     ← 缓存维护 + PL310 勘误（最高优先级）
xil_errata.h            ← 勘误开关
xreg_cortexa9.h         ← CP15 寄存器定义
xl2cc.h                 ← PL310 定义
xscutimer.c/.h/_hw.h    ← SCU 定时器（267 行，几乎白送）
scugic_hw.h             ← GIC 寄存器字典（仅参考）
```

并同步更新 `THIRD_PARTY_NOTICES.md`、`LICENSES.md` 与
`third_party/compliance-manifest.json`（沿用项目现有合规流程）。

> ABI 一致性提醒：这些 BSP 文件按 **hard-float** 编译
> （`cortexa9_toolchain.cmake` 里 `-mfpu=vfpv3 -mfloat-abi=hard`）。
> vendor **源码**没有这个问题；但若直接链 `libxil.a`，浮点 ABI 必须完全一致。

---

### 2.16 设备描述层（类 DTS）—— M3 的主体

#### 为什么必须有这一层

x86 侧**没有驱动模型**：不存在 `struct bus`/`struct driver`/`probe()`/`match()`/
设备树/resource descriptor 中的任何一种。唯一的发现机制是 PCI 枚举
（MCFG ECAM，否则 legacy `0xCF8/0xCFC`），其余全是固定 ISA 端口探测。

而 ARM 这边**连枚举这个概念都不成立**：

| | x86_64 | Zynq-7020 |
|---|---|---|
| 发现方式 | PCI 枚举 / ACPI 表 —— 硬件必须回答"你是谁" | **只能被告知** |
| 地址 | BAR，运行期分配 | Vivado 地址编辑器写死 |
| 中断号 | MSI / ACPI `_PRT` | 从 `xparameters.h` 抄 |
| IP 参数 | 标准 capability 寄存器 | **`xlnx,is-dual` / `xlnx,gpio-width` 读不出来** |

最后一行是关键：AXI GPIO 的数据寄存器宽度恒为 32 位、通道数不体现在任何 ID
寄存器里——**硬件本身不告诉你它是什么**。所以描述不是可选优化，是唯一的信息来源。

**没有枚举还有一个后果**：x86 上漏配一个设备，枚举照样能把它找出来；
这里**描述表就是全部**——少写一个节点，驱动静默不加载且不报错。
所以描述层必须自带启动时打印全部节点的能力，这是唯一能在"驱动没起来"时
区分"驱动写错了"和"节点漏写"的手段。

#### 描述模型（照 DTS 语义）

```
描述模型  ← 照 DTS：节点树 / compatible / reg / interrupts / 属性 / status
落地形式  ← C 表 · DTB · 从 xparameters.h 生成   ← 三者可换，不影响模型
```

**这两个轴是独立的。** 先把模型定死，落地形式怎么换都不动驱动代码。

```c
/* DTS 属性 —— 节点里的任意键值 */
typedef struct {
    const char *name;      /* "xlnx,is-dual"                    */
    uint32_t    value;     /* 1                                 */
} plat_prop_t;

/* DTS 节点 */
typedef struct plat_device {
    const char        *name;        /* "uart1"          (node name)      */
    const char        *compatible;  /* "xlnx,ps7-uart"  (compatible)     */

    uintptr_t          reg_base;    /* 0xE0001000       (reg = <base size>) */
    size_t             reg_size;

    int32_t            irq;         /* **已解码的 GIC INTID**            */
    uint32_t           irq_flags;   /* 1=电平 3=边沿    (interrupts)     */

    const char        *clocks;      /* "uart_clk"       (clocks)         */

    const plat_prop_t *props;       /* 不可探测的 IP 参数                */
    uint32_t           prop_count;

    const char        *parent;      /* 预留:层级(AXI 互联),本期不实现   */
    uint32_t           bus;         /* PLAT_BUS_AXI / APB                */
    bool               enabled;     /* "okay" / "disabled"  (status)     */
} plat_device_t;

/* 驱动侧 —— 与 Linux 的 of_match_table 同形，只是表是编译期常量 */
typedef struct {
    const char *compatible;
    int       (*probe)(const plat_device_t *dev);   /* 0 = 认领 */
    void      (*remove)(const plat_device_t *dev);
} plat_driver_t;
```

匹配就是 `devices × drivers` 的 `strcmp(compatible)` 循环，命中调 `probe()`。

#### 必须收在描述层里的一件事：中断号解码

`xparameters.h` 里的 `_INTERRUPTS` **不是 INTID**，是编码过的
（定义见 BSP 的 `xinterrupt_wrap.h`）：

```
bits[11:0]  = 相对中断号
bits[15:12] = 触发类型
bit20       = 0 = SPI,1 = PPI
实际 GIC INTID = 相对号 + (SPI ? 32 : PPI ? 16 : 0)
```

拿本板真实值验算过：

| 外设 | `_INTERRUPTS` | 解码 | 实际 INTID |
|---|---|---|---|
| `SCUTIMER` | `0x13100d` | PPI,相对 13 → +16 | **29**（正是 Cortex-A9 私有定时器）|
| `QSPI` | `0x4013` | SPI,相对 0x13 → +32 | **51** |

**这个解码绝不能留给每个驱动**——差 32 的错会让驱动挂到一个完全无关的中断上，
而且 symptoms 取决于那个中断恰好是什么。

#### 落地形式：从 `xparameters.h` 生成

`xparameters.h` 是 Vitis 从 XSA 导出的**已经扁平化的 DTS 表示**，
包含 `_COMPATIBLE` / `_BASEADDR` / `_HIGHADDR` / `_INTERRUPTS` /
`_INTERRUPT_PARENT` 以及各 IP 的参数（如 `XPAR_AXI_GPIO_0_IS_DUAL`、
`XPAR_AXI_GPIO_0_GPIO_WIDTH`）。

新增 `tools/gen_board_desc.py`：解析它 → 生成 `plat_device_t` 表。
这相当于把 Xilinx 的 `XLookupConfig` 机制换成本项目自己的形态，
但**同样遵循"单一真值来源 = XSA"**，且内核侧零运行期依赖。

- 与仓库已有的 `tools/gen_ninja.py` codegen 风格一致；
- 生成物**进版本库**（便于 diff 与复核），CI 校验"重新生成后无差异"；
- 中断解码放在**生成器**里：生成的表里 `irq` 字段直接是干净的 INTID，
  编码细节只出现在生成器与本节。

**暂不做的两件事**（本期明确推迟）：
1. **AXI 互联层级** —— `parent` 字段先留着不实现。当前 Zynq 是"一条 APB + 一条
   AXI GP"的扁平结构，等 PL 里真挂上 AXI 互联再补。
2. **PL 动态烧录与热插拔** —— PL 现在没有启用，PL 节点一律
   `enabled = false`。

#### 验收方式

把已经跑起来的 **AXI GPIO**（`0x41200000`，双通道 8 位，无中断）从
`src/led.c` 的硬编码改成走 `plat_driver_t.probe()`。

选它的理由：它是**唯一一个不依赖内核主体**的现成驱动——
GPIO 不是设备节点，不需要 `device_t` / VFS / 堆 / 调度器
（块设备的依赖链见 §4.1）。所以整条描述层路径可以在当前的最小内核上验证完，
不必先搬 `kernel/`。

---

## 3. 风险与决策点（需先拍板）

| # | 决策 | 影响 |
|---|---|---|
| R1 | **系统调用号 ABI**：保留 Linux x86_64 号 + 号翻译层，还是 ARM 独立号表？ | 决定是否需要重建全部用户态。**修正：SXAH 的 57 位号虽然无论如何都必须重编号，但它的"代价"被原文高估了 —— 全部用户态二进制本来就是 x86-64 机器码、本来就必须全部重建（§0 事实 3），所以重编号是免费的，反而所有兼容性问题里最好解决的一个** |
| R2 | **存储介质**：SD 还是 QSPI？ | 决定块驱动与镜像流程 |
| R3 | **启动路径**：U-Boot `bootelf` 还是裸机启动头？ | U-Boot 顺带提供 DTB/网络/镜像加载；裸机更可控但工作量大。另见 §6.4：**开发期用 JTAG 直载可以两者都先跳过** |
| R4 | **浮点 ABI**：hard-float 还是 softfp？NEON 是否启用？ | 需与 bitstream 一致；影响 `liballoc`/busybox/musl 的重建目标与 VFP 上下文切换。**已可定案：AMD 官方 standalone BSP 用的是 `-mfpu=vfpv3 -mfloat-abi=hard`，跟着选 hard-float 风险最低**（见 §6.1） |
| R5 | **32 位地址空间预算**：内核堆现为"初始 256MiB、上限 2GiB"（`build_settings.h:76-81`），用户栈 16MiB，`MAX_CPU_NUM 256` | 4GiB 虚拟空间必须重新切分，现有常量直接冲突 |
| R6 | **弱内存模型**：x86 的 TSO 让很多地方可以偷懒；ARMv7 是弱序 | `include/cpu/lock.h` 已有 `spin_unlock` 里先清标志再 `sfence` 的可疑写法（`:66-83`），迁移时要把所有屏障语义重新审一遍，这是**并发 bug 高发区** |
| R7 | **用户态从哪来**：重建 busybox/musl，还是放弃 Linux 兼容层只跑 XAPI 程序？ | 前者要交叉工具链 + syscall 号翻译；后者工作量小但失去现有用户态生态 |

### 3.1 已拍板的决策

| # | 决策 | 结论 | 依据 |
|---|---|---|---|
| D1 | 工具链 | **Vitis GNU `arm-none-eabi-gcc` 13.3.0**，不用 clang | clang 集成汇编器拒绝 Xilinx BSP 汇编；且不支持 `-specs=` |
| D2 | 浮点 ABI | **hard-float**（`-mfpu=vfpv3 -mfloat-abi=hard`）| 与 AMD 官方 standalone BSP 一致（R4 已定案） |
| D3 | 页表形式 | **ARMv7 短描述符**，先只做 1MB 段 | 4KB 小页留到需要给 DMA 缓冲单独设属性时再加 |
| D4 | 设备描述层 | **描述模型照 DTS，落地形式用「从 `xparameters.h` 生成 C 表」** | 见 §2.16。模型与落地形式是两个独立的轴，先把模型定死 |
| D5 | 双核阶段 | **从 M3 拆出，后移到 AM3（After M3）** | SMP 风险随系统复杂度放大；应在调度器进来之前做 |
| D6 | PL（FPGA）范围 | **本期不启用**：PL 节点一律 `enabled = false`；AXI 互联层级与动态烧录推迟 | 当前 PL 只是占位的 AXI GPIO，没有真实 PL 设计 |

---

## 4. 建议的分阶段路径

每个阶段都以"能在 QEMU `xilinx-zynq-a9` 上看到结果"为验收标准。

| 阶段 | 目标 | 主要内容 | 状态 |
|---|---|---|---|
| **M0** | 工具链与骨架能编译 | `gen_ninja.py` 加 ARCH 维度、ARM 标志集、新 `linker.ld`；`arch/arm32/` 目录与接口定义。**注**：工具链最终选 **Vitis GNU `arm-none-eabi-gcc` 13.3.0** 而非 clang —— clang 的集成汇编器拒绝 Xilinx BSP 的 `asm_vectors.S`/`boot.S`（`ldrneh` 判为非法指令）且不支持 `-specs=` | ✅ 已完成 |
| **M1** | 串口最小可启动内核 | Zynq UARTPS 驱动 + MMIO；GIC + Cortex-A9 定时器；异常向量表；JTAG 直载运行。**注**：实际未走"改造 `main.cpp`"的路线，而是新写了 `arch/arm32/src/kmain.c`（见 §4.1 的路线说明） | ✅ 已完成 |
| **M2** | MMU + 内存管理 | ARMv7 短描述符页表；`PTE_*` 全部重做；1MB 段恒等映射 + 区域表；XN 与 DACR client；**缓存几何/维护原语 + L1+L2 使能**（DMA 前置） | ✅ 已完成 |
| **M3** | **设备描述层与驱动框架（类 DTS）** | 见 §2.16。描述模型照 DTS（节点 / `compatible` / `reg` / `interrupts` / 属性 / `status`）；**从 `xparameters.h` 生成**描述表；`INTID` 解码收在描述层；驱动 `probe()` 匹配循环；启动时打印全部节点。**验收：把 AXI GPIO 从硬编码改成走 `probe()`** | ← 当前 |
| **AM3** | **双核（SMP）** —— 原 M3 的后半，从 M3 拆出后移 | OCM 跳板 + `sev` 引导 CPU1；CPU1 自己的栈/VBAR/TTBR0/DACR/SCU/ACTLR/缓存；`TPIDRPRW` 每 CPU 数据；**per-CPU 中断表**（PPI 是每核银行化的）；spinlock + SGI 做 IPI。**验收：两核各自 1 kHz tick、核间计数器竞争结果正确** | |
| **M4** | 用户态 | SVC 入口 + 寄存器帧映射；ELF32 加载；`R_ARM_*` + `DT_REL`；ARM `crt0.S`；`TPIDRURO` TLS；信号帧；**先跑一个静态链接的 hello XAPI 程序** | |
| **M5** | 存储 + rootfs | SD/SDIO 驱动（ADMA2 + 缓存维护，注册为 `device_t`）；FATFS 打通；镜像流程出 `BOOT.BIN`；挂载 `/system` 并跑 `shell.elf` | |
| **M6** | 网络 | Cadence GEM 驱动替换 e1000（GEM 描述符环 + PHY/MDIO）；lwIP 胶水去 x86 汇编；`netserver.sys` 零改动接入 | |
| **M7** | 模块与 ABI 收敛 | 可加载 `.sys` 模块在 32 位空间工作；导出符号范围校验；重建 busybox/musl（若 R7 选前者） | |

**为什么把双核拆到 AM3 而不是留在 M3**：SMP 的风险（缓存一致性、per-CPU 数据、锁）
会随系统复杂度成倍放大。把它放在"描述层做完、调度器还没进来"的位置，
两个核面对的是同一份简单代码，调试一致性问题的代价最低；
等 M4 把调度器搬进来再上双核，一个 coherency bug 会同时牵扯调度器状态。
这与 §2.6 的判断一致。

### 4.1 已完成阶段的实际情况（与计划原文的偏差）

| 计划原文 | 实际做法 | 原因 |
|---|---|---|
| M0 用 clang | 改用 Vitis GNU `arm-none-eabi-gcc` | clang 拒绝 Xilinx BSP 的汇编 |
| M1 "改造 `main.cpp`，引入 `platform_init()`" | 新写 `arch/arm32/src/kmain.c`，未动 `kernel/` | 先把地基在最小面积上验证透，避免同时怀疑地基与上层 |
| M2 "重做 `page.cpp` 遍历" | 新写 `mmu.c`（纯逻辑）+ `mmu_hw.c`（CP15） | 同上；且这样纯逻辑能上宿主机单测 |
| M1 "静态板级描述符" | 当时的 `platform.h` 宏表 | M3 会把它升级成真正的描述层 |

**这两条路线必须在某一点汇合**，而汇合点就是驱动——因为驱动要注册
`device_t`，而 `regist_device()`（`driver/device.cpp:484 行`）依赖
`krlcb` + `id_alloc`（堆）+ `mutex`/`task/pcb.h`（调度器）+ `fs/partition.h`
（自动分区扫描）+ `mm/uaccess.h`（页表翻译 + `phys_to_virt`）；
`device_manager_init()` 一上来就创建 256 个 mutex 和一个 id_allocator。

也就是说：**块设备驱动必须坐在内核主体之上**，而内核主体（`kernel/memory` 2196 行、
`kernel/task` 3291 行、`driver/fs` 32621 行）目前一行都还没搬。
**不依赖内核主体的驱动**（GPIO、时钟、PL 上的自定义 IP）不受此限——
M3 用 AXI GPIO 验收正是利用这一点。


**建议的优先级原则**：M1 之前不要碰 `driver/` 里的 x86 驱动——先把
CPU/MMU/中断/定时器这条"地基"做完，因为上层业务逻辑（VFS、syscall handler、
调度器策略）几乎不需要改，它们的价值只在能启动之后才体现。

**驱动侧的推进顺序**（与上表对应，按风险从低到高）：
定时器 → 串口 → 中断核心 → **描述层（M3）** → SD 块驱动（M5）→ GEM 网卡（M6）。
其中**块存储的性价比最高**：块层契约干净且与设备无关，驱动只需实现回调并注册，
注册后分区扫描会自动触发；而**网络层的回报也很直接**——因为 `netdev_t` 与硬件解耦，
GEM 驱动一做出来，`netserver.cpp` 不用改一行就能跑起来。

反过来，以下驱动**应当直接删除**而不是移植：
`driver/{ahci,nvme,ide,hda,sb16,ps2,rtc,dma}`、`driver/pci`、
`kmod/{e1000,xhci}` —— 它们全部依赖 PCI/ISA/ACPI，在 Zynq-7020 上没有对应硬件。

### 4.2 进度记录

> **当前位置：M3-6 已完成，M3-7 进行中；AM3 尚未开始。**
>
> 里程碑进度：**M0 ✅ · M1 ✅ · M2 ✅ · M3 6/7 · AM3 0/5 · M4 ⬜ · M5 ⬜ · M6 ⬜ · M7 ⬜**
>
> M3 的 7 个子阶段里前 6 个已全部上板验证（含验收项 M3-4：
> AXI GPIO 从硬编码改成走 `probe()`），只剩 M3-7 串口 RX 命令通道。

里程碑内的子阶段与其状态。**每一条都必须在板上验证过才算完成**——
验证方式见 §4.3。

#### M2（MMU + 内存管理）—— 已完成

| 子阶段 | 内容 | 板上证据 | 提交 |
|---|---|---|---|
| M2-1 | ARMv7 短描述符定义 + 属性组合函数 | 宿主单测（5 个 Xilinx 常量被逐位反推）| `cca786b` |
| M2-2 | 6 段区域表 + 建表 | 宿主单测 + 两条全局不变量 | `ff7db50` |
| M2-3 | 打开 MMU（只开地址转换）| `stage=4 SCTLR=0x08C50079 TTBR0=...` | `06ad713` |
| M2-4 | 数据区加 XN；DACR 切 client | `/5` 取指 → `IFSR=0x0D`，对照 `/3` → `0x08` | `9fbc85b` |
| M2-5a | 缓存几何发现 + 维护原语 | `CLIDR=0x09200003` 几何 `MATCH` | `db0a6a4` |
| M2-5b | 使能 L1 缓存（**补上漏掉的 SCU + ACTLR**）| 加速 6~11x | `18237ce` |
| M2-5c | 按地址区间维护 + 无 DMA 自检 | `selftest=PASS` | `1997dfb` |
| M2-5d | PL310 L2 配置与使能 | `L2 ON`、128KB 工作集 1.42x | `90f579a` |
| M2-5e | **L2 有效性的受控 A/B**（补做）| `on/off/on-again = 11212/15989/11213` | `f4603d5` |

#### M3（设备描述层与驱动框架）—— 完成 7/7

| 子阶段 | 内容 | 板上证据 | 提交 |
|---|---|---|---|
| M3-1 | 描述模型 + 匹配与 probe 循环（纯逻辑）| 13 项宿主单测 + 两次证伪 | `688311d` |
| M3-2 | 从 `xparameters.h` 生成描述表 | 中断解码与手工验算逐项一致 | `8788af5` |
| M3-3 | 板级胶水层（驱动表 + PL 覆盖表 + 打印）| `Board devices: 10 nodes` | `bb508f0` |
| M3-4 | AXI GPIO 驱动走 `probe()` —— **M3 验收** | `probed=1`、基址与位宽来自描述 | `bb508f0` |
| M3-5 | 写回读验证 + **受控 A/B** | 写回读 PASS；两次破坏性实验 | `9af869e` `17e26c0` |
| M3-6 | 启动自检报告 + 自动校验脚本 | `17 passed, 0 failed` | `b627c44` |
| **M3-7** | **串口 RX 命令通道**（`help`/`ver`/`uptime`/`dump`/`probe`/`selftest`/`peek`）| 板上实测收发双向可用，见下方"根因" | `9a713f3` + 本节修订 |

**M3-7 的根因不在内核，在 XSA**：`opjtmp.xsa` 把 MIO bank 1 的电压声明成了
`LVCMOS 3.3V`，而本板该 bank 实际是 1.8V（核心板 `VCCIO_BANK1 → VCC1P8`）。
于是 `MIO_PIN_49` 的 IO 标准字段 `[11:9]` 是 3 而不是 1，接收端按 LVCMOS33 的
VIH(≈2.0V) 判定，而 CH9102F 只能驱动到 1.8V —— 引脚恒读低、没有下降沿，
`SR.RXEMPTY` 恒为 1，**与"线断了"在软件侧完全不可区分**。发送方向不受影响
（输出摆幅由物理 VCCIO 决定），所以看起来像单向故障。

修法：重新导出 XSA 时把 bank 1 电压设成 1.8V（正解）；
在此之前由 `tmp-test/zynq/ps7_mio_bank1_18v.tcl` 在 `ps7_init` 之后补偿。
完整证据链见 `arch/arm32/README.md` 第 8 节。

**这一条对后续阶段有影响**：bank 1 是 MIO16-53，以太网 / USB / SD 的输入都在里面，
在写这些驱动之前必须先确认 XSA（或补偿）已经修好，否则会重现"寄存器都对但收不到数据"
这类极难定位的现象。

#### AM3（双核）—— 未开始

| 子阶段 | 内容 | 状态 |
|---|---|---|
| AM3-0 | 前置于此的陆基已在 M1/M2-5 顺手完成 | ✅ SCU 使能、ACTLR 的 SMP 位与维护广播、L1+L2 缓存已开并验证；`spin_t`/`ldrex`/`strex` 原语已写但**零调用者** |
| AM3-1 | 每 CPU 数据基础设施（`TPIDRPRW`，单核可验）| ⬜ |
| AM3-2 | OCM 跳板 + `sev` 引导 CPU1 | ⬜ |
| AM3-3 | CPU1 自己的栈/VBAR/TTBR0/DACR/缓存 | ⬜ |
| AM3-4 | per-CPU 中断表（**PPI 每核银行化**，见 §2.6）| ⬜ |
| AM3-5 | spinlock + SGI 做 IPI；两核各自 1 kHz tick | ⬜ |

### 4.3 "上板可验证"是硬要求

从 M3 开始，**每一个子阶段都必须有板上证据**，且证据形式分三档，
可信度从低到高：

| 档 | 形式 | 弱点 |
|---|---|---|
| 1 | 软件自述日志（"probed=1"）| **是干这件事的代码自己打印的**，逻辑错了照样自洽 |
| 2 | 硬件回读（把值写进寄存器再读回）| 能证明硬件真的动了，但只能覆盖可读寄存器 |
| 3 | **破坏性 A/B**（改坏一个前提，看行为是否随之改变）| 成本最高，但唯一能证明"这条路径是承重的" |

三档都要用。M3-6 的自动校验脚本把第 1、2 档变成一条命令：

```
python tmp-test/verify_board.py --load      # 退出码即结论
```

第 3 档目前是手工做（M3-5 的两次实验），做法已记入
`arch/arm32/README.md` 的设备描述层章节。

> ⚠ 一条具体教训：M2-5d 提交时曾声称"L2 已验证"，但实际只验了
> "控制寄存器读回 1"——而当时的基准工作集是 4KB，**装得进 L1、压根碰不到 L2**。
> 是第 3 档（把 L2 关掉再测）才揭穿的。所以"寄存器说开了"永远不算验证。

---

## 5. 不需要重写、可直接复用的资产

写这部分是为了给出投入产出比的另一半：

- **系统调用处理体**：`kernel/syscall/sys.cpp`（5,313 行）、
  `kernel/syscall/xapi/*.cpp` — 纯 POSIX 语义逻辑，只依赖 `regs` 的字段名。
- **进程/线程管理**：`kernel/task/{pcb,scheduler,mutex,poll,ipc,reaper}.cpp` 的
  EEVDF 调度策略、锁协议、队列、fork/clone 记账。
  **只有每个文件里约 40 行的寄存器/内存状态管线是架构相关的。**
- **内存分配器**：`kernel/memory/{bitmap,buddy,heap,lazyalloc,vma}.cpp`（1,222 行，无汇编）。
- **用户态管理**：`kernel/user/*.cpp`（1,595 行，无汇编）。
- **VFS 与文件系统**：全部 `driver/fs/**`（VFS、FATFS、proc/dev/tmp/pipe/pty/socketfs）
  — 建立在平台中立的 `device_t` 上。
- **`include/mm/uaccess.h`** — 翻译后 `memcpy` 的设计天然可移植。
- **设备模型**：`include/device.h` + `driver/device.cpp` —— 256 项扁平数组 + 函数指针回调，
  与总线无关，新平台直接复用。
- **网卡 ABI**：`include/netdev.h` + `kernel/netdev.cpp` —— 只有 5 个字段，
  GEM 驱动可零改动接入 netserver。
- **块层**：`driver/device.cpp` 的 `rw_device`/`blk_device_read`/`blk_device_write`
  已处理非整扇区与物理连续透传，SD 驱动只需实现 4 个回调。
- **lwIP 协议栈**：`kmod/netserver/lwip/**` 本身架构中立，只需换网卡驱动与 `sys_arch` 胶水
  （后者只有 4 处 x86 汇编）。
- **`include/elf.h`**：已含 `EM_ARM`、`ELFCLASS32`、全部 `R_ARM_*`。
- **C++ 名字改编解析**：Itanium ABI，架构中立。
- **用户态启动逻辑**：`constart.cpp` 几乎不用改（只需去掉 x86 属性）。
- **FATFS 配置**：`FF_VOLUMES 10` + 512 字节扇区 + exFAT/LFN 已就绪，SDXC 可直接用。
- **第三方源码**：`third_party/{talc,libutf,libvterm,lexbor,linux-uapi,mikanos-hankaku,
  busybox-source,mbedtls-src}` 都是架构中立源码；
  `font/hankaku.bin` 是裸 4096 字节字体位图，与架构无关。

### 一个需要留意的模块 ABI 限制

`.sys` 模块只能使用约 76 个导出符号（`docs/DRIVER_GUIDE.md:137-154`），
而 `init_idt_desc`、`ioapic_add`、`send_eoi`、`pci_enable_msi`、`spin_lock`、`mutex_*`
**都没有导出**——这正是现有驱动只能轮询的原因（`docs/DRIVER_GUIDE.md:160-162,464-467`）。

移植到 ARM 时应把 `INTID → handler` 注册接口、EOI、锁原语一并导出，
否则 GEM/SD 驱动仍然只能放在内核内建 `driver/` 里，而不能做成 `.sys` 模块。
这是**架构设计上的一个顺带改进点**，建议在 M3 一并处理。

---

## 6. Zynq 侧调试与工具链（基于本机实际安装核对）

本节结论来自对 `C:\AMDDesignTools\2025.2` 的实际探查，不是通用建议。

### 6.1 本机已有的可用件（好消息）

| 组件 | 路径 | 状态 |
|---|---|---|
| ARM 交叉编译器 | `C:\AMDDesignTools\2025.2\gnu\aarch32\nt\gcc-arm-none-eabi\bin\` | **可用**。`GCC 13.3.0`，`-dumpmachine` = `arm-xilinx-eabi`（Xilinx 改名版） |
| 编译/链接验证 | — | 已实测：`-mcpu=cortex-a9 -mfpu=vfpv3 -mfloat-abi=hard` 与 `softfp` 均能编译并链接，产出 `ELF32 / ARM / EABI5` |
| `bootgen` | `Vitis\bin\bootgen.bat` | **可用且独立**。已实测生成 4.2MB `BOOT.BIN` |
| `xsdb` (JTAG 调试器) | `Vitis\bin\xsdb.bat` | **可用**。Tcl 脚本引擎实测正常；`hw_server` 能正常启动 |
| 预编译 FSBL | 见 6.2 | 已有现成 `fsbl.elf`，**不需要重建** |
| `dtc` | `Vitis\bin\dtc.exe` | 可用，移植需要 DTB |

**AMD 的 standalone BSP 用的编译选项**（来自导出的 `cortexa9_toolchain.cmake`，可直接照抄）：

```
-mcpu=cortex-a9 -mfpu=vfpv3 -mfloat-abi=hard -DSDT
-O2 -g -Wall -Wextra -fno-tree-loop-distribute-patterns
```
注意 AMD 自己选的是 **hard-float + vfpv3**（不是 neon）。
这正好可以定掉方案里的 **R4（浮点 ABI）**：跟着 AMD 官方选 hard-float 风险最低。

### 6.2 "导出不全"的真相：确实不全，但对你影响有限

你的直觉是对的，而且 AMD 自己在生成的配置里就承认了。在
`...\platform\export\platform\sw\standalone_ps7_cortexa9_0\bsp.yaml` 中：

```
standalone_enable_sw_intrusive_profiling:
  description: This option is not supported in the Unified Vitis IDE in this
    version. Change to the classic IDE if you want to use this option
```

2025.2 是 **Unified IDE** 世代，经典的 `xsct` platform/app 流程已被取代，
部分功能明确缺失且官方回复是"回到 classic IDE"。

**但关键点是：导出物本身其实是完整的。** 现成的平台目录里已经有：

```
platform\export\platform\
├── hw\sdt\
│   ├── ps7_init.c / .h / _gpl.c / .tcl     ← DDR 与时钟初始化（最重要）
│   ├── system-top.dts, zynq-7000.dtsi, pcw.dtsi, pl.dtsi   ← 设备树
│   └── System_wrapper.bit                   ← 比特流
└── sw\
    ├── boot\fsbl.elf                        ← 预编译 FSBL（319,584 字节，ELF32 ARM）
    ├── standalone_ps7_cortexa9_0\
    │   ├── Xilinx.spec                      ← 本地副本，链接必需
    │   ├── cortexa9_toolchain.cmake
    │   └── hw_artifacts\ps7_init.c, ps7_cortexa9_0_baremetal.dts, sdt.dts
    └── qemu\qemu_args.txt
```

**一个隐蔽的坑**：导出的 `cortexa9_toolchain.cmake` 引用
`$ENV{ESW_REPO}/scripts/specs/arm/Xilinx.spec`，而本机 **`ESW_REPO` 是空的**。
不过 spec 文件**已被一起导出到平台目录里**（`standalone_ps7_cortexa9_0\Xilinx.spec`），
系统里也有母本 `C:\AMDDesignTools\2025.2\data\embeddedsw\scripts\specs\arm\Xilinx.spec`。
所以自己写构建脚本时，直接指向本地这一份即可，**不要依赖 `ESW_REPO`**。
这很可能就是你觉得"命令行导出不全"的表层原因之一。

### 6.3 QEMU：本机实际上完全没有

探查结果：`Vitis\bin` 下只有 `qemu-system-aarch64.bat` 与
`qemu-system-microblazeel.bat` 两个**外壳脚本**，而且
**`C:\AMDDesignTools` 下不存在任何 QEMU 可执行文件**——QEMU 组件没有安装。

更麻烦的是，即使装上，Vitis 的 QEMU 也是给 ZynqMP/Versal（aarch64）用的；
**Zynq-7020 是 32 位 Cortex-A9，需要 `qemu-system-arm`，Vitis 并不提供。**
（导出的 `qemu_args.txt` 里写的是 `-M arm-generic-fdt-7series`，
那是 Xilinx QEMU 分支的机型名，同样不在 2025.2 的安装集里。）

**结论：QEMU 必须自己装，而且要用主线 QEMU**：
- 主线 QEMU 自带 `xilinx-zynq-a9` 机型（`hw/arm/xlnx-zynq.c`），
  支持 `-kernel`、`-serial`、`if=sd`，以及 Zynq 的 GEM/SD/UART 模型。
- 这正是 2.13 节里推荐的 `-M xilinx-zynq-a9`——它**不依赖 Vitis**，
  反而是三者中最省事的一环。
- Windows 上装 MSYS2 的 `mingw-w64-x86_64-qemu`，或官方 QEMU Windows 安装包。

### 6.4 建议的三级调试策略

按"反馈速度"排序，**前两级都不需要 SD 卡、不需要 BOOT.BIN、不需要 FSBL**。

#### 第 1 级：QEMU（开发期主力）

```bash
qemu-system-arm -M xilinx-zynq-a9 -cpu cortex-a9 -m 512M \
  -kernel out/kernel.elf \
  -serial mon:stdio \
  -S -s                     # -S 暂停等 gdb，-s 开 :1234 gdb stub
```
配合 `arm-none-eabi-gdb`：
```bash
arm-none-eabi-gdb out/kernel.elf -ex 'target remote :1234'
```
优势：可断点、可看内存/寄存器、崩溃能立刻定位、**全程可脚本化**。
移植早期（M1–M4）几乎所有问题都应该在这里解决，而不是上板。

#### 第 2 级：JTAG 直接加载（上板首选，绕过整个启动链）

这是**最被低估、但对本移植最有用**的一环。用 `xsdb` 脚本即可，
把 DDR 初始化 + 比特流 + ELF 全部推下去，**不需要 FSBL、不需要 BOOT.BIN、不需要烧 SD 卡**：

```tcl
# jtag_boot.tcl —— 用法: xsdb jtag_boot.tcl <ps7_init.tcl> <system.bit> <kernel.elf>
set ps7_init [lindex $argv 0]
set bitfile  [lindex $argv 1]
set elf      [lindex $argv 2]

connect
targets -set -filter {name =~ "APU"}

# 1) DDR 与时钟初始化（来自导出的平台）
source $ps7_init
ps7_init
ps7_post_config

# 2) 可选：加载 PL 比特流
targets -set -filter {name =~ "xc7z*"}
fpga -f $bitfile

# 3) 下载并运行内核
targets -set -filter {name =~ "ARM*#0"}
rst -processor
dow $elf
con
```
`ps7_init.tcl` 与 `System_wrapper.bit` 都已在 6.2 的导出目录里。
**这条路径把"改一行 → 上板验证"的循环从"重新打镜像 + 拔插 SD 卡"缩短到几秒**，
在 M1–M3 阶段价值极大。

> 实测状态：`xsdb` 脚本引擎与 `hw_server` 均正常；
> 当前未接 JTAG 线，所以 `connect` 会停在等待目标。
> 接上 Digilent/Xilinx 下载器后 `targets` 即可枚举出两个 Cortex-A9 核。

#### 第 3 级：SD / QSPI 启动（阶段性验收）

只有在验证"真实启动链"时才需要，用 6.5 的 `bootgen` 流程。

### 6.5 完全绕开 Vitis IDE 的构建与打包流程（已实测通过）

```
[arm-none-eabi-gcc]  →  out/kernel.elf
                              │
[预编译 fsbl.elf] ────────────┼──→ bootgen → BOOT.BIN → SD 卡 / QSPI
[System_wrapper.bit] ─────────┘
```

`boot.bif`（Zynq 格式）：
```
the_ROM_image:
{
    [bootloader]<platform>\sw\boot\fsbl.elf
    <platform>\hw\sdt\System_wrapper.bit
    <你的>\out\kernel.elf
}
```

打包命令：
```
bootgen -image boot.bif -arch zynq -o BOOT.BIN
```

**实测结果**：用 6.1 的独立 `arm-none-eabi-gcc` 编出一个最小 ARM 内核 ELF，
连同导出的 `fsbl.elf` + `System_wrapper.bit` 一起，`bootgen` 成功产出
**4,213,104 字节**的 `BOOT.BIN`（`bootgen v2025.2`，退出码 0）。

也就是说：**从编译到可烧写镜像，整条链路都不需要打开 Vitis IDE，
也不需要它的 platform/app 流程。** Vitis 在这次移植里只需要提供三样东西：
跨平台 GCC、`bootgen`、`xsdb`——而它们都是独立的命令行工具。

### 6.6 落地建议

1. **构建系统**：用 6.1 的选项集接到 `tools/gen_ninja.py`（对应方案 M0），
   不引入 Vitis 的 CMake/platform 流程，也不依赖 `ESW_REPO`。
2. **移植早期（M1–M4）只用 QEMU**，把架构层的 bug 在这一层清干净。
3. **需要上板时用 JTAG 直载**（6.4 第 2 级），不要一开始就折腾 SD 启动。
4. **只在阶段性验收时**用 `bootgen` 出 `BOOT.BIN`（6.5）。
5. 需要的话，可以把 6.4/6.5 的脚本固化成 `tools/zynq/` 下的
   `jtag_boot.tcl` + `boot.bif` + 一个 `ninja` 目标，
   让"打包"和"直载"都是一条命令。

### 6.7 工具链建议（实测决策清单）

汇总 §2.13（构建标志）、§2.15（BSP 复用与链接）与本节的实测结果，
给出可直接落地的工具链选择。

#### 决策：ARM 侧主工具链用 Vitis 的 GNU 工具链（GCC 13.3.0）

不是"因为已经装了"，而是有三条实测理由：

**理由 1：clang 的集成汇编器编不了 Xilinx 的 BSP 汇编。**

对 `standalone_v9_4` 的 Cortex-A9 启动汇编逐个实测（`-mcpu=cortex-a9 -mfpu=vfpv3 -mfloat-abi=hard`）：

| BSP 汇编文件 | 作用 | clang 集成汇编器 | GNU `as` |
|---|---|---|---|
| `asm_vectors.S` | 异常向量表 + **勘误 775420 处理** | ❌ **FAIL** | ✅ OK |
| `boot.S` | MMU/启动初始化 | ❌ **FAIL** | ✅ OK |
| `translation_table.S` | 初始页表 | ✅ OK | ✅ OK |
| `cpu_init.S` | CPU 初始化 | ✅ OK | ✅ OK |
| `xil-crt0.S` | C 运行时启动 | ✅ OK | ✅ OK |

clang 的具体报错是拒绝条件执行指令：
```
asm_vectors.S:134:2: error: invalid instruction, did you mean: ldrexh, ldrh?
 ldrneh r0, [lr,#-2]
```
失败的恰好是**最要紧的两个文件**（向量表与 MMU 启动）。也就是说，只要想复用
§2.15 里推荐的 BSP 代码（尤其是带勘误处理的向量表），就绕不开 GNU `as`。

**理由 2：`Xilinx.spec` 是 GCC 独有机制。** BSP 的构建流程依赖
`-specs=Xilinx.spec`（见 §6.2 的 `cortexa9_toolchain.cmake`），clang 不支持 `-specs=`。

**理由 3：libgcc 的取用最顺。** GCC 下直接 `-lgcc` 即可；clang 下必须显式给
`.a` 全路径（见下）。

> 补充：项目现在的 `tools/gen_ninja.py` 已经把 `cc`/`cxx`/`ld` 做成可由
> `CC`/`CPP`/`LD` 环境变量覆盖（`:528-532`），所以**按架构切编译器不需要改架构**，
> x86_64 继续用 clang，ARM 用 GCC，两者并存。

#### 完整标志集（实测可用，可直接写入 `gen_ninja.py`）

```
# 公共
--target 由工具链决定；本机即 arm-xilinx-eabi
-mcpu=cortex-a9 -marm -mfpu=vfpv3 -mfloat-abi=hard
-fno-tree-loop-distribute-patterns          # AMD BSP 同款，避免 memset/memcpy 被改写成语义更强的调用

# 内核/驱动/kmod（对齐项目现有 x86 风格）
-ffreestanding -nostdlib -nostdinc -fno-builtin -fno-stack-protector
-fno-exceptions -fno-rtti -fno-use-cxa-atexit -fno-threadsafe-statics
-fshort-wchar -std=gnu++17 -MMD -MP
-fno-pic -fno-pie                            # 对应 x86 侧的 -mcmodel=large 位置

# 用户态
-ffreestanding -nostdinc -fno-builtin -fno-stack-protector -std=c++11

# 需要删除的 x86 专有项
-m64 -mno-red-zone -mstackrealign -mno-80387 -mcmodel=large
NASM -f elf64 / objcopy -O elf64-x86-64
```

**链接（关键，见 §2.15）：**

```
# 推荐：GCC 驱动
arm-none-eabi-g++ -mcpu=cortex-a9 -marm -mfpu=vfpv3 -mfloat-abi=hard \
  -nostdlib -T arch/arm32/linker.ld <objs> -lgcc -o out/kernel.elf
```

⚠️ `-lgcc` **不能省**。ARM 无硬件整数除法，32/64 位除法会产生
`__aeabi_uidiv` / `__aeabi_uldivmod`；项目现有链接行
`ld -T linker.ld --static`（`tools/gen_ninja.py:633-638`）不含 libgcc。

#### 备选：保留 clang（可行，但要多配一个汇编器）

若希望 ARM 侧也保持 clang 以统一风格，实测可行，组合如下：

| 环节 | 工具 | 实测 |
|---|---|---|
| C/C++ 编译 | `clang++ --target=armv7a-none-eabi` | ✅ 可用（clang 22.1.8，`D:\LLVM\bin`），产物 `ELF32/ARM` |
| BSP `.S` 汇编 | **必须换 GNU `as`** | clang ❌ / GNU ✅（见上表） |
| 链接 | `ld.lld` + 显式 libgcc 路径 | ✅ 成功；产物 `hard-float ABI` 标志正确 |
| 链接 | `clang++ --gcc-toolchain=... -lgcc` | ❌ **失败**：`ld.lld: error: unable to find library -lgcc` |

可用命令（已实测通过）：
```bash
# 编译
clang++ --target=armv7a-none-eabi -mcpu=cortex-a9 -mfpu=vfpv3 -mfloat-abi=hard \
  -ffreestanding -nostdlibinc -fno-builtin -fno-exceptions -fno-rtti \
  -fno-stack-protector -std=gnu++17 -O2 -c foo.cpp -o foo.o

# 链接（libgcc 必须给全路径）
clang++ --target=armv7a-none-eabi -fuse-ld=lld -nostdlib -T arch/arm32/linker.ld \
  <objs> "C:/AMDDesignTools/2025.2/gnu/aarch32/nt/gcc-arm-none-eabi/aarch32-xilinx-eabi/usr/lib/arm-xilinx-eabi/13.3.0/libgcc.a" \
  -o out/kernel.elf
```

**代价**：规则表里要同时存在 clang 与 GNU as 两套编译规则，且 `-specs=`、BSP 的
CMake 流程都用不上。**除非有明确理由坚持单一编译器，否则不推荐。**

#### 本机路径速查

| 用途 | 路径 |
|---|---|
| GCC / G++ / as / ld | `C:\AMDDesignTools\2025.2\gnu\aarch32\nt\gcc-arm-none-eabi\bin\` |
| libgcc（ARM state，EABI） | `...\aarch32-xilinx-eabi\usr\lib\arm-xilinx-eabi\13.3.0\libgcc.a` |
| 其余 multilib 变体 | `...\usr\lib\{arm-xilinx-eabi, arm-xilinx--gnueabi, arm-xilinx-linux, thumb\v7+fp\hard, thumb\v7-a+fp\hard}` |
| clang / LLD（备选） | `D:\LLVM\bin\`（clang 22.1.8） |
| BSP 头文件（汇编要用） | `...\platform\ps7_cortexa9_0\standalone_ps7_cortexa9_0\bsp\include\`（含生成好的 `bspconfig.h`） |
| BSP 汇编源码 | `C:\AMDDesignTools\2025.2\data\embeddedsw\lib\bsp\standalone_v9_4\src\arm\cortexa9\gcc\` |

> 注意：汇编 BSP 文件时需要**同时**加 `-I ...\arm\cortexa9`（取 `xil_errata.h`）
> 和 `-I ...\bsp\include`（取 `bspconfig.h`）。`bspconfig.h` 在 embeddedsw 里
> 只有 `.in` 模板，**必须用导出平台里生成好的那一份**——
> 这也是"直接用 embeddedsw 源码"时容易踩的一个坑。

---

## 7. 一句话总结

**地基（CPU/MMU/中断/定时器/SMP/启动/构建）几乎要全部重写（约 3,500 行新代码），
上层（VFS/syscall 处理/调度策略/设备模型）基本可以原样保留；
真正的成本不在"写 ARM 代码"，而在"把散落在 14+ 个文件里的 x86 常量、
95 处内联汇编、以及 x86 二进制生态一次性收敛掉"。**

建议按 M0→M3→AM3→M4→M7 推进（AM3 = 双核，从原 M3 拆出）。
**M0–M2 已完成并上板验证**，实际代码量与本文估计同量级（`arch/arm32` 约 5,400 行，
含验证基础设施；本文的 3,500 行只算了地基本身）。

**计划本身需要一条补充**：本文假设的是"移植现有内核"（改造 `main.cpp`/`page.cpp`/
`sys.cpp`），而实际执行的是"先在最小面积上把地基写对"（新写 `kmain.c`/`mmu.c`/
`cache_hw.c`）。**这两条路线必须在某一点汇合，而汇合点就是驱动**——
块设备驱动要注册 `device_t`，而 `regist_device()` 依赖堆、调度器、VFS、
`phys_to_virt`（依赖链见 §4.1），这些目前一行都还没搬。
不依赖内核主体的驱动（GPIO、时钟、PL 自定义 IP）不受此限，
所以 **M3 用 AXI GPIO 验收描述层**是最省的一次路径验证。

调试方面：**工具链其实够用，缺的是 QEMU（自己装主线版即可）和一个正确的调试顺序。**
把 QEMU 当主力、JTAG 直载当上板手段、`bootgen` 只在验收时用，
就能完全绕开 Vitis Unified IDE 那些"明确不支持"的功能。

**最后一条经验，写在这里因为它比上面任何一条都更影响后续成本**：
M0–M2 阶段修掉的 bug 里，**几乎没有一个是"ARM 与 x86 不同"造成的**，
全部是"这个假设从来没被验证过"——空指针解引用、
缓存使能了却完全无加速（漏 SCU/ACTLR）、
耗时基准被编译器整体优化掉、首次测量取到未初始化值。
所以 §0 那句"上层基本可以原样保留"应当读作
**"上层代码可复用，但需要同等密度的重新验证"**——
它们是纸面上可移植，搬到 32 位 + SVC 入口 + 弱内存序下不会在编译期报错，
只会在运行期偶发。

