# arch/arm32 —— Zynq-7020 (Cortex-A9) 架构层

OpenXJ380 移植到 ARMv7-A 的地基。对应 `docs/ZYNQ7020_PORT_PLAN.md`：
从 M0（**工具链 → 构建系统 → 链接 → JTAG 直载 → 板级可见输出**）一路做到 M4-11。

现状：**M0–M4-11 已完成并板上验证**（板级自检 **97 项全绿**、**8 组破坏性 A/B 全部检出**、
串口命令通道 10/10）。内核能启动、点灯、双核调度、抢占、抢占式让出、
互斥锁、线程退出路径，并通过 OCM 心跳 + 串口自检报告向 JTAG/主机汇报状态。

⚠ **它仍然不是可用的系统**：没有用户态、没有系统调用层、没有文件系统、
没有块设备/网卡驱动、没有可加载模块；只在两块 Zynq-7020 开发板上验证过。
对外说明见 `docs/ZYNQ7020_PORT_STATUS.md`，与上游合流的路线见
`docs/ZYNQ7020_INTEGRATION_PLAN.md`。

---

## 目录结构

```
arch/arm32/
├── boot/
│   ├── start.S          启动汇编:屏蔽中断、使能 FPU、建各模式栈、清 BSS、跳 kmain
│   │                    （另含 cpu1_entry:第二个核的入口）
│   ├── vectors.S        异常向量表 + 各异常的处理外壳（致命异常停在 wfe 自旋）
│   └── kernel.ld        链接脚本:加载到 DDR 0x00100000,两个核各自的栈区,
│                        L1 页表 16KB 对齐的 .mmu_tbl 段
├── include/arch/
│   ├── types.h          定宽类型（freestanding 不依赖 stdint.h,hosted 借用 libc）
│   ├── io.h             MMIO 读写 + 屏障语义
│   ├── cpu.h            架构接缝:中断/CP15/缓存/TLB/自旋锁
│   ├── platform.h       板级常量 + 心跳区/故障选择器的物理地址
│   ├── mmu.h            ARMv7 短描述符定义、属性构造函数、区域表
│   ├── percpu.h         每核结构（cpu_id/online/ticks/ipi_count/栈顶）
│   ├── smp.h            第二个核的释放与在线等待、IPI、压力测试
│   ├── irq.h            GIC 抽象 + 中断注册/统计
│   ├── timer.h          全局定时器（nanoTime 的替代）
│   ├── uart_ps.h        Cadence UARTPS 驱动
│   ├── uart_baud.h      波特率闭环标定（纯逻辑,宿主可测）
│   ├── console.h        最小格式化输出
│   ├── led.h            LED / 拨码开关
│   ├── heartbeat.h      ★ 跨模块契约:OCM 心跳槽位定义（只能有一处）
│   ├── selftest.h       启动自检报告的判定逻辑（纯函数）+ 输出接口
│   ├── shell.h          串口命令通道
│   ├── fault_test.h     故障注入选择器（含 guard page 的三个）
│   ├── board.h /
│   │   board_devices.h  M3 设备描述层:从 xparameters.h 生成的节点表
│   ├── plat_device.h    device_t / plat_driver_t / probe 匹配（纯逻辑）
│   ├── axi_gpio.h       PL AXI GPIO 驱动（描述层上的第一个真驱动）
│   ├── palloc.h         M4-2 物理页分配器（2 bit/页）
│   ├── heap.h           M4-3 内核堆（空闲链表,自检会核对统计一致性）
│   ├── vmap.h           M4-4 细粒度映射（段 -> 小页的属性**翻译**）
│   ├── kstack.h         M4-5 内核栈池 + guard page
│   ├── krlibc.h         M4-1 的 15 个纯函数 + errno
│   └── errno.h          与 x86 侧逐值一致的 errno 定义
└── src/
    ├── kmain.c          内核入口(启动顺序见下)
    ├── board.c          板级胶水:心跳、故障选择器、时钟
    ├── board_devices.c  设备描述表(生成物)
    ├── plat_device.c    描述层匹配逻辑（不含 MMIO,宿主可测）
    ├── axi_gpio.c       AXI GPIO 驱动
    ├── led.c             PL LED（走描述层）与 PS LED
    ├── timer.c           全局定时器 + 周期中断
    ├── uart_ps.c         UART 驱动
    ├── uart_baud.c       波特率标定（纯）
    ├── console.c         最小 printf 风格的输出
    ├── gic.c             GIC + 异常现场打印 + FSR 译码
    ├── fault_test.c      故障注入
    ├── cache.c/.h        缓存几何与维护原语（纯）
    ├── cache_hw.c        CP15 缓存维护
    ├── mmu.c             描述符编解码与区域表（纯）
    ├── mmu_hw.c          MMU 使能、TTBR0/DACR
    ├── percpu.c/.h       每核表（纯）
    ├── percpu_hw.c       TPIDRPRW
    ├── smp.c             第二个核
    ├── selftest.c        自检报告输出
    ├── shell.c           命令通道
    ├── palloc.c          物理页分配器（纯）
    ├── heap.c            内核堆（纯）
    ├── vmap.c            细粒度映射（纯）
    ├── kstack.c          栈池（纯,TLB 通过钩子外置）
    ├── kstack_hw.c       TLB 维护钩子（CP15）
    └── krlibc.c          krlibc 子集 + errno
```

**分层约定（不是洁癖,是"能不能在宿主机上把最容易错的地方钉死"）**:
标了「纯」的文件**不含任何 MMIO / CP15 / 内联汇编**,由宿主编译器直接编译
并跑自检 —— 见 `tests/test_arm32_*.py`。需要 CP15 的那一半单独一个 `*_hw.c`。
唯一一处**刻意**的偏离是 `kstack.c`:它自己拥有 TLB 维护这件事,
但做成函数指针(理由见「M4-5」一节)。
## 构建

```bash
# 生成 ARM 构建图（与 x86 图完全独立，互不影响）
python tools/gen_ninja.py --out build-arm.ninja --arch arm32

# 构建
ninja -f build-arm.ninja arm32        # 产物 out/kernel-arm.elf
```

> ⚠ `ninja format`（`tools/ninja_build.py:400`）的根目录列表里**没有 `arch/arm32`** ——
> 它只格式化 `kernel` / `driver` / `graphics` / `font` / `lib` / `include`。
> 所以本目录的代码**不归 clang-format 管**，风格靠 AGENTS.md 的约定与一致性维持：
> 4 空格缩进、120 列、函数体大括号独占一行、指针的 `*` 跟变量名。
> 别拿 `clang-format` 的输出当成"这里没格式化好" —— 用它去改会把注释对齐打散。

工具链自动探测，可用环境变量覆盖：

| 变量 | 默认 | 说明 |
|---|---|---|
| `ARM_CC` | PATH 或已知 Vitis 路径 | `arm-none-eabi-gcc` |
| `ARM_OBJCOPY` | 由 `ARM_CC` 推导 | |
| `ARM_LIBGCC` | `-lgcc` | 仅在必须指定归档路径时才设置 |
| `ARCH` | `x86_64` | 等价于 `--arch` |

### 为什么用 Vitis 的 GNU 工具链而不是 clang

三条实测理由（详见 `docs/ZYNQ7020_PORT_PLAN.md` §6.7）：

1. **clang 的集成汇编器编不了 Xilinx BSP 的汇编**。`asm_vectors.S`（异常向量表，
   含勘误 775420 处理）与 `boot.S`（MMU 启动）都会被拒绝，报
   `invalid instruction ... ldrneh`。GNU `as` 两个都能过。将来 vendor BSP 代码时绕不开。
2. `Xilinx.spec`（`-specs=`）是 GCC 独有机制，BSP 构建流程依赖它。
3. libgcc 取用最顺。

`gen_ninja.py` 已把 `cc`/`cxx`/`ld` 参数化，所以**两个架构可以并存**：
x86_64 继续用 clang，arm32 用 GCC。

### 关于 `-lgcc`（重要，且与最初设想不同）

ARM 没有硬件整数除法指令，32/64 位除法会产生 `__aeabi_uidiv` / `__aeabi_uldivmod`
调用。`-nostdlib` 不会自动带入 libgcc，**必须显式加 `-lgcc`**。

关键在于**不要硬编码 libgcc 的路径**。Xilinx 的 aarch32 工具链**没有 ARM 状态的
hard-float multilib**——非 Thumb 的变体只有 armv5te。对于
`-mcpu=cortex-a9 -marm -mfpu=vfpv3 -mfloat-abi=hard`，GCC 驱动会选
`thumb/v7-a+fp/hard`。如果按直觉去取
`.../usr/lib/arm-xilinx-eabi/*/libgcc.a`，拿到的是 **soft-float** 归档，链接会失败：

```
error: kernel-arm.elf uses VFP register arguments,
       libgcc.a(_udivmoddi4.o) does not
```

用 `-lgcc` 让驱动自己解析 multilib 即可。

> ⚠ 这条与 `docs/ZYNQ7020_PORT_PLAN.md` §2.15 早期的说法**相反**。那里的
> "`--gcc-toolchain -lgcc` 失败" 是 **LLD** 的行为；换成 GNU ld 后 `-lgcc` 正常。

### 关于 Windows 上的规则写法

本架构层的 ninja 规则**不使用 `mkdir -p $$(dirname $out)`**，而是由
`gen_ninja.py` 在生成时预先创建对象目录。

原因：原生 Windows 的 ninja 直接调 `CreateProcess`，不会为 `mkdir` 这类
shell 内建命令启动 `cmd.exe`，所有边都会报 `CreateProcess failed`。
去掉 shell 依赖后规则在两个平台上都能跑。

`ninja clean` 只删声明过的输出文件、不删目录，所以目录能存活；
手动 `rm -rf out` 后重新生成一次即可。

---

## 单元测试

单测跑在**宿主机**上，不碰板子 —— 所以只覆盖能脱离硬件验证的纯逻辑。

```
python -m unittest discover --start-directory tests --pattern 'test_*.py'
```

| 测试 | 覆盖内容 | 为什么值得测 |
|---|---|---|
| `tests/test_arm32_uart_baud.py` | `uart_baud_search()` 的分频搜索 | 这段数学出过 **7.033 倍**的波特率偏差：标定点用了 `BAUDDIV=0`，而合法区间是 **4..254**。回归保护的就是这个区间边界。 |
| `tests/test_arm32_console.py` | `console_printf` 的格式化 | panic 路径的输出格式；`%p` 在 32 位上必须走 `uintptr_t` 中间转换 |

为了让同一份源码既能被 ARM 交叉编译、又能被宿主 gcc 编译，
`include/arch/types.h` 按 `__STDC_HOSTED__` 分了两个分支：
宿主构建走标准头（`stdbool.h`/`stddef.h`/`stdint.h`），
freestanding 构建自带 `size_t`/`uintptr_t`/`bool` 等定义。
少了这个分支，宿主上会报 `typedef redefinition`。

**当前整体结果**：45 tests / 1 FAIL / 8 ERROR。
这些失败**全部是预先存在的**，与 ARM 侧代码无关，基线 commit `08e5c9c`
为 42 tests / 1 FAIL / 10 ERROR —— 即新增测试把 2 个 error 转成了通过，
**未引入任何回归**。

> ⚠ **本机宿主工具链是坏的**（预先存在，与本移植无关）：
> MinGW 的 `gcc`/`g++` 连 `int main(){return 0;}` 都编译不了
> （`cc1.exe` 静默退出 1），且没有 `sha256sum`。
> 受影响的测试会报 ERROR 而不是 FAIL。排查 ARM 侧问题时不要被这些噪声带偏。
> 上表的两个测试不依赖 MinGW，用的是 `("gcc","cc","clang")` 里第一个可用的编译器。

---

## 启动顺序（`kmain`）

```
 1. led_init()                  LED 先起来 —— 它不依赖未知时钟,是最可靠的反馈手段
 2. 写 OCM 心跳 magic           JTAG 立刻可确认"内核活了"
 3. timer_init()                ⚠ 必须早于任何 timer_delay_*
 4. LED 上电自检                全亮 -> 全灭
 5. uart_probe()                探测串口是否存在
 6. 闭环收敛 UART 参考时钟 -> 初始化 -> 环回自检 -> 打印横幅
    ⚠ 环回自检必须**在横幅之前**:它会在本机 TX 上留下探针字节,
      把行首的 CHECK 污染成 UCHECK,而报告解析是按行首 ^CHECK 匹配的
 7. gic_init()                  关中断下配置 Distributor / CPU Interface
 8. irq_register() + 启动定时器 + gic_enable_irq()
 9. irq_global_enable()         最后一步才打开 CPU 中断响应
10. mmu_enable()                段映射恒等映射,只开地址转换
11. 缓存几何发现 -> L1 -> SCU/ACTLR -> PL310 L2
12. 设备描述层                  从 xparameters.h 生成的节点表 -> probe 匹配
13. 9.4  物理页分配器 palloc   池 = [_kernel_end 向上对齐, DDR 末尾)
14. 9.44 细粒度映射 vmap        真实页表上拆段 + 改映射 + TLB 失效后读回
15. 9.45 内核堆                32MB 一次性要足（增长区必须紧邻,而 palloc 不保证）
16. 9.46 内核栈池 + guard page 32 槽 x 1MB,每槽一页 guard
17. 9.5  放出 CPU1              per-CPU 表就绪 -> 释放 -> 等在线 -> IPI 压力
18. 自检报告                  固定格式回传,由 verify_board.py 判退出码
19. shell_init() / banner      命令通道（在报告之后:此前串口还在标定,回显会乱）
20. 主循环                     跑马灯 + PS LED 慢闪 + 心跳 + 周期输出
                               + fault_test_poll() + shell_poll()
```

另有 `start.S` 在清 BSS 之前调用 `vectors_install()` 写 VBAR ——
必须早于任何可能出错的 C 代码，否则 VBAR 还指向 0（OCM），
一出错就跳到无意义的位置，连现场都抓不到。

顺序上有几处是踩过坑才定下来的，注释里都标了原因。

## 调试通道：OCM 心跳

`0x00020000` 起 16 个 32 位槽，JTAG 用 `mrd` 直接读，**不需要串口**：

| 槽 | 偏移 | 含义 |
|---|---|---|
| 0 | `0x00` | magic `0x4F583338`（"OXJ8"）|
| 1 | `0x04` | 主循环计数 |
| 2 | `0x08` | PL LED 图案 |
| 3 | `0x0C` | 拨码开关值 |
| 4 | `0x10` | 全局定时器低 32 位 |
| 5 | `0x14` | PS LED 状态 |
| 6 | `0x18` | UART 参考时钟（0 = 未标定）|
| 7 | `0x1C` | PS GPIO `DIRM_0` |
| 8 | `0x20` | UART 探测结果（1 = 存在）|
| 9 | `0x24` | 当前设定下实测的真实波特率 |
| 10 | `0x28` | 周期 tick 计数 |
| 11 | `0x2C` | GIC 收到的中断总数 |
| 12 | `0x30` | 参考时钟来源（1 = 闭环收敛，2 = 兜底常量）|
| 13 | `0x34` | 闭环实际迭代次数（0 = 没进迭代）|
| 14 | `0x38` | 最终生效的 `BAUDGEN` |
| 15 | `0x3C` | 最终生效的 `BAUDDIV` |
| **16** | `0x40` | **MMU 阶段标记**（见下）|
| 17 | `0x44` | 最大 tick 间隔（µs，正常应紧贴 1000）|
| 18 | `0x48` | 缓存加速比（正常在数倍以上）|
| 19 | `0x4C` | 缓存维护自检（0 = 通过）|
| 20 | `0x50` | L2 有效性：128KB 工作集关/开 L2 的耗时比（实测 1.42）|

心跳区预留 **64 槽**（`0x00020000`–`0x000200FF`），故障注入选择器从
`0x00020100` 开始。这条边界由 `heartbeat.h` 里的静态断言双向锁住 ——
它曾经被踩过：选择器原先在第 16 槽，而心跳后来也扩到了第 16 槽，
一旦撞上，`fault_test_poll()` 会把心跳进度号当成注入码，
症状是"内核莫名进了 Data Abort"，而心跳本身看上去完全正常。

★ 预留**容量**是 64，但**已分配**的槽是 0–31（`HB_SLOT_COUNT = 32`）：
上表列到槽 20，21–31（`LEDCHECK` / `PROBED` / `SELFTEST_FAILED` /
`CPU1_*` / `IPI_COUNT` / `SMP_VIOLATION`）见 `heartbeat.h` —— 槽号的
**唯一定义处**在那里，本表只在讲某个机制时顺带引用。
槽 32–63 是**保留区**，由 `kmain` 在启动时显式清零：OCM 上电内容未定义，
不清的话"还没人写过"与"写了 0"分不开，JTAG 会把残值当成真实读数。

（2026-09-18，合流决策 D5：容量 32 → 64，选择器 `0x00020080` → `0x00020100`。
扩容而不是删旧槽的理由：34 个槽名**每个都有写者**，而 JTAG 可见的诊断量
只能放在**不可缓存**的低 1MB —— DDR 是写回可缓存，JTAG 直读物理内存会读到陈旧值。）

**槽 16 与其它槽的写法不同**：它由「中途可能再也回不来」的模块自己写。
MMU 打开过程一旦出错，`kmain` 就再也执行不到了，
所以进度必须由开 MMU 的代码在每一步之前写入：

| 值 | 含义 |
|---|---|
| 0 | 尚未开始 |
| 1 | 页表已填好 |
| 2 | `TTBR0`/`DACR`/`ACTLR` 已配置，MMU 未开 |
| 3 | 正要写 `SCTLR` 打开 MMU |
| 4 | MMU 已开，且已回到 C 代码继续跑 |
| `0xEE` | 自检未通过，主动放弃开 MMU |

挂死时读回的值就是"最后成功进入的阶段"，能把范围缩到一条指令附近。

槽 12–15 是为另一个陷阱加的：**闭环收敛成功返回的值，和收敛失败后由调用方
填的兜底常量，可能完全是同一个数字，含义却相反** —— 前者经过实测验证，
后者只是猜的。只记 `uart_clk` 无法区分这两种情况，于是踩过一次：
心跳显示 `uartclk=100500000` 看起来正常，实际是兜底值，收敛早就失败了。

这条通道比串口更好用：能读任意内存、能看 PC/寄存器、崩溃后仍可读。
移植早期应当优先依赖它。

⚠ **读取时机**：`magic`（槽 0）在 `kmain` 一开头就写了，而 `DIRM0`/`TICKS`/
`IRQCOUNT` 分别在启动横幅之后、中断子系统起来之后才写。9600 波特下横幅本身
要 ~870 ms，所以"看到 magic 就立刻读整片心跳区"会稳定地读到一片 0。
务必轮询槽 10（`ticks != 0`）作为门控 —— 曾经据此误判成"内核卡死在 `uart_puts`"。

---

## 已知问题与待办

### 1. ~~本板串口不可用~~ —— 已解决（用重新导出的 XSA）

**现状：串口已完全可用。** 用户重新导出了含 UART1 的 `opjtmp.xsa`，
从中生成的 `ps7_init` 打开了 UART 外设，问题消失。
下面保留原始排查记录，因为它说明了"外设寄存器读回 0"这类现象该怎么归因。

**当时的现象**：UART0/UART1 寄存器读回恒为 0。

**当时的根因**：原有 `AXI_GPIO_1` 比特流对应的 **PS7 配置没有启用 UART**。

证据链（逐条实测）：

| 证据 | 结果 |
|---|---|
| 导出的 `xparameters.h` | 没有任何 UART 定义 |
| `ps7_init.c` | `UART0_CPU_1XCLKACT = 0x0`、`UART1_CPU_1XCLKACT = 0x0`，"UART REGISTERS" 段为空 |
| PS 外设空间扫描 | **只有 GPIO 与 QSPI 响应**；UART/SPI/SDIO/GEM/I2C 全部读回 0 |
| JTAG 写入 MIO_PIN_48/49（`L3_SEL=7` → UART1） | 写入成功（`0x12E0`/`0x12E1`），**UART 仍读回 0** |
| JTAG 把 APER_CLK_CTRL 全部置 1 | **UART 仍读回 0** |
| `UART_CLK_CTRL` | bit1（CLKACT1）本来就是 1 |

**结论：软件层面打不开它。** MIO 路由、APER 门控、CLKACT 三条路都试过且都生效了，
但外设寄存器始终读回 0 —— 说明该外设在当前 PS7 配置下根本不响应。

**最终解法**：用带 UART1 的 XSA 重新生成 `ps7_init`，
即 `tmp-test/zynq/ps7_init_uart1.tcl`（已提交）。JTAG 加载时 `source` 它即可，
不需要重新烧写 Flash、也不需要 FSBL/BOOT.BIN。
注意 `ps7_init` 只管 **PS**（时钟/MIO/DDR），比特流管 **PL**（AXI GPIO → LED），
两者独立，可以交叉组合：用新 `ps7_init` 换到串口，同时保留旧比特流不丢 LED。

**板子的 PS 调试口已逐脚确认是 UART1**（原理图追踪）：

| AC850 核心板 | 网络 | SoC 球号 | AC880 底板 |
|---|---|---|---|
| 27 脚 | `PS_MIO48` | D11 | P2/27 = `PS_UART_TXD` |
| 45 脚 | `PS_MIO49` | C14 | P2/45 = `PS_UART_RXD` |

与 Zynq 标准的 UART1 = MIO48(TX)/MIO49(RX) 一致。

在此之前，内核用 OCM 心跳工作完全正常。

### 2. UART 参考时钟：已用闭环收敛解决（保留踩坑记录）

**结论：`UART_REF_CLK = 100 MHz`（Xilinx 标准值），串口现已可用。**

最初用"单点测量 + 外推"的办法求参考时钟：在 `BAUDGEN=256, BAUDDIV=0` 处
用全局定时器测发送耗时，再按 `baud ∝ 1/(BAUDGEN×(BAUDDIV+1))` 外推到工作点。
**这个办法给出 14.3 MHz，而真值是 100 MHz —— 偏低 7.033 倍。**

定位过程（记录以备后人）：

1. 先独立验证时间基准：用墙钟测全局定时器，实测 332.5 MHz，
   与假定的 333.333 MHz 只差 0.24% → **时间基准没问题**，排除计时误差。
2. 核对帧格式：`MR=0x20` 确为 8 数据位+无校验+1 停止位 → **10 位/字节的假设成立**。
3. 让内核在**工作点上**直接实测波特率（不外推），连读心跳得到对照表：

   | 标称 | 实测 | 比值 |
   |---|---|---|
   | 9600 | 67536 | 7.035 |
   | 19200 | 135072 | 7.035 |
   | 38400 | 270012 | 7.032 |
   | 57600 | 405021 | 7.032 |
   | 115200 | 810062 | 7.032 |

   **稳定 7.03 倍** —— 这是系统性比例错误，不是随机误差。

**根因**：标定点用了 `BAUDDIV=0`，而 AMD 官方驱动（`XUartPs_SetBaudRate`）
遍历的 BAUDDIV 范围是 **4..254**。`0` 不在有效区间，该点的实际分频不等于
`(BAUDDIV+1)=1`，于是整个外推链按固定倍数偏掉。

**修法**：改为**闭环收敛**（`uart_converge_ref_clk`）——
设定 → 在工作点实测 → 按"参考时钟与实际波特率成正比"修正 → 迭代至相对误差 ≤0.5%。
不依赖任何模型，12 次迭代内收敛。首轮即得到 `100,000,000 Hz`。

启动横幅实测输出：

```
 CPU          : 666666687 Hz
 Global timer : 333333343 Hz
 UART ref clk : 100000000 Hz (source=1 iters=1, self-calibrated)
 Baud         : requested=9600 actual=9600 (BAUDGEN=1736 BAUDDIV=5 err=0 ppm)
 PS GPIO DIRM0: 0x00000180 OEN0: 0x00000180
 IRQ status   : ticks=20 irq_count=20 last_intid=29 spurious=0
 Periodic tick is RUNNING (1 kHz, Cortex-A9 private timer).
[XJ380/arm32] alive loop=12 led=0x01 ticks=2178 irq=2178 uptime=3221 ms
```

`source=1` 表示闭环收敛成功（`2` 表示走了兜底常量），`iters=1` 表示一次迭代即收敛。
这两个字段是必要的：光看 `uart_clk` 无法区分"实测验证过"和"只是猜的"。

### 3. 本板串口硬件（已从原理图逐脚确认）

| 芯片 | 用途 | 对应 |
|---|---|---|
| **CH9102F** | PS USB 转串口 | `PS_UART_TXD = MIO48` / `RXD = MIO49` → PC 上的一个 USB 串口（本机是 `COM4`，**因机器而异**，写在 `config.py` 的 `SERIAL_PORT`）|
| CH340E | **PL 侧** USB 转 TTL | 另一路，PL 无 UART 逻辑所以不会有数据 |
| JTAG18M02HS2 | USB 转 JTAG | 与上述经 SL2.1S Hub 共用一根 USB 线 |

⚠ 排查时注意：**JTAG 和两路 UART 共用一根 USB 线**（板载 USB Hub），
但它们是各自独立的设备/COM 口。串口没数据时不要怀疑"和 JTAG 抢口"。

### 4. 中断子系统（M1，已完成并在板上验证）

对应 x86_64 侧的 `kernel/pctable/idt.cpp` + `kernel/intr/apic.cpp`。
两者机制差别很大，下面三条是移植时最容易按 x86 直觉写错的地方，
都在 `include/arch/irq.h` 里写了警示：

| | x86_64 | ARMv7-A |
|---|---|---|
| 中断描述符 | 256 项 IDT，每项一个描述符 | 固定 8 项向量表，基址放 VBAR，每项仅一条指令位 |
| 注册方式 | **没有 API**，驱动自己写 IDT；IOAPIC 重定向是硬编码 4 行 | 显式 `INTID → handler` 表，注册与使能分离 |
| 结束中断 | `send_eoi()` 往 LAPIC 0xB0 写 0 | **必须把 `ICCIAR` 读到的 INTID 原样写回 `ICCEOIR`** |
| 优先级方向 | 数值越大优先级越高 | **数值越小优先级越高**（0=最高，0xFF=最低） |

文件：

```
boot/vectors.S          8 项向量表 + IRQ/SVC/Abort 入口 + vectors_install
include/arch/irq.h      异常帧 ABI、INTID 定义、注册 API
src/gic.c               GIC 驱动 + INTID 处理表 + 异常现场诊断
src/timer.c             周期 tick（Cortex-A9 私有定时器，PPI 29）
```

**IRQ 入口的时序要点**（`boot/vectors.S`）：

```
sub  lr, lr, #4      @ IRQ 的 LR 指向"下一条指令",减 4 才是被中断的那条
push {r0-r12, lr}    @ 14 字现场帧;这个布局是汇编与 C 的 ABI
mov  r0, sp          @ 帧指针作为第一个参数
bl   c_irq_handler
pop  {r0-r12, lr}
movs pc, lr          @ 跳转的同时把 SPSR 拷回 CPSR(等价于 x86 的 iretq)
```

**实测验证**（1 kHz tick，串口输出）：

```
IRQ status  : ticks=20 irq_count=20 last_intid=29 spurious=0
Periodic tick is RUNNING (1 kHz, Cortex-A9 private timer).
[XJ380/arm32] alive loop=12 led=0x01 ticks=2178 irq=2178 uptime=3221 ms
[XJ380/arm32] alive loop=44 led=0x01 ticks=8547 irq=8547 uptime=9663 ms
```

| 检查项 | 结果 |
|---|---|
| tick 频率 | 相邻两条状态行 `Δticks=1611` 对应 `Δuptime=1611 ms`，**精确 1000 Hz** |
| 中断丢失 | `irq_count` 与 `ticks` **完全相等**（11743 = 11743），零丢失 |
| INTID | `29`（私有定时器），符合设计 |
| 虚假中断 | `spurious=0` |
| GIC 寄存器 | `GICD_CTLR=1`、`GICD_ISENABLER0` bit29=1、`GICC_CTLR=1`、`GICC_PMR=0xF0` |
| 定时器寄存器 | `LOAD=333332`、`CONTROL=0x07`（使能+自动重载+中断） |
| JTAG 交叉校验 | 心跳槽 10 与槽 11 相等（`0x2DDF = 0x2DDF`），与串口独立确认同一结论 |

驱动注册中断的用法：

```c
gic_init();                                        /* 关中断下配置 */
irq_register(GIC_INTID_A9_PRIVATE_TIMER, handler, NULL);
gic_set_priority(GIC_INTID_A9_PRIVATE_TIMER, 0x80);
a9_timer_start_tick(1000);
gic_enable_irq(GIC_INTID_A9_PRIVATE_TIMER);
irq_global_enable();                               /* 最后才开中断 */
```

顺序不能颠倒：中断可能在处理函数登记之前就打进来。

**已知问题：启动阶段有一次约 67 ms 的 tick 计数阶梯（预先存在，与 MMU 无关）**

现象：主循环前两条状态行报告 `uptime - ticks ≈ 1016 ms`，第三条起变成
`≈ 1086 ms`，此后**长期恒定**（连续 20 秒以上稳定在 ±1 ms，即稳态 0 ppm）。

这个阶梯**不是 MMU 引入的**，用受控 A/B 验证过 —— 把 `mmu_enable()` 调用
摘掉重新编译烧写，出现完全相同的阶梯：

| | loop=1 | loop=4/2 | loop=12/10 | loop=20 之后 |
|---|---|---|---|---|
| 无 MMU（对照）| 1016 | 1018 | **1085** | 1087 恒定 |
| 有 MMU | 1015 | 1014 | **1083** | 1082 恒定 |

稳态精确（相邻两条状态行的 `Δticks` 与 `Δuptime` 逐段相等），
所以这只影响启动头一秒半的计数。

已排除的解释：私有定时器周期不对（`LOAD=333333`，周期正好 1.000 ms）；
中断被长时间屏蔽（`maxgap` 最大只有 6.9 ms，换算不出 67 个 tick）。

尚未定论。为此在 `tick_handler` 里常驻了中断延迟测量
（`maxgap` = 相邻两次 tick 的最大间隔，正常应紧贴 1000 µs；
`lag` = 累计欠下的 tick 数），心跳槽 17 也记录了它。
私有定时器是**电平触发**的，多次 expire 会被合并成一次中断 ——
光看 GIC 状态和寄存器分不出"丢中断"和"时钟不准"，必须直接量间隔。

配套的异常诊断（`c_data_abort_handler` / `c_prefetch_abort_handler` /
`c_undef_handler` / `c_svc_handler`）会打印出错地址与寄存器现场，
对应 x86 侧的 `kernel/wsod/`。

**这些处理函数是"出了事才跑"的代码，正常路径永远走不到** ——
所以专门做了故障注入（`src/fault_test.c`）：内核启动后用 JTAG 往
`0x00020100` 写选择器即可触发，**不需要重新烧录**。没有专门触发过，
就无法区分"处理函数写对了"和"处理函数根本没被调用、只是系统恰好没崩"。

一条命令完成"加载 → 跑起来 → 等中断子系统 → 注入 → 回读寄存器"：

```
<vitis-dir>\Vitis\bin\xsdb.bat tmp-test/jtag/run_kernel_uart.tcl <1|2|3|4>
```

配合串口抓取（会打印原始 hex，波特率不对时能区分"全 0x00 / 全 0xFF / 有字符"）：

```
python tmp-test/run_and_capture.py <串口> 9600 2 <1|2|3|4>
```

（`<vitis-dir>` 与 `<串口>` 都不写死在这里 —— 它们来自仓库根目录的 `config.py`；改动与自检见 `docs/BUILD_ARM32.md`。）

**实测结果**（五个用例各跑一遍，注入前 `ticks` 与 `irqcount` 均相等）：

| 选择器 | 异常 | 关键证据 |
|---|---|---|
| 1 | Data Abort | `DFAR=0x50000000`、`DFSR=0x08`（synchronous external abort）、`pc=0x00100914`、`r3=0x50000000` |
| 2 | Undefined Instruction | `pc=0x00100934`（UDF 指令处）、`r4` 为选择器 |
| 3 | Prefetch Abort（未映射）| `IFAR=0x50000000`、`IFSR=0x08`、`pc=0x50000000` |
| 4 | SVC | `!!! SVC (no syscall layer yet) - diagnostic only, returning !!!`，随后 `SVC returned normally` |
| 5 | Prefetch Abort（XN）| `IFAR=0xE0001000`、**`IFSR=0x0D`（permission fault, section）**、`pc=0xE0001000` |

前三个停机 PC 分别落在 `boot/vectors.S` 中三个向量各自的 `wfe` 自旋循环里 ——
证明处理函数返回后确实停在了预期位置。

**用例 3 与 5 的对比是验证 XN 的唯一手段**：两者都产生 Prefetch Abort，
但 `IFSR` 完全不同 ——

- 3 访问的 `0x50000000` 背后没有从设备 → `0x08` 外部异常；
- 5 访问的 `0xE0001000` 是**已映射**的 UART → `0x0D` 权限故障（被 XN 拦下）。

所以**光看到"Prefetch Abort"不能说明 XN 生效了**，必须核对 `IFSR`。
反过来，如果 XN 没写进描述符，用例 5 就不是"报个错"这么简单：
`blx 0xE0001000` 会把 UART 寄存器的内容当作指令执行，
行为完全不可预测。这个用例本身就是 XN 价值的最好说明。

**SVC 与前三个有本质区别**：它的向量 `_vec_svc` 没有 `wfe` 自旋，
处理完就 `pop {r0-r12, lr}` / `movs pc, lr` 返回到 SVC 的下一条指令。
所以它的处理函数**不能**打印 "System halted"，否则会让人以为系统停了。
用例 4 输出末尾的 `SVC returned normally` 顺带验证了这条返回路径 ——
**那正是将来系统调用要走的机制**（`movs pc, lr` 在跳转的同时把 SPSR 拷回 CPSR）。

**踩坑提醒**：`DFSR`/`IFSR` 的故障状态在 **bit [4:0]**，不是 [3:0]。
`0x08` 在 [3:0] 下看着像"域故障"，实际 `[4:0]=0b01000` 是
"Synchronous external abort"（访问了没有从设备的地址）。上一版就写错了这个
提示，反而误导排障 —— 现在用 `fsr_status_text()` 显式查表。

### 5. MMU 与地址转换（M2，已在板上启用）

**现状：MMU 已打开，1MB 段恒等映射，系统在地址转换下正常运行。**

x86 侧的 `include/mm/page.h` 用的是 4 级页表 + `PTE_*` 位语义，
那套词表在 ARMv7 上**没有对应位置**（`PTE_FRAME_ALLOCATED = 1<<62`、
`PTE_NO_EXECUTE = 1<<63` 在 32 位短描述符里无处安放），
所以是整体重做，而不是改几个常量。

| | x86_64 | ARMv7-A 短描述符 |
|---|---|---|
| 级数 | 4 | 2 |
| 项宽 | 64 位 | **32 位** |
| 一级表 | 512 项 | **4096 项**（4GB / 1MB）|
| 基本粒度 | 4KB | L1 段 = 1MB，L2 小页 = 4KB |
| 权限 | U/S 位 | AP[2:0] 三级 + Domain 域 |
| 内存类型 | PWT/PCD 两位 | TEX[2:0]+C+B+S 组合 |
| 不可执行 | NX = bit63 | XN，**L1 与 L2 位置不同** |

文件分工：

```
include/arch/mmu.h        描述符定义、区域表 API、CP15 位定义（纯头文件）
src/mmu.c                 纯逻辑：描述符编解码、区域表、建表 ← 宿主单测
src/mmu_hw.c              硬件侧：页表实体、CP15 配置、开 MMU ← 只能上板验证
boot/kernel.ld            .mmu_tbl 段（NOLOAD，16KB 对齐 + 两条 ASSERT）
```

拆成 `mmu.c` / `mmu_hw.c` 是必须的：`mmu.c` 被 `tests/test_arm32_mmu.py`
直接编译，一旦引入 `mcr`/`mrc` 内联汇编宿主就编不过，那部分逻辑也就没法单测。
这与 `uart_baud.c` / `uart_ps.c` 的拆法同源。

**区域表**（`src/mmu.c`，未列出的地址一律为 fault）：

| 范围 | 属性 | 说明 |
|---|---|---|
| `0x00000000`–`0x000FFFFF` | Normal NC | OCM（实测 192KB）+ 心跳 |
| `0x00100000`–`0x3FFFFFFF` | Normal WB | DDR，内核在这里运行 |
| `0x40000000`–`0xBFFFFFFF` | Strongly-Ordered | PL（AXI GP0/GP1）|
| `0xE0000000`–`0xE02FFFFF` | Device **+XN** | PS 外设（UART/I2C/SPI/SD/QSPI…）|
| `0xF8000000`–`0xF8FFFFFF` | Device **+XN** | SLCR / SCU / GIC / PL310 |
| `0xFFF00000`–`0xFFFFFFFF` | Normal NC | 高位 OCM 别名 + BootROM |

**三处与 Xilinx 原表的有意偏离**，理由都写在代码里：

1. **低 1MB 用不可缓存**。`0x20000` 是 JTAG 唯一的心跳观测通道，
   而 JTAG 走 DAP/AXI 直接读物理内存、**不经过 CPU 的 L1/L2**。
   这段一旦是写回可缓存，心跳写进去只是躺在 cache 里，JTAG 读到的是陈旧值 ——
   偏偏 MMU 刚开的那几次调试最依赖它。OCM 是片上存储，不可缓存的代价很小。
2. **高位 OCM 别名与低位逐位相同**。同一物理内存的两种映射若属性不同，
   架构上是 UNPREDICTABLE。另外**地址也修正过**：BSP 的
   `XPAR_PS7_RAM_1 = 0xFFFF0000`，不是想当然的 `0xFFF00000`；
   本区域从 `0xFFF00000` 起是段粒度（1MB）所致，Xilinx 原表也记录了同样限制。
3. **未列出的地址填 fault**，而不是像 Xilinx 那样把大片保留区也填成有效映射。
   Xilinx 表覆盖的 NAND/NOR/QSPI-XIP 在这块板子上都不存在；
   填成 fault 能让误访问立刻暴露，而不是发一次可能挂死 AXI 总线的事务。

**XN（不可执行）的取舍** —— 只有确定是纯数据的区域才标：

| 区域 | XN | 理由 |
|---|---|---|
| PS 外设 / SLCR-GIC | ✅ | 纯数据 |
| 低 1MB（OCM）| ❌ | 计划里的 SMP 方案要用 OCM 放 CPU1 启动跳板，那段代码将来要在 OCM 里执行 |
| PL | ❌ | 将来可能有需要取指的东西（软核 BRAM / 从 PL 加载的代码段）；当前 PL 只是占位设计 |
| DDR | ❌ | 内核代码本身在这里 |

⚠ **XN 与 AP 受不同机制管辖**：AP 只在 DACR 该域为 client 模式时才参与判定，
而 **XN 始终生效**。所以即使在 manager 模式下 XN 也是当前唯一能真正约束内核自身的属性位。

**DACR 已从全 manager 切到全 client**：现在 AP 位才真正参与判定。
行为上应当完全不变（所有区域 AP 都是全权限，系统只跑在 PL1）——
这一点本身就是最好的验证：切过去如果坏了，说明某个描述符的 AP 位是错的。
这一步是为 M4 用户态铺路，趁故障原因最单纯的时候把这条路径验证通。

**开 MMU 的过程可诊断**：每阶段写心跳槽 16，挂死时读回即为最后进入的阶段
（0=未开始 1=已建表 2=已配 TTBR0/DACR 3=正要开 4=已开 `0xEE`=自检失败）。

**当前只开了地址转换（`SCTLR.M`），未开 D-Cache / I-Cache**，这是有意的风险切分：
打开 D-Cache 前必须让整个数据缓存失效，否则残留脏行会在之后被写回、
覆盖正确数据；而"整块 D-Cache 失效"在 ARMv7 上不是一条指令，
要按 set/way 遍历（依赖 CCSIDR 读出的缓存几何），属缓存维护范畴 ——
`arch/cpu.h` 现有原语全是按地址的（MVA 形式），恰好没有整块失效。
所以缓存维护与开缓存一起留给 M2-5。代价是系统仍很慢，
换来的是 M2-3 的失败原因**只有一种**：页表错了。

### 6. 缓存（M2-5，已在板上使能）

```
include/arch/cache.h      几何解码、CLIDR/CCSIDR 位定义（纯逻辑，宿主单测）
src/cache_hw.c            CP15 访问、按 set/way 的整块操作、SCU/ACTLR、使能与基准
```

**现状：L1 D-Cache 与 I-Cache 已使能，实测加速 6 倍。L2（PL310）仍未动** ——
`ps7_init` 里没有任何 L2 代码（实测确认），所以它处于复位状态。

**这一步踩到一个很隐蔽的坑，值得单独记下来。**

第一版使能缓存后，寄存器看着完全正常：

```
Caches      : D=ON I=ON  SCTLR=0x08C5107D     <- C 位与 I 位都是 1
Cache bench : off=24704 us  on=24701 us  speedup=1x
```

**但缓存完全没有加速效果。** 原因是移植时漏了两步：

| 漏掉的步骤 | 后果 |
|---|---|
| 使能 SCU（`0xF8F00000` bit0）| 可共享访问的一致性无从保证 |
| 设置 ACTLR 的 SMP 位（bit6）| 本核不参与 SCU 一致性 |

而区域表把 DDR 映射成 **Shareable（S=1）**（与 Xilinx 原表一致）。
Cortex-A9 上可共享的访问要走 SCU，SCU 没开、SMP 位没置时，
硬件干脆不把这些访问放进 L1 —— 于是缓存"开着但没用"。

补上之后：

```
Coherency   : SCU=0x00000003 ACTLR=0x00000041
Cache bench : off=24693 us  on=3704 us  speedup=6x
Cache check : caches are demonstrably effective
```

⚠ **这个坑光看寄存器是发现不了的**，SCTLR 的 C/I 位读回来一直是 1，
系统也一直"正常工作"。只有耗时基准能揭穿它。
所以 `cache_benchmark_us()` 是这套验证的核心，不是可有可无的装饰。

（同一类问题还有一次：基准测试**第一版被编译器整体优化掉了** ——
缓冲区在 `.bss` 里、编译期就知道全是 0，于是 GCC 把求和折叠成常数，
读出"20 万次访存只用 7µs"。已加 `volatile`，理由写在代码注释里。
这个错误恰好被基准自己的判据抓到：速度比 1x 触发警告。
**验证工具本身失效比没有验证更危险**，所以判据必须建立在
"物理上可能不可能"上，而不是"代码跑完了没有"。）

**CLIDR 实测值纠正了一个凭记忆写错的假设**：本板 `CLIDR = 0x09200003`
（LoC=1、LoUIS=1），而不是想当然的 `0x00000003`。单测基准值已改用实测值。

**L2（PL310）不在 CLIDR 里** —— CLIDR 只描述 L1。所以"用 CP15 按 set/way
循环失效所有级别"这个看起来更完备的通用写法，在本芯片上只覆盖 L1，
L2 必须走它自己的寄存器（`0xF8F02000`）。这条直接决定了下一步的做法。

**整块操作必须用 set/way 而不是按地址**：按地址（MVA）只能覆盖你
*想得起来*的地址，而"打开缓存前清掉复位残留"这个问题恰恰是
"不知道缓存里有什么"。对应约束是：set/way 整块操作在多核下不安全
（会丢掉别的核未写回的脏行），本项目只在单核启动阶段、使能缓存之前用。

> ⚠ **调试注意**：D-Cache 开了之后，**JTAG 读 DDR 里的变量可能读到陈旧值**
> （JTAG 走 DAP/AXI，不经过 CPU 的 L1/L2）。心跳区在 OCM 里、被映射为
> 不可缓存，所以心跳本身仍然可靠 —— 这也是当初把低 1MB 设成不可缓存的
> 另一个回报。但要读内核在 DDR 里的变量时，得先让 CPU 把它们写回。

**按地址（MVA）的区间维护**（DMA 真正要用的接口）：

```c
cache_clean_range(addr, size);              /* 内存 -> 设备:写回到一致性点 */
cache_invalidate_range(addr, size);         /* 设备 -> 内存:丢弃旧副本   */
cache_clean_invalidate_range(addr, size);   /* 双向交替使用              */
```

⚠ **区间会向外对齐到整行**。缓存维护按行生效，而一行的所有权与请求范围无关：
若只覆盖范围内的行，范围两端各有一行只有一部分落在里面时，
那一行的另一半就得不到处理 —— 对 DMA 来说就是缓冲区首尾各有一小段
没被写回（或没被失效），表现为"大部分时候对，偶尔错几个字节"。
`cache_align_range()` 负责这件事，它是有语义的逻辑，所以放在
`cache.h` 里并配了单测（含溢出、空区间、退化几何等边界）。

**不需要 DMA 也能验证 clean/invalidate 的语义** —— 靠的是
"缓存与内存是两份副本"这个事实：

| 测试 | 步骤 | 期望 | 若失败说明 |
|---|---|---|---|
| 1 | 写旧值→写新值→`invalidate`→读 | 读到**旧值** | invalidate 没丢弃缓存副本 |
| 2 | 写新值→`clean`→`invalidate`→读 | 读到**新值** | clean 没把数据写回内存 |
| 3 | 未整行对齐的区间 | 数据自洽 | 对齐逻辑漏掉了边界行 |

板上实测：`Cache maint : line=32 B  clean/invalidate selftest=PASS`。

另外把 `arch/cpu.h` 里原来那三个**朴素**的 MVA 区间函数删掉了
（它们按 32 字节硬编码步进、不做对齐、不处理边界），
换成只提供**单地址**原语，区间语义统一收进 `cache_hw.c` ——
避免同一件事有两套实现，其中一套还是错的。

**L2（PL310，512KB）已使能**：

```
Cache L2    : ON  ID=0x410000C8 TYPE=0x9E300300 CTRL=0x00000001
L2 bench    : 128KB working set  on=11212 us  off=15989 us  on-again=11213 us
L2 check    : effective (off/on = 1.42x)
Cache maint : line=32 B  clean/invalidate selftest=PASS
```

顺序与 Xilinx `boot.S` 一致：**L1 先开，再初始化 L2**。
`ps7_init` 里没有任何 L2 代码（实测确认），所以在此之前它一直处于复位状态。

**"寄存器读回 1"不算验证。** L2 使能后我一度以为已经验完了，但 4KB 的
L1 基准工作集根本装不进……准确说是**装得进 L1，压根碰不到 L2**——
那个加速比证明不了 L2 的任何事。补了一个受控 A/B：用 **128KB 工作集**
（远超 32KB 的 L1、远小于 512KB 的 L2），依次测「L2 开 → 关 → 再开」。

| 状态 | 耗时 |
|---|---|
| L2 开 | 11212 µs |
| L2 关 | 15989 µs |
| L2 再开 | 11213 µs |

**结论：L2 确实在缓存，但只有 1.42 倍，不是我最初想当然的"一个数量级"。**
顺序访问被 PL310 的预取掩盖了相当一部分延迟，纯读循环又比真实负载更友好。
第一版判据按 2 倍写，于是误报了 WARN —— 判据已改为
「关掉明显更慢 **且** 重新打开能精确回到原水平」。

其中**第二条才是关键证据**：它同时验证了 `disable/enable` 没有副作用
（直接清使能位会丢掉尚未写回的脏行，是关缓存最容易出错的地方）。
加速比的绝对大小反而不能当阈值 —— 它取决于工作集、访问模式与预取行为。

⚠ **Zynq 上这颗 PL310 的寄存器偏移与通用 PL310 手册不一致**，
而且不一致的恰好是两个"按 Way"与"按地址"的操作：

| 偏移 | 通用 PL310 | 本芯片（Zynq）|
|---|---|---|
| `0x0770` | Invalidate Line by PA | Invalidate by PA | 
| `0x077C` | Clean&Invalidate by PA | **Invalidate by Way** |
| `0x07B0` | Clean Line by PA | Clean by PA |
| `0x07F0` | Invalidate Way | **Clean&Invalidate by PA** |
| `0x07FC` | Clean&Invalidate Way | Clean&Invalidate Way |

**`0x77C` 与 `0x7F0` 的功能在两者之间是对调的。** 用错不会报错，
只会静默地把"失效整条 way"当成"清洗一行地址"来做。
本实现的取值来自 AMD/Xilinx standalone BSP 的 `arm/cortexa9/xl2cc.h`
（随这颗芯片一起出货的定义），并在代码里标注了这个差异。

**L1 与 L2 必须一起维护，且顺序不能反**：

- clean 必须**先 L1 后 L2** —— 先清 L2 的话，L1 随后写回的脏行会留在 L2 里，
  而 L2 的清洗已经做完了，这些数据最终到不了 DDR；
- invalidate 也必须**先 L1 后 L2** —— 反过来 L1 里残留的旧副本会一直用下去。

只做 L1 的危害：Zynq 的 L2 位于 CPU 与 DDR 之间，PL 侧主设备访问 DDR 会经过它，
数据停在 L2 里没落到 DDR 时，**CPU 这边一切正常，问题只在设备侧偶发出现**。
`cache_*_range()` 已经两级都做，顺序写死在同一个函数里。

**勘误处理**（`xil_errata.h` 的 MIT 定义）：

| 勘误 | 影响 | 本实现 |
|---|---|---|
| PL310 588369 | 单条"清洗并失效"**不会失效已经干净的行** | 已规避：拆成"先清洗、再失效"两步 |
| PL310 727915 | Background Clean&Invalidate **by Way** 会导致数据损坏 | **未使用那条路径**（初始化用按 Way 失效，区间用按地址），已在代码里记录规避方法备查 |
| ARM 775420 | 会中止的数据缓存维护操作可能导致死锁 | 未处理：当前维护操作只作用于已映射的内核缓冲区 |

### 7. 设备描述层（M3，已在板上验证）

**为什么 ARM 必须有这一层**：x86 是枚举（PCI 配置空间 / ACPI），硬件必须回答
"你是谁"；ARM 这边连枚举这个概念都不成立。四条落差里最要命的是——
**PL 侧 IP 的参数读硬件根本读不出来**：AXI GPIO 的数据寄存器宽度恒为 32 位、
通道数不体现在任何 ID 寄存器里。描述不是可选优化，是唯一的信息来源。

```
arch/arm32/include/arch/plat_device.h   描述模型（照 DTS 的语义）
arch/arm32/src/plat_device.c            匹配与 probe 循环（纯逻辑，宿主单测）
arch/arm32/board/xparameters.h          生成器输入（从 XSA 导出，固化进仓库）
tools/gen_board_desc.py                 生成器
arch/arm32/{src,include/arch}/board_devices.*  生成物（提交进仓库，便于 diff）
arch/arm32/src/board.c                  驱动表、PL 覆盖表、诊断输出
arch/arm32/src/axi_gpio.c               AXI GPIO 驱动（本层的第一个用户）
```

**描述模型与落地形式是两个独立的轴**：模型照 DTS（节点 / `compatible` /
`reg` / `interrupts` / 属性 / `status`），落地形式是"从 `xparameters.h` 生成的
C 表"。换 DTB 时驱动代码一行都不用动。

**中断号解码收在生成器里**：`_INTERRUPTS` 不是 INTID，是
"相对号 + 触发类型 + SPI/PPI"的编码，实际 INTID 还要加偏移（SPI +32 / PPI +16）。
本板实测对照：`SCUTIMER 0x13100d → 29`（私有定时器）、`QSPI 0x4013 → 51`。

板上输出：

```
 Board devices: 10 nodes (10 active)
   [on ] axi_gpio_0      0x41200000 irq-   xlnx,axi-gpio-2.0
   [on ] scutimer        0xF8F00600 irq29  arm,cortex-a9-twd-timer
   ...
 Board probe  : total=10 disabled=0 probed=1 unclaimed=9 failed=0
 LED self-test: AXI GPIO at 0x41200000, 8-bit, dual-channel
 LED check    : write/readback PASS (6 patterns)
```

#### 受控 A/B：证明这一层是承重的，而不是装饰

"probed=1"这种日志是**做 probe 的那段代码自己打印的** —— 匹配循环若有
微妙错误，它照样会打印出一组自洽的数字。这与心跳里那个 LED 图案属于同一类
"自证"。所以专门做了两次**破坏性实验**，看行为是否随之改变：

| 实验 | 改动 | 板上结果 |
|---|---|---|
| **A/B-2** | PL 覆盖表改成永不匹配 | `disabled=1 probed=0`、节点显示 `[pl ]`、`LED WARN`、`ledcheck=0xFFFFFFFF`、**物理 LED 全灭** |
| **A/B-1** | 从描述里抽掉 `xlnx,is-dual` | 节点仍 `[on ]`、驱动**匹配上了 compatible 但主动拒绝**，因此 `probed=0 failed=1`（不是 `unclaimed`）、LED 同样全灭 |

**A/B-1 顺带在真板上验证了 `failed` 与 `unclaimed` 的区分**：两者都表现为
"没起来"，但 `unclaimed` 说明描述表里的设备没有任何驱动声明认识它（驱动还没写），
`failed` 说明有驱动认识但全部放弃（驱动写了但不认这个硬件）——排查方向完全不同。

**这两次实验也回答了"流水灯和 M3 之前一模一样"的疑问**：行为不变是预期的
（重构不该改变行为），但描述层确实是**承重**的 —— 描述一坏，灯就灭。

#### 一处计划偏差

计划 D6 原文是"PL 节点一律 `enabled = false`"。实际做成了
**生成器默认 false + 手写覆盖表启用**：因为 `xparameters.h` 只说明
"**设计里**有这个 IP"，不说明"**比特流已加载**"——后者是运行时事实，
生成器不该替它做假设。`board.c` 的 `g_pl_present[]` 就是补上那个事实的地方，
也是"换比特流后该改哪里"的唯一答案。

### 8. 串口命令通道（M3-7）：已打通 —— 根因是 XSA 把 MIO bank 电压配错了

**目标**：不改固件就能在现场查板子 —— 一个极小的行编辑命令通道
（`help` / `ver` / `uptime` / `dump` / `probe` / `selftest` / `peek <hex>`）。

**已实现**：`shell.c`（行编辑 + 命令分派）、`uart_loopback_selftest()`（本机环回自检）、
`verify_board.py`（抓串口 → 解析自检报告 → 退出码即判定）。

#### 验收（板上实测，冷启动）

**下面这组是换成修正后的 XSA、且加载器里已无任何补偿代码之后重跑的。**

| 判据 | 结果 |
|---|---|
| `python tmp-test/verify_board.py --load` | **17 passed, 0 failed**，退出码 0 |
| `python tmp-test/shell_test.py --load` | **10/10 通过**（`help`/`ver`/`uptime`/`dump`/`probe`/`selftest`/`peek`/未知命令/前缀/空行），退出码 0 |
| `python tmp-test/mio_guard_test.py` | 注入错误配置 → 38 个引脚报错；正确配置 → 0，**守卫有效** |
| 破坏性 A/B（见下） | 行为随 `MIO_PIN_49` 的 `[11:9]` 一位来回翻转 |

#### 能用的部分（板上实测）

串口输出方向**完全正常**，启动横幅、设备表、自检报告、shell 提示符都能收到：

```
 Serial command channel ready. Type 'help'.
>
```

`uart_loopback` 自检 **PASS** —— 它把 `MR.CHMODE` 切到本机环回，
在**控制器内部**把 TX 接回 RX，所以它证明了：接收移位逻辑、RX FIFO、
波特率时钟、`RX_EN` 全都是好的。

#### 当时卡住的现象（已解决，根因见本节末尾）

板子一个字节都收不到。两个**相互独立**的发送端都试过：
本仓库的 `tmp-test/shell_test.py`（pyserial）和用户自己的串口终端，
发 `help\r` 均无任何回显。

#### 证据链（寄存器读数已逐条对照 Xilinx 头文件，不是凭记忆）

| 读数 | 值 | 含义 |
|---|---|---|
| `UART1.SR` (0xE000102C) | `0x0A` | `RXEMPTY=1`、`RXFULL=0`、**`RXOVR=0`** → RX FIFO 从未进过任何字节 |
| `UART1.CR` (0xE0001000) | `0x114` | `RX_EN=1`、`RX_DIS=0`、`RXRST=0` → 接收已使能且没被按住复位 |
| `MIO_PIN_49` (0xF80007C4) | `0x000016E1` | Vivado 自己为 UART1 RX 生成的值，bit11 `DisableRcvr=0` → 输入缓冲已开 |
| `MIO_PIN_48` (0xF80007C0) | `0x000016E0` | 同上（TX 侧），而 TX 实测是通的 |
| `UART1.MR` | `0x20` | 8N1、`CHMODE=00`（普通模式）—— TX 能正确解码即证明该寄存器对两个方向都成立 |

`SR` 只有 5 个位，容易看错，这里按 `uartps_v3_14/src/xuartps_hw.h` 逐位核对：

```
bit0 RXOVR   bit1 RXEMPTY   bit2 RXFULL   bit3 TXEMPTY   bit4 TXFULL
```

`0x0A = 0b01010` → `RXEMPTY=1` + `TXEMPTY=1`。**TX FIFO 空、RX FIFO 也空，
且没有溢出** —— 也就是数据根本没到控制器。

补充一条**消歧证据**：`ps7_init` 对 `MIO_PIN_48/49` 的写法是
`mask_write 0xF80007C0 0x00003FFF 0x000016E0` 与 `... 0x000016E1`，
掩码只覆盖 bit[13:0]。既然 MIO48 在**同一套复用配置**下确实作为 UART1 TX 工作，
MIO49 的复用就是对的 —— 复用选择不构成嫌疑。

#### 两次**无效**的软件探测（都记下来，避免以后当成结论）

想从软件侧直接看 MIO49 焊盘上的电平，试了两条路，**两次都被自己的对照组挡下来了**。

**(a) 引脚复用给 UART 时读 `DATA_RO`** —— 一次误判，差点让你去查线。

`DATA_RO` 号称"与引脚复用功能无关"。让 PC 交替连发 `0x00`/`0xFF`，
JTAG 每 250 ms 采一次（偏移取自 `gpiops_v3_14/src/xgpiops_hw.h`：
`DATA_RO = 0x60`、bank 步进 `0x04`，MIO[53:32] 属 bank1 → 读 `0xE000A064`，
bit16=MIO48、bit17=MIO49）。

240 个采样点里 **bit17 恒为 0**。但脚本里放了对照：**bit16（已知正被板子驱动的 TX）
也恒为 0**。对照组同时为 0，说明 `DATA_RO` 在引脚复用给外设后看不到焊盘 ——
**实验不成立，不能据此说"引脚被拉低"**。脚本保留为 `tmp-test/pad_probe.py`。

**(b) 临时把 MIO49 改回 GPIO 后再读** —— 对照组又失败了。

思路是把 `MIO_PIN_49` 从 `0x16E1`（UART1）临时写成 `0x0601`（GPIO，
`0x0600` 是 `ps7_init` 给 MIO7/MIO8 的 GPIO 值），读完再写回。
写 SLCR 前先解锁（`0xF8000008 ← 0xDF0D`）—— MIO_PIN 受 SLCR 保护。
写入和还原都成功（读回 `0x00000601` / `0x000016E1`）。

但对照又挂了：拿 MIO7/MIO8 当参照，它们的 `DATA_RO` 读数**纹丝不动**。
于是再单独做了一次自证实验（`tmp-test/jtag/gpio_dataro_check.tcl`）：
直接从 JTAG 驱动 MIO7/MIO8（`DIRM0=0x180`、`OEN0=0x180` 都是输出），
再读 `DATA`/`DATA_RO` —— 三者**恒为同一个值 `0x507FFE03`，写入没有任何效果**。

按 Xilinx `xgpiops.c` 核对了掩码/数据的高低半字约定
（"upper 16 bits is the mask and lower 16 bits is the data"）和寄存器地址
（`Bank*0x08 + 0x00` = `0xE000A000`），**编码没错，但它就是不生效**。
所以这个读数不可信，**关于焊盘电平我没有任何可信证据，不下结论**。

**结论：`DATA_RO` 这条路走不通，不再纠缠。** 判断焊盘只能靠外部仪器。
（教训：两次都是"对照组同时不动"暴露的问题。没有对照位，两次都会得出错误结论。）

#### 信号链已从官方图纸逐段确认

翻官方资料包里的那几份 PDF（底板原理图、核心板原理图、
管脚信息表、硬件说明文档），把整条链核实了一遍。**三个独立来源互相印证**：

| 来源 | 内容 |
|---|---|
| AC880 底板原理图 第 1 页 | 网络对应表：`PS_UART_TXD ≡ PS_MIO48`、`PS_UART_RXD ≡ PS_MIO49` |
| AC850 核心板原理图 第 2 页 | `PS_MIO48_501 → 球 D11`、`PS_MIO49_501 → 球 C14` |
| AC850 核心板原理图 第 8 页 | P2 连接器：**27 脚 = `PS_MIO48`**、**45 脚 = `PS_MIO49`**、1 脚 = `VCC1P8` |
| 硬件说明文档 第 23 页 | `PS_UART_RXD → MIO49`、`PS_UART_TXD → MIO48` |

底板原理图第 4 页（`04_Debug_Port`）给出的电路是**直连**，排查掉了一个重要嫌疑：

```
PS_UART_RXD ──┬── R150 1K ── VIO_1P8        (1.8V 上拉)
              ├── CH9102F pin 21 (TXD)
              └── P2/45 ──> AC850 45 脚 ──> MIO49 (球 C14)   ← 坏在这里
PS_UART_TXD ──┬── R151 1K ── VIO_1P8
              ├── CH9102F pin 20 (RXD)
              └── P2/27 ──> AC850 27 脚 ──> MIO48 (球 D11)   ← 实测通畅
```

**两边都是 1.8V，中间没有电平转换芯片。**（CH9102F 的 `VIO` 脚也接 `VIO_1P8`；
两个 `SN74LVC1T45` 电平转换器属于**另一路** `PL_UART`/CH340E，与 PS 串口无关。）
所以"电平不匹配"这个可能性可以排除，也解释了为什么 TX 方向工作正常。

顺带发现两个**零工具就能看的诊断点**：`PS_TX_STATUS` / `PS_RX_STATUS`
经 `R53`/`R54`（0Ω）分别驱动 CH9102F 的 `TXS`(14) / `RXS`(13) 脚上的
红色 `D12` 与黄色 `D13` 指示灯。板子在持续打印，**红灯应常闪**；
**在 PC 端敲键盘时黄灯应闪** —— 黄灯闪而板子不响应，就直接把故障锁到
`CH9102F pin 21 → MIO49` 这一段。

#### ★ 真正的根因：`opjtmp.xsa` 把 MIO bank 1 的电压声明成了 3.3V ★

**上面"断点在片外"的结论是错的。** 硬件是好的，问题在 PS 的配置上。

`.hwh`（`tmp-test/zynq/design_1.hwh`）里 54 个 MIO **全部**是 `LVCMOS 3.3V`：

```
<PARAMETER NAME="PCW_MIO_48_IOTYPE" VALUE="LVCMOS 3.3V"/>
<PARAMETER NAME="PCW_MIO_49_IOTYPE" VALUE="LVCMOS 3.3V"/>
```

而本板 MIO bank 1 实际供电是 **1.8V**，三条独立证据：

| 证据 | 内容 |
|---|---|
| AC850 核心板原理图 第2页 | `VCCIO_BANK1` 接 `VCC1P8` |
| AC880 底板原理图 第4页 | `PS_UART_RXD/TXD` 经 1K 上拉到 `VIO_1P8`；CH9102F 的 `VIO` 脚同样接 `VIO_1P8` |
| **vendor 参考工程 `02_key_ctrl_led` 的 `.hwh`** | bank 1 的 38 个 MIO 全是 `LVCMOS 1.8V`，bank 0 的 16 个是 `LVCMOS 3.3V` |

**vendor 的 `ps7_init.c` 把 `MIO_PIN_xx` 的位域解码直接写在注释里**，字段一目了然：

```
0XF80007C0[7:5]   = 功能选择(Lx_SEL)
0XF80007C0[11:9]  = IO 标准         <-- 3 = LVCMOS33,1 = LVCMOS18
0XF80007C0[12:12] = 上拉使能
EMIT_MASKWRITE(0XF80007C0, 0x00003FFF, 0x00001200),   <-- vendor(bank1=1.8V)
```

对照两边同一引脚：

| | `MIO_PIN_7`(bank0) | `MIO_PIN_48`(bank1) | `MIO_PIN_49`(bank1) |
|---|---|---|---|
| vendor（正确） | `0x0600` → IO=`3` | `0x1200` → IO=`1` | `0x1200` → IO=`1` |
| 本 XSA（错误） | `0x0600` → IO=`3` ✓ | `0x16E0` → IO=**`3`** ✗ | `0x16E1` → IO=**`3`** ✗ |

**机制**：`MIO49` 的输入缓冲按 **LVCMOS33** 配置，其 VIH 约 2.0V，而 CH9102F 只能
驱动到 1.8V —— 引脚**恒读低**，于是**没有下降沿**，UART 永远检测不到起始位。
现象就是 `SR.RXEMPTY` 恒为 1、`RXOVR` 恒为 0，**与"线断了"在软件侧完全不可区分**。

**输出方向为什么不受影响**：输出摆幅由物理 VCCIO 决定，仍然是 1.8V，
而 CH9102F 的输入参考也正好是 1.8V —— 所以 TX 一直正常。**这正是它极具迷惑性的原因。**

**验证（改一个字段即可）**：把 `MIO_PIN_49` 的 `[11:9]` 从 3 改成 1（即 `0x16E1 → 0x12E1`），
其余位（功能选择 `[7:5]=7`、上拉 `[12:12]`、`bit0`）原样保留，**命令通道立刻工作**：

```
> help
 commands:
   help      列出全部命令
   ver       打印版本与板级信息
   uptime    开机时长(毫秒)
   dump      打印设备描述表(全部节点与状态)
   probe     重跑驱动匹配并打印统计
   selftest  重跑可重复的那几项自检
   peek      peek <hex-addr>: 读 32 位;未映射地址会 Data Abort
```

**影响不止串口**：bank 1 是 MIO16-53（38 个脚），以太网、USB、SD 的输入都在里面，
**同样受影响**。

#### 受控破坏性 A/B：证明这一位是承重的

上面只是"改完能用了"，还不能排除"碰巧改好了别的东西"。所以做了 A/B：
用 JTAG 在运行中把 `MIO_PIN_49` 的 `[11:9]` 在这两个值之间来回切，
每个阶段连发 5 次 `ver` 取一致性（避免把偶发丢包当成现象）。
脚本：`tmp-test/mio_ab_test.py`。

| 阶段 | `MIO_PIN_49` | `[11:9]` | 收到应答 |
|---|---|---|---|
| A | `0x000012E1` | 1 = LVCMOS18 | **5/5** |
| B | `0x000016E1` | 3 = LVCMOS33 | **0/5** |
| A' | `0x000012E1` | 1 = LVCMOS18 | **5/5** |

行为完全随这一位翻转，且可逆 —— 结论成立。

#### 那 bank 0 呢？（逐脚核对过，不用改）

只说 bank 1 容易让人以为"整个 MIO 都配错了"。实际逐脚对比 vendor 与本 XSA 的
`MIO_PIN` 值后，结论是**只有 bank 1 错**：

| bank | 对应 MIO | vendor | 本 XSA | 差异位 | 结论 |
|---|---|---|---|---|---|
| **bank 0** | MIO0-15 | `[11:9]=3` (3.3V) | `[11:9]=3` (3.3V) | — | ✅ **一致** |
| **bank 1** | MIO16-53 | `[11:9]=1` (1.8V) | `[11:9]=3` (3.3V) | **bit10** | ❌ 全部 38 脚都错 |

硬件侧也对得上：核心板原理图第 3 页 `VCC3P3 VCCIO_BANK0` —— **bank 0 实际就是 3.3V**，
声明与实物一致。所以修复脚本**只动 MIO16-53，不碰 bank 0**。

**bank 0 上唯一的差异**：MIO1-6 的 `bit1`（vendor `1`，本 XSA `0`），MIO0 与 MIO7-15
逐字节完全相同。已排除的可能：

- **不是 IO 标准问题** —— `bit1` 不属于 `[11:9]`，两边的 `[11:9]` 都是 3；
- **不是 QSPI 配置差异** —— 两边的 `PCW_QSPI_*` 14 个参数**逐项相同**，
  且 `PCW_QSPI_PERIPHERAL_ENABLE=0`（两个设计都没启用 QSPI）；
- MIO1-6 正好是 QSPI 的 6 个脚，vendor 工程里也只有这 6 个脚 `bit1=1`。

**未定论**：`.hwh` 里只有 `PCW_VALUE_SILVERSION`（两边都是 3），拿不到工具版本，
所以无法确证它是不是 Vivado 版本差异导致的生成器默认值漂移。
**但它不涉及 IO 标准、且落在两个设计都未使用的引脚上，因此不动它** ——
在证据不足时改一个不理解的位，风险大于收益。

#### 顺带修掉一个真 bug：环回自检会冲掉待发输出

改完 IO 标准后 `verify_board.py` 仍然报"报告不完整"，而且**两次抓包字节完全一致**
（自检区间 786 字节逐字节相同），所以不是抓包竞态，是板子真的少打了。

丢的是 `CHECK uart_baud_ppm` 整行 + `uart_clock_source` 只剩半行。根因在
`uart_ps.c` 的 `uart_loopback_selftest()`：

```c
mmio_write32(base + UART_CR, UART_CR_TXRST | UART_CR_RXRST);   /* ← 有问题 */
```

`uart_putc` **只等 `TXFULL` 清零（保证 FIFO 里有一个空位）就返回，并不等字节真的
发出去**。9600 波特下 64 字节的 FIFO 要 ~67 ms 才排空，所以自检跑到这里时，
刚才那几行报告文本还压在 FIFO 里 —— **`TXRST` 把它们整个丢掉了**。

修法与后续暴露出的第二个问题：

1. **先 `uart_wait_tx_empty()` 排空，再动 FIFO**；并且**只写 `RXRST`，不写 `TXRST`**
   （TX 已经空了，没有东西需要冲）。
2. 去掉 `TXRST` 后又露出第二个问题：`uart_loopback = 0 FAIL`，而且报告里多出一个
   孤立的 `U`。原因有两层：
   - `SR.TXEMPTY` 只表示 **FIFO 空**，不表示**移位寄存器空**。切到环回那一刻最后一个
     字节还在往外移，它会绕回 RX 排在探针前面，只读第一个字节就会误判。
     改成**在最先收到的 8 个字节里找探针**（不猜延时、不依赖波特率）。
   - 本地环回**并不切断 TX 引脚的输出**，探针字节会漏到线上。所以把这个自检
     **挪到横幅之前**调用（结果缓存给后面的报告用）—— 漏出的字符落在报告区间之外，
     不会把行首的 `CHECK` 污染成 `UCHECK`。而解析脚本是按行首 `^CHECK ` 匹配的，
     被污染的那一行会被整条漏读（这正是当时"解析到 16 条而声明 17 条"的原因）。

**这个坑的价值在于它的表现形态**：它看起来像"串口丢包"或"printf 截断"，
而真相是"打印完还没发出去的那一段被后续的寄存器重置吃掉了"。
判定的关键是**确认丢失是确定性的**（连抓两次逐字节相同），
确定性就把"抓包竞态"这一类解释全部排除掉了。

#### 顺带修正一个测试期望值

`tmp-test/shell_test.py` 里原有 `Case("peek 41200008", [... "0x00000000"])`，
注释写"自检结束后清零了"。但 `0x41200008` 正是 AXI GPIO 的 LED 数据寄存器，
**主循环的流水灯一直在改写它**，所以几乎不可能是 0（实测读回 `0x00000010`）。
这是**期望值定错**，不是 `peek` 有问题。改成读 OCM 心跳的 magic 槽
（`0x4F583338`，内核写死的常量），顺便与 JTAG 加载器里的同名校验形成交叉验证。

#### 修法：已从源头修好（2026-09-13 重新导出 XSA）

重新导出的 `opjtmp.xsa` 把 MIO bank 1 声明成了 **1.8V**，Vivado 现在原生生成：

```
IOTYPE 分布: 16 × LVCMOS 3.3V (MIO0..15) + 38 × LVCMOS 1.8V (MIO16..53)
MIO_PIN_48 = 0x000012E0      MIO_PIN_49 = 0x000012E1
```

**这正是当初手工推导出来的补偿值**（`0x16E0/0x16E1` 减 `0x400`）—— Vivado 生成的
与手工推的**逐位相同**，互为独立印证。

新旧 `ps7_init` 做了完整 diff 后再替换，确认**只有 `ps7_mio_init_data_*` 三个 proc 变了**
（同一套配置的三份硅版本变体），其余 23 个 proc（PLL / 时钟 / DDR / 外设 / post_config /
debug / `ps7_init` 本体）**逐字节相同** —— 也就是只改了 MIO 电气属性，没碰任何别的东西。

新 XSA 里也带了 `opxjtmp.bit`，但它的 `.hwh` 只含 `processing_system7`、**没有 PL IP**，
所以**没有采用**：PL 侧继续用 `AXI_GPIO_1_SOFT` 的比特流，保住了 AXI GPIO 流水灯。
「PS 配置」与「PL 比特流」交叉组合这件事依旧成立。

#### 补偿已退役，改为加载时硬校验

原来的补偿脚本 `ps7_mio_bank1_18v.tcl` 已删除。取而代之的是
`tmp-test/zynq/ps7_mio_bank1_check.tcl`：加载器在 `ps7_init` 之后**只校验、不改寄存器**，
MIO16-53 里只要有一个不是 LVCMOS18 就**当场 exit 1** 并列出具体引脚：

```
FAIL:有 38 个 MIO16-53 不是 LVCMOS18(bank 1 应为 1.8V):
    MIO16 = 0x00001600  [11:9]=3
    ...
  这是 XSA 的 PS7 配置问题,不是内核问题。请重新导出 XSA 并把
  MIO bank 1 的电压设成 1.8V。临时补偿可用 ps7_mio_bank1_18v_fixup。
```

**为什么保留校验而不是直接删干净**：这类配置错误的表现是"寄存器全对但收不到数据"，
没有任何报错 —— 正是它让这次排查绕了一大圈。把它变成一条加载时断言，
下次换 XSA 出错会当场大声失败，而不是再花一轮去查线、查芯片、查焊点。

**守卫本身也被测了**（没被触发过的守卫等于没有守卫）：
`tmp-test/mio_guard_test.py` 把正确的 `ps7_init` 机械改坏（MIO16-53 的 `[11:9]` 从 1 改回 3，
即每个值 `+0x400`，共 3 套硅版本变体 × 38 行 = 114 处），跑一遍 `ps7_init`，结果：

| 场景 | 不合规引脚数 | 期望 |
|---|---|---|
| 注入旧式错误配置 | **38** | 38 → 会拦下 |
| 恢复正确配置 | **0** | 0 → 放行 |

**注意**：这一层修的是 MIO 的电气属性，与引脚被复用成什么功能无关，
所以它不会和后续在 bank 1 上启用的其它外设冲突。

#### 复盘：为什么绕了这么久

1. **环回自检把范围缩小对了**（证明控制器完好），但它**无法覆盖"输入阈值"这一层** ——
   内部环回完全绕过了焊盘。
2. 两次想直接读焊盘电平的尝试都被各自的对照组挡下（见上一节），
   所以始终没有拿到"焊盘到底是高还是低"的证据。
3. **"TX 正常"被当成了"配置没问题"的证据**，但输出和输入对 IO 标准的敏感度不同 ——
   这是本次最大的思维漏洞。
4. 最后是靠**翻 vendor 自己的参考工程**才定位到的：同一块板、同一个引脚，
   vendor 写 `LVCMOS 1.8V`，我们的 XSA 写 `LVCMOS 3.3V`。
   **教训：把 vendor 的参考工程当作"这块板的期望配置基准"，比只看自己导出的 XSA 可靠得多。**

### 9. 双核（AM3-1/2/3，已在板上跑起来）

**已完成**：每 CPU 数据、放 CPU1 起来、CPU1 建立自己的栈/VBAR/MMU/缓存并自报上线。
**未完成**：per-CPU 中断（PPI 银行化）、spinlock 与 SGI IPI（AM3-4/5）。

#### 与 x86 的对应关系

| x86_64 | Cortex-A9 / Zynq |
|---|---|
| `INIT-SIPI-SIPI` | 写 `0xFFFFFFF0` + **`SEV`**（UG585 §6.1.10）|
| AP 从实模式低地址起步 | CPU1 从**任意物理地址**起步,**但必须是 ARM-32 指令** |
| `swapgs` + `KERNEL_GS_BASE` | **`TPIDRPRW`**（每核 banked,特权态专用）|
| 每核 GDT/IDT | 每核 `VBAR` / `TTBR0` / `ACTLR` |
| APIC per-CPU 寄存器 | GIC 的 PPI 在 distributor 里**按核银行化** |

#### 上板前先确认了 CPU1 的实际状态

我们用 JTAG 直载、**不跑 BootROM**,所以"SEV 协议能不能直接用"是个必须先验证的前提。
实测（`tmp-test/jtag/probe_cpu1.tcl`）：

```
A9_CPU_RST_CTRL = 0x00000000      CPU1 没被按在复位里,时钟在跑
0xFFFFFFF0      = 0xFFFFFF2C      BootROM 预置的"安全网"地址还在
CPU1: PC=0xffffff34  LR=0xffffff2c  -> 正停在 BootROM 的 WFE 等待循环
CPU0: PC=0x00103818                 -> 跑着我们的内核
```

**结论:协议可以直接用。**

#### 计划的一处简化：不需要 OCM 跳板

计划原文写的是"OCM 跳板 + `sev`",那是照搬 x86"AP 需要一个低地址可达的实模式入口"的思路。
ARM 不需要:`0xFFFFFFF0` 可以放**任意 32 位地址**,而内核链接在物理 `0x00100000`、
CPU1 起来时 MMU 关着 —— 物理地址本来就直接可达。所以 `cpu1_entry` 直接在 `start.S` 里,
跳板纯属多余。

#### ★ 抓到一个真正的 SMP 一致性 bug ★

CPU1 起来后各项检查都过,但**它表项里的 `mpidr` 是 0**（应为 `0x80000001`），
而 CPU1 自己写心跳时那个值是**对的**。对照哪些字段活下来很说明问题：

| 字段 | 最终值 | 谁写的 |
|---|---|---|
| `cpu_id` / `last_intid` / `stack_top` | ✓ 正确 | **CPU0**（释放 CPU1 之前）|
| `online` / `loops` | ✓ 正确 | **CPU1**（缓存使能**之后**）|
| `mpidr` | ✗ 变回 0 | **CPU1**（缓存使能**之前**）|

**机制**：CPU0 的 `percpu_table_reset()` 把整行读进自己的 L1 并置脏。CPU1 起来时
**缓存是关的** —— 它的写入直通 DDR，**不会让 CPU0 的缓存副本失效**。随后 CPU0
那条脏行被换出，把 CPU1 早先写的 `mpidr` 覆盖回 0；而 `online`/`loops` 是 CPU1
**开了缓存之后**写的，走 SCU 一致性路径，所以没事。

**这就是典型的"能跑但会随机坏"** —— 不修的话以后会以内存随机错乱的形式冒出来，
而那时几乎不可能联想到启动阶段这一次写入。

**修法（两处，缺一不可）**：
1. `percpu_init_self()` **只写 CP15（TPIDRPRW），不写任何共享内存** ——
   新增 `percpu_publish_self()` 负责写身份字段，**只能在缓存使能之后调用**；
2. `smp_release_cpu1()` 在 SEV 之前对 percpu 表做
   `cache_clean_invalidate_range()`，把 CPU0 的脏行清出去。

修完 `mpidr = 0x80000001` 稳定留住。

#### 验收

| 判据 | A（放 CPU1）| B（不放，破坏性 A/B）|
|---|---|---|
| `smp_cpu1_online` | 1 PASS | 0 **FAIL** |
| `smp_cpu1_stage` | 6 PASS | 0 **FAIL** |
| `smp_cpu1_mpidr` | 0x80000001 PASS | 0 **FAIL** |
| `smp_cpu1_loops` | 1 PASS | 0 **FAIL** |
| `smp_distinct_stacks` | 1 PASS | 0 **FAIL** |
| 总计 | **22 passed, 0 failed** | 17 passed, **5 failed** |

**CPU1 确实在独立满速运行**：JTAG 直接读 percpu 表，`loops` 在 700ms 内
从 1698044652 涨到 1906881904（约 **7300 万次/秒**，符合 666MHz 上紧凑循环的量级）。
两核栈分别是 `0x0013D000`（`__stack_top`）与 `0x0014A000`（`__stack1_top`），
相差 `0xD000` 正好是一整块。

**A/B 顺带抓出一个假检查**：`smp_cpu1_id` 查的是 `cpu_id`，而那是 **CPU0 在放 CPU1
起来之前就填好的** —— B 侧它照样 PASS，即永远不会失败、等于没判。已换成
`smp_cpu1_mpidr`（只有 CPU1 自己能写），B 侧确认失败。

#### 上板前的静态核对（Thumb 会直接跑飞）

UG585 明确"CPU1 的首次跳转只支持 ARM-32 指令集"。所以链接后必须核对：

```
001000b0 <cpu1_entry>:           <- 4 字节编码 = ARM,不是 Thumb
  1000b0: e10f0000  mrs  r0, CPSR
  1000c0: e59f0024  ldr  r0, [pc, #36]   -> 0x1000ec
字面量池: 0x1000e0 = 0x0013D000 (__stack_top,  _start 用)
          0x1000ec = 0x0014A000 (__stack1_top, cpu1_entry 用)
```

两个入口各取各的栈符号 —— 取错就变成两核共用一块栈，而那种错不会立刻崩。

#### AM3-4/5：每核中断、SGI 做 IPI、自旋锁互斥性

**每核中断**：`GICC_CTLR` / `GICC_PMR` 以及 **SGI/PPI 段的使能位在 distributor 里
都是按核银行化的** —— CPU0 在 `gic_init()` 里配过，对 CPU1 毫无作用。
漏掉的症状很隐蔽：distributor 那边看上去什么都配好了，而 CPU1 的中断永远不来。
所以 `gic_cpu_init()` + `smp_enable_ipi_this_cpu()` + `gic_enable_irq(29)` 每个核各做一遍。

**自旋锁互斥性**（`smp_lock_counter = 40000 == 40000`、`violations = 0`）：
两核各在同一个锁下累加 20000 次，**判据不只是"计数对不对"** ——
计数只反映**丢了更新**；而"进临界区时 owner 不是空闲"能直接抓到
**两核同时进临界区**，那才是锁坏掉的定义。（计数恰好没丢但互斥已破是可能的。）

**★ SGI 不排队 —— 这是个必须记住的硬件语义 ★**

第一版一次连发 16 个 SGI，目标核**只收到 5 个**，判据 `>= 16` 直接报 FAIL。
查下来这不是故障：**GIC 的 SGI 是边沿触发且不排队的** ——
同一个 INTID 在目标核还没应答时再发，那次会被**直接丢弃**，而不是排成第二次中断。

所以正确的验证方式是**发一个、等它被处理掉、再发下一个**，这样 `sent == seen`
才是一条成立的判据。改成这样后 `8 == 8` 稳定通过。
（这一条值得记：把"我没收到全部"当成故障去查，方向会完全跑偏。）

**顺带修掉一个真 bug：`tick_handler` 的全局状态被两核竞争。**

这个中断处理函数是**两核共用**的，而它里面的 `g_tick_seen` / `g_tick_last_gt` /
`g_tick_max_gap` 是 **CPU0 的诊断状态**。CPU1 的 tick 走进来会把这些覆盖掉 ——
症状是自检里 `irq_ticks_eq_irq` 报 FAIL（ticks 显示的是 CPU1 的数，
拿去和 CPU0 的 `irq_count` 比自然对不上），看起来像丢了中断。
现在处理函数在清完本核标志后，非 CPU0 直接返回；本核自己的 tick 计数在 percpu 里，不受影响。

**当前验收：26 passed, 0 failed（连续 10 次冷启动）。**

#### `irq_spurious` 那次 1285：**出现过一次，之后 13 次未复现 —— 未修复**

完整账目（这是唯一一次出现）：

| 运行 | `irq_spurious` |
|---|---|
| AM3-4/5 首次验证 | **0**（那次红的是 `irq_ticks_eq_irq`）|
| 修完 tick race + IPI 之后 | **1285** ← **唯一一次** |
| JTAG 连续采样（同一次启动，11 秒 / 90 点） | 1285，**冻结不涨**，同期 `irq` 仍按 1kHz 稳定涨 |
| 后续验证 x2 | 0 |
| **连续 10 次冷启动** | **全部 0** |

**这不是"修好了"，是"没能复现"。** 单次通过区分不了"已修"与"很罕见"，
所以这一项**保持原判据、保持在场**，只把观测记录写清楚。

已排除的：

- **不是持续竞态** —— 冻结采样显示它涨到 1285 就停住，之后 800ms x 5 次采样一动不动；
- **不是启动早期必然发生** —— 11 秒的时间序列（自检完整落在窗口内）是一条平直的 0；
- **不是 CPU1 造成的** —— `c_irq_handler` 里的 CPU0 守卫已生效，`unhandled = 0`、
  `last_intid = 29`（定时器），CPU1 的计数走自己的 percpu 条目。

**判据（留给复现时用）**：`c_irq_handler` 里 `irq_count++` 在 spurious 判断**之前**，
所以若那 1285 是真的虚假中断，CPU0 上必然满足

```
irq_count = ticks + spurious + unhandled
```

不成立就说明计数是**被别的写入破坏**的（内存越界），而不是中断累加出来的 ——
两者排查方向完全相反。工具：`tmp-test/spurious_probe.py`。
（⚠ 该工具的读数有固有偏差：顺序 `mrd` 之间相隔几十毫秒，实测差 -32，
不是 bug；判据只在 `spurious` 非 0 时有意义。）

**现状判断**：符合 GIC 的 ack 竞态特征（nIRQ 已拉高但读 `GICC_IAR` 前中断被撤销，
返回 1023），而我们的处理正是标准做法（1023 时**不回 EOI**）。
但这只是**假设，未证明** —— 所以不写成"已解决"。

**下一步（若要继续追）**：用二分法统计出现率 —— 先关掉 SGI 段跑 N 次，
再关掉整个 `smp_stress_run` 跑 N 次。没有出现率就无法判断任何改动是否有效。
考虑到 13 次未复现，这一项按"已知的罕见现象 + 判据在场"处理，不阻塞 M4。

### 10. M4-1：krlibc 子集（已完成）

**搬运范围是数出来的，不是拍脑袋定的**：扫描 `driver/fs` + `include/fs` +
`driver/device.cpp`（排除 `ffunicode.cpp` 那张 2MB 的生成表，它不调 libc），
得到 **23 个函数、699 个调用点**。其中 `malloc`/`calloc`/`realloc`/`free`
共 316 处属于**堆**（M4-3），不算本阶段。

本阶段只做**不依赖堆、不依赖调度器、不看硬件**的纯函数（15 个）：
`memcpy` `memmove` `memset` `memcmp` `strlen` `strcmp` `strncmp` `strcpy`
`strncpy` `strcat` `strchr` `strrchr` `strtok` `isdigit` `atoi`，外加 `errno` 存储。

已经复用过一轮的结论：**没被调用、没法验证的代码不搬。**

★ **2026-09-18 补记（M4A-1.1a）：那一组堆函数补上了。**
`malloc` / `calloc` / `realloc` / `free` 现在有了 —— 但**分配器一个字节都没重写**：
`heap.c`（M4-3）早就做完了，缺的只是"名字"这一层接线，实现在
`src/kmalloc.c`，语义与**刻意没提供**的两个函数（`aligned_alloc` / `usable_size`）
见 `arch/kmalloc.h`。

值得一提的三点：

1. **它不是"顺手改个名字"** —— 启动时的 `Heap smoke` 现在走 `malloc`/`free`，
   所以这条接线每次上板都被跑一遍；
2. **`free()` 是 void，而 `heap_free()` 会告诉你失败**。接成 void 就把
   "双重释放/野指针"这个信号扔了，于是它变成"堆慢慢坏掉、很久以后在无关处崩"。
   所以留了 `kmalloc_bad_free_count()` —— 这是那类错误**唯一**看得见的地方；
3. **`Heap bind A/B`**：把绑定撤掉之后 `malloc` 必须返回 NULL。
   "冒烟测试通过了"本身说明不了"通过的是这条接线"，撤掉再试才算数。
   它不解锁任何破坏性路径，所以是本项目第一组放在**报告之前**的 A/B。

**还没做的**：上游文件写的是 `#include <mm/alloc/alloc.h>` / `<mm/heap.h>`，
那层 include 路径映射没做 —— 今天只保证**符号**存在（`nm` 能在
`out/kernel-arm.elf` 里看到 `malloc`/`free`/`calloc`/`realloc`）。

#### errno：用机械手段保证两个架构一致，而不是靠自觉

`arch/arm32/include/arch/errno.h` 是 `include/errno.h`（旧 XJ380，125 条定义）的副本。

**"复制两份靠自觉同步"是不可接受的** —— 漂移的表现是"某个驱动返回了错误的
errno"，极难定位。所以 `tests/test_arm32_krlibc.py` 里有一个**逐值比对**的测试：
它解析两个文件、比对每一条 `#define` 的名字与数值（含 `EWOULDBLOCK EAGAIN`
这类别名指向的名字），不一致就报错。

**⚠ 一处路径偏离（已记录，B5 时消除）**：旧 XJ380 里是 `#include "errno.h"`，
这里是 `<arch/errno.h>`。原因是实测踩到的 —— 放在 include 根下会让**宿主单测**的
`-I` 顶掉系统 `<errno.h>`，于是 `test_arm32_shell` / `test_arm32_plat_device` 全挂。
B5 把两边合到同一个头文件上时这个偏离自然消失。

#### 测试上的一个 Windows 坑

宿主测试直接链接会报 `duplicate symbol: atoi`（UCRT 已经导出了它）。
用 `-Wl` 的"允许多重定义"能绕过，但那个开关各平台拼写不同
（GNU ld 是 `--allow-multiple-definition`，lld-link 是 `/force:multiple`），
测试会变成依赖工具链。

用的办法是**给被测符号加前缀**（`-Datoi=krtest_atoi` …）：编译器无关，
而且附带一个好处 —— 它**彻底阻止编译器用内建替掉我们的实现**，
而这正是这个测试想避免的（要测的是我们的代码，不是 libc 的）。
真正的符号名由 ARM 侧构建（`-nostdlib`）负责验证。

### 11. M4-2：物理页分配器（已完成）

自己写的位图分配器，页大小 4KB。**2 bit/页**而不是 1 bit：

```
00 = FREE      01 = USED      10 = RESERVED
```

一位只能表达两态，于是**双重释放检测不出来**（释放一个空闲页会静默变成 no-op）、
**分不清"已分配"与"保留"**（内核镜像/页表/栈必须永远不可释放）。
这两类错误的共同点是**不会当场出错**，而是在很久以后以完全无关的症状崩掉 ——
多花一位买断它们很划算。

分层沿用 `mmu.c`/`mmu_hw.c`：`src/palloc.c` **不含 MMIO/CP15，bitmap 由调用方提供**，
所以宿主能直接编译并穷尽错误路径。「物理内存从哪来」是调用方的事：
kmain 用 `_kernel_end` 向上对齐到 DDR 末尾，**不需要逐个 reserve** ——
镜像之内的东西根本不在池范围内。

#### ★ 顺带炸出一个从 M1 埋到现在的雷：内核从来没使能 FPU ★

加完 palloc 后内核当场 Undefined Instruction，PC 指向：

```
103f00: eddf0bfa  vldr  d16, [pc, #1000]     <- VFP 指令
103f18: edc40b04  vstr  d16, [r4, #16]
```

GCC 用 **VFP 寄存器做 8 字节块拷贝**（编译选项是 `-mfpu=vfpv3 -mfloat-abi=hard`），
而 **CPACR / FPEXC 从来没被设置过** —— 于是 VFP 指令以 Undefined Instruction 收场。
报错现场指向那条 `vldr`，看起来像"编译器生成错了代码"，**实际是内核少做了一步初始化**。

**为什么到 M4-2 才炸**：只要函数大到让 GCC 决定用 VFP 做块拷贝就会中招，
而 `palloc_selftest` 里那个 64KB 静态数组的初始化正好跨过了阈值。
**之前只是没踩到，不是没问题。**

修法（`boot/start.S` 的 `enable_fpu`）：两个寄存器都要动，只做一个仍然 undefined ——

```
CPACR(c1,c0,2) bits[23:20] = 0xF   允许访问 CP10/CP11
FPEXC.EN(bit30)          = 1       VFP 本身使能
```

⚠ **两个都是每核寄存器**，所以 CPU0 与 CPU1 各调一次；都排在**任何 C 代码之前**
（编译器可能在任何函数里生成 VFP 指令）。

#### 验收

| 判据 | 结果 |
|---|---|
| `palloc_selftest` | 0（41 项检查，含双重释放/未对齐/越界/保留页/OOM/超上限）|
| `palloc_smoke` | 1（真实页上 alloc → 写图案 → 读回 → free）|
| `palloc_pages_ok` | 1（261782 页 / 1022 MB，从 `0x0016A000` 起）|
| 全量自检 | **29 passed, 0 failed** |
| 宿主单测 | 1 passed（自检 + 独立复核状态未被破坏）|

**`palloc_smoke` 是刻意的额外一层**：自检跑的是合成实例（基址 `0x10000000`，
宿主上也能跑），它证明**逻辑**对，但证明不了"这段物理内存真的能用"。
冒烟测试才是对真实 RAM 的读写 —— 与缓存那节的"写回读"同一个道理。

### 11. M4-3：内核堆（已完成）

自己写的空闲链表堆，一条**按地址顺序的双向链表**贯穿整个堆区，空闲块也在链表里（靠
`state` 区分）—— 于是合并就是"看邻居"这么简单，**不需要额外维护一张空闲表**，
而那张表迟早会与链表不同步。

每个块头带**魔数**，于是"越界写"与"用野指针 free"能被**当场**抓住：

```
magic / state / size / next / prev / pad
```

#### 自检当场抓到两个 bug（这是它存在的理由）

**① `largest_free` 从来没被更新过。** 它一直是 `heap_init` 时的初值 ——
自检里"统计必须与实际链表一致"那一条立刻报错。这个字段没有任何调用方
读它做决定，所以**它自己不会以任何形式表现出来**，属于典型的静默失真。

**② 更隐蔽的一个：宿主上 `sizeof(heap_block_t)` 是 40，而我把头硬编码成 24。**

宿主机是 64 位（指针 8 字节），于是负载指针压到结构体尾巴上。**板子上（32 位）
恰好是 24 所以能跑 —— 而宿主测试测的是一套完全不同的布局，等于白测。**
现在头大小由 `sizeof` 推导并对齐到 8，加静态断言钉住"对齐后的头装得下结构体"。

⚠ 两套布局（host 40 / target 24）**不同但没有 ABI 含义** —— 堆的内部布局
不需要跨架构一致。需要一致的是 `device_t`/`errno` 那类对外契约。

#### 一个设计约束：堆不按需增长

自检第 45 项暴露了一个**设计问题**而不是笔误：`heap_extend` 要求增长区与堆区
**紧邻**，而 `palloc_alloc_pages()` 返回的页**不保证相邻** ——
所以"堆不够就向 palloc 要一页接上"这条路**根本走不通**。

因此实际用法是**启动时一次性要一大块连续内存**（kmain 里 8192 页 = 32MB）。
增长回调保留为可选路径，并在头文件里写明相邻约束；宿主测试用它验证增长逻辑。
将来若真需要动态扩张，应当改成**分段堆**（每段各自一条链表）—— 那是另一轮的工作。

#### 验收

| 判据 | 结果 |
|---|---|
| `heap_selftest` | 0（58 项：双重释放/魔数破坏/非法指针/对齐/碎片回收/增长/统计一致）|
| `heap_smoke` | 1（真实内存上两块 alloc→写→读回→free→check）|
| `heap_size_ok` | 1（32MB）|
| 全量自检 | **32 passed, 0 failed** |
| 宿主单测 | 1 passed（含**真实越界写**是否被检出）|

宿主测试里有一项值得单说：它不满足于"手工改魔数能被发现"，而是**真的越界写**
（对 32 字节的块写 64 字节，踩到下一块的块头），验证检查确实会报错。

### 11. M4-4：4KB 小页与细粒度映射（已完成，已接入内核）

**核心能力不是"能映射 4KB 页"，而是把一段 1MB 恒等映射就地拆成 L2 表、
且保持每一页的映射与属性不变。** guard page（M4-5）依赖的正是它 ——
段映射下一个 1MB 里的 256 页共享属性，做不到"只留一页不映射"。

#### ★ 第一版失败的原因已写成接口约束 ★

段描述符与小页描述符的**属性位布局完全不同**：

| 字段 | 段(L1) | 小页(L2) |
|---|---|---|
| AP[1:0] | bit 11:10 | bit 5:4 |
| AP[2] | bit 15 | bit 9 |
| TEX | bit 14:12 | bit 8:6 |
| S(共享) | bit 16 | bit 10 |
| XN | bit 4 | **bit 0** |

`MMU_ATTR_NORMAL_WB = 0x15DE6` 里 bit 16 是段的 S 位，**而在小页里 bit 16 属于
物理地址字段** —— 直接搬运会让拆段之后每一页的物理地址凭空多出 `0x10000`。

**所以接口收的是参数（`vmap_attr_t`），不是打包好的属性值**；拆段时从段描述符
**解码回参数**，再用 `mmu_l2_small_page_attr()` 重建。`mmu.h` 同时提供两个
构造函数正是为了这件事。

宿主测试里专门钉了这一条：断言"由小页构造函数重建的值"**不等于**"段属性值的低 12 位"。

#### 另一条已验证的结论

**指针与物理地址在结构体里分成两个字段**（`l2_pool` / `l2_pool_pa`）。
描述符里存 32 位物理地址，而宿主指针是 64 位 —— 截断再转回会得到野地址
（实测：宿主上直接访问违例）。目标板上两者数值相同，但代码不能依赖这一点。

#### 映射语义：`vmap_map` 是严格的

对**已存在的 4KB 映射**一律返回 `ALREADY`，**不静默覆盖** ——
覆盖会让先注册的那一方悄悄失效，而那种问题要等它自己访问时才炸。
换映射的流程是 `unmap -> map`。

对**段**映射则不需要先 unmap：`vmap_map` 会自动拆段并覆盖那一页。

#### 验收

| 判据 | 结果 |
|---|---|
| `vmap_selftest` | 0（45 项：拆段逐页核对、越界保护、重复映射拒绝、池耗尽、幂等、XN 位真的变了）|
| 独立复核（宿主） | 拆段后 256 页逐页核对物理地址与属性；属性断言"重建 ≠ 搬运" |
| `vmap_board_split` / `vmap_board_live`（板上） | 1 / 1 —— 真实页表上拆段、改映射、TLB 失效后**通过 VA 读回**到改写后的内容 |
| 全量单测 | 9 failed / 70 passed（9 项是预先存在的，与本移植无关）|

#### 接进内核时一次定位到两个 **调用方**的 bug

> 这一段值得留在 README 里，因为两个 bug **都不在 `vmap` 里**。

**① `l2_pool_pa` 传错了值。** `vmap_init(..., g_l2_pool, (u32)region, ...)` ——
把**要映射的数据区**当成了 L2 表池的物理地址，于是 L1 描述符指向数据区，
硬件会把那块内存里的数据当描述符读。铁证是 `l1_after = 0x001761E1`
的后半段恰好等于 `region`。

> 讽刺的是，`l2_pool` / `l2_pool_pa` 这个分离**正是上一轮为了防这类错误才做的**，
> 而调用处填错了值。**分离字段能防"截断成野地址"，防不了"填错变量"** ——
> 后者只能靠把值打出来看。

**② 测试区间没有 1MB 对齐。** `palloc_alloc_pages(256)` 只保证 4KB 对齐，
拿到 `0x00176000`，它**跨了两个 1MB 段**而只拆了第一个。
⚠ 这不是 `vmap` 的 bug：`vmap_map` 自己会按需拆段；错的是测试里
"拆一次就覆盖整段"这个假设。修法是**要 512 页再向上对齐到 1MB**。

**这一轮的教训**：把三位状态的错误码二值化成 `split_ok` 之后，
只知道"失败了"，白白多花一整轮上板时间。改成打印
`init_err` / `split_err` / `l1_before` / `l1_after` 后，两个 bug 一次全部暴露。
**验证代码本身的信息量就是产出的一部分。**

### 11. M4-5：内核栈池 + guard page（已完成，第三级证据成立）

**产出不是一个"能分配栈"的函数，而是一个越界就当场被抓住的栈。**

每个栈的**紧下方**留一页不可访问（guard page）。栈是向下长的，所以 guard 在
低地址一侧；写穿栈底立刻产生 Data Abort，`DFAR` 正好落在 guard 页里。

```
   地址递增 ->
   +----------------+ <- slot_base(i) == guard(i)   ★ 不可访问 ★
   | guard 页(4KB) |
   +----------------+ <- base(i)，栈区底
   |  栈页 0..N-1   |
   +----------------+ <- top(i)，初始 SP（8 字节对齐）
   | guard(i+1)     | <- 下一个槽的 guard，紧邻
```

两条不变量（自检逐条断言）：`guard(i) == base(i) - 4KB`、
`guard(i+1) == top(i)` —— 槽与槽无缝相邻，不浪费也不重叠。

#### 规模照搬源 OS，不是拍脑袋

| 项 | 值 | 来源 |
|---|---|---|
| 每栈大小 | **1MB** | `include/proto.hpp` 的 `CONFIG_KERNEL_TASK_STACK_SIZE = 1048576` |
| 每任务栈数 | **2** | `kernel/task/pcb.cpp` 成对分配 `kernel_stack` + `syscall_stack` |
| 池 | 32 槽（= 16 个任务），每槽 257 页 | 板级 `kmain` 的 `KSTACK_SLOTS` |

任务真要用两个栈这件事在 M4-6/M4-7 落地；这里先把池按这个口径开出来，
免得池小到要用时才返工。

#### guard 用"不映射"实现，不用"没权限"实现

两种做法都能拦住访问，但报出来的状态码不同，适用范围也不同：

| 做法 | 机制 | `FS[4:0]` |
|---|---|---|
| L2 项清零 | 转换故障 | `0x07` translation fault, **level 2** |
| 映射但 `AP=0b000` | 权限故障（需 DACR = client）| `0x0F` permission fault, level 2 |

默认用**不映射**：页表里根本没有这一项，连投机访问都被拦住；也不依赖
DACR 的模式或 XN 这类"只管取指"的位。

`AP=0b000` 那条路也实现了，但**不是为了"另一种 guard"**，而是为了补一笔旧账：
M2-4 把 DACR 从全 manager 切成全 client 时，理由是"所有区域的 AP 都是 0b011，
所以行为应当完全不变" —— 也就是说 **AP 到底有没有被硬件执行，当时并没有被验证过**。

#### ★ 为什么 TLB 维护由模块自己拥有 ★

这是本模块**刻意没有**沿用"纯模块不管 CP15、由调用方收尾"那条惯例的地方。

改完页表忘了失效 TLB，后果不是"慢一点"，而是 **guard 静默失效**：
拆段之前那一整段是**段映射**，TLB 里留着段表项，于是 guard 页照样能访问，
而页表里看上去完全正确 —— 这种失败模式**没有任何下游错误可查**。

所以本模块自己拥有这件事，但把它做成一个**函数指针**（`kstack_flush_fn`），
好处是两头都占：模块内部绝不会漏刷；纯逻辑部分仍能在宿主机上编译，
而且宿主单测可以**断言这个钩子被调用过、且区间覆盖了 guard 页** ——
"忘记刷 TLB"于是从"只能靠人记得"变成了**可自动检查**的性质。

#### 验收：三级证据齐全

| 级别 | 手段 | 结果 |
|---|---|---|
| 一级（软件自报）| `kstack_selftest` / 6 项 CHECK | 0 / 全 PASS |
| 二级（硬件读回）| `vmap_lookup(guard) == 0`、栈区真的能写读 | PASS |
| **三级（破坏性 A/B）** | `tmp-test/guard_trip.py --load` | **成立** |

三级证据那一步的做法（**一次加载里跑完**）：

```
B  选择器 8 —— 对照组：把 guard 页临时映射成普通可读写页，再跑同一段溢出
      结果：无异常，64 个字静默写进 guard 页，内核继续推进（"静默损坏"被看见）

C  选择器 6 —— guard 生效：同一段代码、同一批地址
      结果：Data Abort，DFAR = 0x02497FFC = base-4（guard 页最后一个字）
            FS[4:0] = 0x07（translation fault, level 2），WnR = 1（写），domain = 15
```

**只有"同一地址、同一条指令，只差这一页映射与否"这一组对照，才能说明 guard
是承重的。** 读回页表做不到这一点 —— 那个条件在 TLB 里还留着段表项时同样成立。

板级自检 **41 passed / 0 failed**（比 M4-4 多了 6 项）。

#### ★ 顺带炸出一个真 bug：`DFSR` 不能用 `fsr & 0x1F` ★

guard page 第一次上板时报出：

```
DFSR = 0x000008F7   (reserved / unknown)      <- 旧译码
```

查权威位域（ARMv7-A ARM，`TTBCR.EAE == 0` 即短描述符格式）后发现：

| 位 | DFSR | IFSR |
|---|---|---|
| [3:0] | `FS[3:0]` | `FS[3:0]` |
| [7:4] | **Domain** | RES0 |
| [9] | LPAE | LPAE |
| [10] | `FS[4]` | `FS[4]` |
| [11] | WnR | RES0 |

**Domain 正好压在状态位上面**（bits[7:4]），于是 `dfsr & 0x1F` 会把 `Domain[0]`
当成 `FS[4]`。本内核 domain = 15 → 每个状态码凭空 +0x10：

```
真实 0x07 (translation fault, level 2)  ->  & 0x1F 得 0x17  ->  "保留/未知"
真实 0x05 (translation fault, level 1)  ->  & 0x1F 得 0x15  ->  "保留/未知"
```

**为什么一直没暴露**：以前只触发过 L1 **fault 项**的故障（如 `0x50000000`
没有映射），fault 项的 Domain 位是 0，`& 0x1F` 碰巧等于真值；而取指路径
（IFSR）**根本没有 Domain 字段**。guard page 的 L1 项是**页表描述符**、
domain = 15，这才露出来。

顺带把状态表按权威表重写了一遍 —— 原来 `0x0A` / `0x0B` / `0x0E` 三条都是错的
（`0x0B` 才是 domain fault level 2，`0x0E` 是 TT walk 上的外部异常，
permission fault level 2 是 `0x0F`），并给 Data Abort 输出补上 **domain / WnR**：
那次就是靠这两个字段定位的，只打状态文本的话，"域号不对"和"状态码不对"
在串口上长得一模一样。

改动动的是**所有**故障诊断路径共用的译码，所以按"改了就要重验"的规矩，
M1 阶段验证过的三条路径（选择器 1/3/5）用 `tmp-test/fsr_decode_check.py` 重跑：
`DFSR=0x08`、`IFSR=0x08`、`IFSR=0x0D` 分别译成
"synchronous external abort, not on translation table walk"、
同上、以及 "permission fault, level 1" —— 三条全部译对，FAR 与注入地址一致。

#### 宿主单测抓到的两个 bug（并做了 A/B 确认自检确实能抓到）

自检在一个**不与 1MB 段对齐**的合成池上跑（起点 `0x400FF000`），
于是**同时**造出两种 guard 位置：

- 槽 0：`base = 0x40100000` 段对齐 → guard = `0x400FF000` 落在**上一个段**，
  而那一段没有被任何栈区映射碰过 → `vmap_unmap()` 返回 `IS_SECTION`，必须先拆段；
- 槽 1：guard = `0x40200000` 段对齐 → 先被自己的栈区映射拆段覆盖、再取消映射。

两条路径都断言，并用 `stat_guard_split` 证明第一条**真的被走到过**
（否则那项检查是空的）。两个 bug 都是先被它抓到的：

| bug | 后果 | 宿主 A/B 确认 |
|---|---|---|
| alloc 先映射栈页、最后处理 guard | 若 guard 落在上一个段，映射失败时**回滚也清不掉它**（要靠的正是同一张拿不到的表）→ 留下一个半配置的槽 | 改回去 → 自检在第 **82** 项失败 |
| AP=none 的 guard "看到段就拆、拆完就返回" | 拆段填出来的是恒等映射（**可访问**）→ guard 静默变成普通可读写页，而且返回 `VMAP_OK` | 改回去 → 自检在第 **91** 项失败 |

#### 一条 `vmap` 使用契约（M4A-1 的 fs / DMA 缓冲也会需要）

`vmap_map` 对**已存在的 4KB 映射**一律拒绝（刻意不静默覆盖），所以
"把一个页从任何状态变成我要的映射"必须分三步，顺序不能换：

```
1. split  段映射下先拆成小页（幂等）
2. unmap  ★ 最容易漏的一步 ★ 拆段会把整段 256 页都填成恒等映射，
          于是 vmap_map 从第二页起全部返回 ALREADY
3. map    建立调用方要的映射
```

第一版就是漏了第 2 步，症状很典型：**第一页成功、从第二页起全部 `ALREADY`**
（也就是"分配 1MB 的栈只在第一页成功"）。

由此还推出一条对 guard 至关重要的不变量：**一个 1MB 段只会被拆一次，
而拆一定发生在任何一页被单独控制之前或同时** —— 所以不存在
"guard 先被取消映射、之后又被一次拆段重新映射回来"的时序。
换句话说 **guard 不会静默消失**。

#### 已知限制（写明，不当作已完成）

- `kstack_tlb_flush_range` 只失效**本核** TLB。现在栈只由 CPU0 用，所以够用；
  等任务真跑在 CPU1 上时，CPU1 那边可能还留着启动阶段的段表项，
  **guard 对它是失效的**。留给 M4-7：TLBIMVAA 广播，或用 IPI 让对端自己刷。
- 选择器 7（`AP=0b000` 的硬件 trip）需要建**第二个池实例**（`guard_kind`
  是池级配置），板级尚未建。目前该路径由 `kstack_selftest` 覆盖；
  触发选择器 7 时会**如实说明这一点**，而不是悄悄退化成"不映射"那一种 ——
  那会让 6 和 7 看起来都验证过了。
- `kstack_free` **不**把页还给 palloc：池的意义是"内存预先提交、反复复用"，
  逐次向 palloc 要页既做不到"归还后再拿到同一段"，也会让 guard 的位置
  随分配次序漂移，出问题时无法复现。池的生命周期 = 系统生命周期。

### 11. M4-6：寄存器帧（已完成，pc 的偏移在本板实测后改正）

**产出不是"定义两个结构体",而是把汇编与 C 之间那条 ABI 变成可核对的东西。**

#### 先看清 x86 那边有什么：**两套寄存器帧，不是一个**

计划 §4.7 的调研结论：

| 帧 | 结构体 | 布局由谁决定 | 用在哪 |
|---|---|---|---|
| 调度帧 | `registers_t`（`include/task/pcb.h:17-44`）| `scheduler.cpp:46-92` 的 push/pop 序 | timer → `change_proccess` |
| 异常帧 | `struct X64_REGS`（`include/cpu/longm.h:9-35`）| `kernel/intr/handler.S:5-28` | 异常 + syscall |

两者**同为 192 字节**，但 `rbx..rdi` 段与 `r8..r15` 段位置**互换**，指针不可互换。
ARM 侧同样分两张：`arm_exc_frame_t`（16 字）+ `arm_task_ctx_t`（17 字）。

#### ★ 本板实测的 LR 偏移表 ★

ARM 的异常入口把返回地址放在 `LR_<mode>`，而**它离"出错的那条指令"有多远，
每个异常类型都不同**。x86 上这件事由硬件压栈的 `rip` 一并解决，ARM 上必须自己算。

`tmp-test/exc_frame_probe.py` 的方法是：注入点是 ELF 里已知编码的指令，
地址用 objdump 读出来，再与处理函数打印的值相减 —— **不比"看起来差不多"，
比的是地址相等**。实测结果：

| 异常 | 入口 LR | 旧代码打印的 pc | 真指令 | 差 |
|---|---|---|---|---|
| SVC | SVC 指令 + 4 | `0x00101908` | `0x00101904`（`svc`）| **+4** |
| Undefined | 未定义指令 + 4 | `0x001018C8` | `0x001018C8`（UDF）| 0 ✓ |
| Prefetch Abort | 取指失败地址 + 4 | `0xE0001000` = IFAR | IFAR 本身 | 0 ✓ |
| Data Abort | 出错指令 + **8** | `0x00105C38` | `0x00105C34`（`str r2,[r3],#-4`）| **+4** |
| IRQ | 被中断指令 + 8 | 下一条 | 被中断的那条 | **+4** |

#### 根因：`ret` 与 `pc` 被当成了同一个值

旧代码只有一个 `pc` 字段，同时承担两件事，而这两件事要的值**不一样**：

```
返回   要"首选返回地址"    SVC = LR        IRQ = LR-4    ← 旧代码这两处是对的
诊断   要"出错/被中断指令"  SVC = LR-4      IRQ = LR-8    ← 旧代码这两处偏 4
```

于是一个**看起来完全正常**的值其实指向了隔壁那条指令。这不是"少了个字段"，
是两件不同的事被塞进了一个槽。

**修法**：帧里分成 `ret` 与 `pc` 两个字段，`vectors.S` 里每条异常各自算两个值。
修完实测：

```
SVC          打印 0x001019F4  = `svc` 本身            ✓
Undefined    打印 0x001019B8  = UDF 本身              ✓
Data Abort   打印 0x00105F24  = `str r2,[r3],#-4`      ✓
             (objdump 核对:105f24: e4032004 str r2, [r3], #-4)
```

#### ★ 偏移宏只写一份，汇编与 C 共用 ★

这正是 §4.7.6 记下的那个坑的正面做法：

> x86 的 `kernel/intr/handler.S:29-31` 把 `PROCESSOR_INFO` 的三个偏移
> （`0x4c0` / `0x4e0` / `0x4e8`）硬写在汇编里，**一个 `static_assert` 都没有**，
> 全靠算术偶然对上 —— 改一次字段顺序就静默错位。

所以这里拆成两层：

- `arch/taskctx_asm.h` —— **只含 `#define`**，所以 `boot/vectors.S` 能用 cpp 直接包含；
- `arch/taskctx.h` —— 结构体 + `_Static_assert`，把 C 结构体的 `sizeof`/字段偏移
  与那些宏对上，**同时保护了汇编那一侧**；另有 `arm_svc_immediate()` 等纯函数。

`vectors.S` 里现在**没有任何裸偏移**：建帧是 `EXC_FRAME_ENTER lr_fix, ret_fix`
两个宏参数，帧大小来自 `ARM_EXC_FRAME_BYTES`。

#### 帧布局的**运行时**自检

`_Static_assert` 钉得住 C 侧的宏，钉不住"汇编真的按这些宏存了" ——
`stmia` 的寄存器顺序、`sub sp` 的字节数、`mrs` 存到哪个槽，只有跑起来才知道。

所以加了一个选择器 9：把 r0-r12 设成已知图案，再执行 `svc #0xA5A5`；
`c_svc_handler` 逐个核对帧里的 13 个槽，外加 SPSR 的模式位与 `ret == pc + 4`。

**★ 这个自检依赖 `pc` 的偏移是对的 ★**
它靠"读 `pc` 处那条指令的立即数"来判断这是不是一个布局自检。
旧代码的 `pc` 指向 SVC 的下一条，按它取立即数会取到随机的下一条指令 ——
自检会**静默地什么都不做**。所以报告里的判据是 `== 0` 而不是 `>= 0`：
`-1`（没跑过）必须算失败，否则"汇编写错了"与"自检根本没触发"在报告上长得一样。

#### 验收（板上实测）

| 判据 | 结果 |
|---|---|
| `exc_frame_layout` | **0** —— 13 个寄存器槽 + SPSR 模式 + `ret == pc+4` 全部一致 |
| `exc_frame_probe.py` | SVC / Undefined 的 pc 与真指令**地址相等** |
| `guard_trip.py` | Data Abort 的 pc == 那条 `str`；两组 guard A/B 仍成立（无回归）|
| 全量自检 | **43 passed / 0 failed** |
| 宿主单测 | `tests/test_arm32_taskctx.py`：独立重算偏移、用真实对象量字段偏移、钉住"同一异常下 ret 与 pc 不同" |

#### 刻意**没有**做的事（写明理由）

- **异常帧里暂时不放 `sp`**。现在所有异常都从 SVC 模式进，而入口已经切到了
  异常模式自己的栈上 —— 被中断者的 SP 在 IRQ/ABT/UND 模式里**读不到**。
  硬塞一个"有时有效"的字段比不放更危险。`arm_task_ctx_t` 里有 `sp`，
  因为任务切换是协作式的：切换代码就跑在那个任务自己的栈上。
- **IRQ 仍在 IRQ 模式的栈上建帧**。按计划 §4.5，M4-7 会改成
  `srsdb sp!, #MODE_SVC` 落到**任务的栈**上 —— 那时 SP 由构造决定，
  异常帧才该加 `sp` 槽。顺序上是 M4-7 先做这件事。

### 11. M4-6：PCB / TCB 结构体（已完成）

**产出不是"照抄一份结构体"，而是三份清单 + 一条比 x86 更窄的汇编边界。**

#### 一、字段怎么来的：三份清单（计划 §4.7）

移植的规程是"源 OS 怎么做才是"，所以先把 `include/task/pcb.h`（252 行）
逐字段读了一遍，分成三类。`arch/tcb.h` 里**每个字段后面都写着它在
`pcb.h` 里的行号** —— 那不是装饰，是可以拿源文件逐行核对的账。

| 类别 | 处理 |
|---|---|
| ARCH_INDEP | **逐字段沿用 x86 的名字**（`parent_group` / `thread_queue` / `file_open` / `brk_current` / `eevdf_vruntime` …），类型按 ARM 取 |
| ARCH_DEP | 只有 **`pagedir`** 一个（x86 是 4 级页表根 + CR3，ARM 是 L1 表 + TTBR0）；TCB 里是 `ctx` 与 `vfp` |
| X86_USER_ONLY / 暂缓 | 逐条列在 `tcb.h` 的注释表里，并汇总成 `ARM_TCB_DEFERRED_FIELDS` 常量，便于将来 grep |

`pcb_t` / `tcb_t` 是**指针 typedef**（`pcb.h:46-47`），这条约定照抄。

#### 二、★ 一处刻意的偏离：一个任务只要**一个**内核栈 ★

x86 每个 TCB 有**两个** 1MB 内核栈（`pcb.h:174` 的 `kernel_stack` 与
`pcb.h:182` 的 `syscall_stack`）。原因很具体：

> x86 的 `syscall` 指令**不会切栈**。所以 `handler.S:315` 必须从 TCB 里
> 把 `syscall_stack` 装进 `%rsp` —— 内核得有个已知的栈可切。
> 而中断走 TSS.rsp0 那条路，于是两者天然是两段栈。

**ARM 不是这样**：`SVC` 与 `IRQ` 各有**硬件 banked** 的 SP（`SP_svc` / `SP_irq`），
进异常时硬件自动切，不需要从任何结构体里读栈指针。任务切换时把栈顶写进
`SP_svc` 即可，`SVC` 进来就已经在它上面了。

⇒ 在 ARM 上留 `syscall_stack` 字段 = 每个任务白留 1MB 永不使用的栈。
所以 `arm_thread_control_block` **没有**这个字段。

连带影响：M4-5 的栈池当初按"32 栈 = 16 个任务（每任务两个）"开的，
现在口径是 **32 栈 = 32 个任务**。池大小不用改，说法要改。

#### 三、★★ 汇编的边界是"指向 ctx 的指针"，不是"TCB 加某个偏移" ★★

这一条是本次最值得记的设计结论，因为**第一版走错了路**。

第一版照 x86 的样子，在 `taskctx_asm.h` 里定义 `ARM_TCB_OFF_CTX` 等四个偏移，
让汇编直接按偏移访问 TCB 里的 `ctx` / `kernel_stack` / `vfp`。**做不下去**：

- TCB 有大量**指针字段**（`parent_group` / `argv` / `cwd` …）。宿主的指针是
  8 字节、目标是 4 字节，于是**宿主机上算出来的偏移与目标板不同** ——
  那几个 `_Static_assert` 在宿主上验的是错的布局。
  （本项目在 `heap_block_t` 的 `sizeof` 上已经踩过一次同样的坑。）

更关键的是第二条：**汇编根本不需要知道它**。

| | x86 | ARM |
|---|---|---|
| syscall 时怎么找内核栈 | 从 TCB 读 `syscall_stack` 装进 `%rsp`（`handler.S:315`）| **硬件 banked**，自动切 |
| per-CPU 当前任务 | `%gs:0x4c0` | TPIDRPRW 拿结构体指针，再按偏移取 `current_task` |
| 切换器拿什么 | 就地改写 C 栈上的 `registers_t` | C 侧把 **`&tcb->ctx`** 传进汇编 |

⇒ ARM 侧汇编真正要钉的偏移只剩两组，而且**两组都只含 `u32` 字段**，
所以宿主与目标的布局**一致**，断言在宿主机上验的就是目标板的真实偏移：

- `arm_task_ctx_t` 内部的偏移 —— `arch/taskctx.h` 里断言
- `percpu_t.current_task` —— `arch/percpu.h` 里断言

**这比 x86 少一层，是架构给的，不是省略出来的。**

顺带把 `percpu_t` 的地址字段从 `uintptr_t` / 指针改成了 `u32` ——
不是为了省内存（ARM 上一样是 4 字节），而是**只要结构体里有一个指针宽度的
字段，宿主上的布局就与目标不同，"在宿主机上验偏移"这件事就做不成**。
改完之后 `sizeof(percpu_t) == 44` 在两个平台上一致。

#### 四、★ 一个被断言逼出来的真问题：VFP 的对齐 ★

最初把 VFP 区写成 `aligned(32)`，理由是"VFP 按 4 字一组访问，对齐足一点稳"。
那是**没有依据的保守取值**，而且立刻撞上现实：`heap_alloc` 只保证 8 字节
（`heap.h` 的 `HEAP_ALIGN`），于是"结构体声明 32 对齐、分配器只给 8"
构成未定义行为。

真实要求：ARMv7-A 的 VFP 传输指令要求地址 **4 字节对齐**。
x86 的 16 是 `fxsave64` 的硬件要求，**ARM 没有对应物**。
（对照 `include/cpu/fpu.h:7` 的 `aligned(16)` —— 那个 16 是有依据的。）

所以去掉对齐属性，由自然对齐（`u64` 成员带来的 8）决定，并加了一条
**故意的绊线**：

```c
_Static_assert(_Alignof(struct arm_thread_control_block) == 8u,
               "TCB 的自然对齐不是 8 —— 有新字段要求更强的对齐,而 heap_alloc 只保证 8。…");
```

将来谁照抄 x86 的 `aligned(16)`，这里会立刻报错，迫使他面对分配器的问题，
而不是让代码在目标板上以未定义行为的方式跑起来。

#### 五、验收

| 判据 | 结果 |
|---|---|
| `tcb_ctx_layout` | **0** —— 汇编按宏读回 17 个字段全部一致 |
| `exc_frame_layout` | 0（无回归）|
| 全量自检 | **44 passed / 0 failed** |
| 板上实测 | `Tcb ctx : sizeof=512 align_ok=1 -> PASS` |
| 宿主单测 | `tests/test_arm32_tcb.py`：字段名与宽度逐条钉、TaskStatus 数值、指针 typedef 约定、"刻意没有的字段" |

汇编侧还加了一组 `.if` / `.error` 静态断言，钉住**指令序列所依赖的前提**
（`r[]` 必须在 ctx 开头、槽是 4 字节步长）—— 这些在 C 侧看不出来。

`boot/context.S` 的布局自检用**返回码**而不是"打印后停机"：
布局错了应该报 FAIL 后继续跑，一次看到全部问题，而不是把内核弄停。

### 11. M4-9：抢占（搬帧）—— 已完成，板上 62/0

**这一步的核心是一句架构事实**：

> ARM 的异常帧里**没有 SP**。`EXC_FRAME_LEAVE` 的收尾是
> `add sp, sp, #0x38; rfeia sp!`，所以返回后的 SP **恒等于帧基址 + 64**。
> ⇒ **帧在谁的栈上，谁就被恢复。**

于是"切换任务"在 ARM 上就是**在目标任务的栈上另搭一个帧**，
再把那个帧交回 `rfeia` —— 而不是 x86 那样就地改写一个帧
（x86 的帧里有 `rsp`/`ss`，`iretq` 从帧里取，所以就地改就够了）。

| 文件 | 职责 |
|---|---|
| `src/sched.c` | **纯逻辑**：帧搬迁的数据搬运（`sched_ctx_from_frame` / `sched_frame_for` / `sched_frame_from_ctx`）+ 四条可切换性绊线 + 可调度性判据。宿主穷尽测 |
| `src/sched_kern.c` | 接真实 TCB/栈池/队列；`sched_tick` 里"收现场 → 搭新帧 → 切 current" |
| `boot/vectors.S` | `_vec_irq` / `_vec_svc` 共用协议：**返回值就是"要从哪个帧离开"**（`mov sp, r0`），照抄 x86 `call timer_handle; mov rsp, rax` |
| `boot/context.S` | `arch_svc_yield`（陷阱式让出）；`arch_ctx_save` / `arch_ctx_switch`（原语，调度器已不用）|

#### 唯一的一条切换路径

```
timer IRQ ──┐
            ├──> c_irq_handler / c_svc_handler ──> sched_tick(frame)
svc #YIELD ─┘                                            │
                                  ┌──────────────────────┘
                     "从哪个帧离开" ← 不切换：原 frame
                                     切换：在目标栈上新搭的帧
```

**源 OS 只有一个切换点**（`timer_handle`），而 `scheduler_yield()` 也不是例外 ——
它是 `scheduler_ticks = TIME_SLICE; int $32`，软中断进同一个入口。
ARM 侧对应 `svc #ARM_SVC_YIELD`。

⚠ 为什么用 SVC 而不是 GIC 的 SGI：**`int n` 不受 IF 屏蔽**，而 SGI 是一条 IRQ，
`CPSR.I=1` 时会被挂起。源 OS 的 `scheduler_sleep_ns()` 是
`do { yield(); } while (WAIT);` —— 在关中断的上下文里用 SGI 会**空转不切换**。

M4-8 那套 `arch_ctx_switch` 让出**已退场**：两套机制保存的寄存器集不同
（协作式只存 r4-r11+r0），被抢占过的线程若被协作式切回来，r1-r3/r12 就是垃圾，
而且**不报任何错**。

#### ★ 这一步踩的四个坑，是同一类错误 ★

四次都是**凭"应该是这样"去写死一个硬件的位，而那个值在板上可以读出来核对**：

| 我断言的 | 板上真值 | 症状 |
|---|---|---|
| "`cpsr=0` 是 User 模式" | **不是任何模式**（0b00000 未分配）| 发现得早，只改注释 |
| "内核 CPSR 是 `0x53`" | **`0x153`**（A 位是复位值 1，`msr cpsr_c` 碰不到它）| 新线程**第一条指令**异步外部中止。DFAR 还是垃圾值（FS=0x16 时 DFAR 无意义），把方向带偏一整轮 |
| "内核是 `.arm`，T 位必为 0" | **`0x173`（Thumb）** —— libgcc 的 `__udivmoddi4` 就是 Thumb，`timer_read_us()` 的 64 位除法会进去 | `invalid_ctx` 涨到 45965，线程卡在就绪队列里 |
| "SP 必然 8 字节对齐" | **`0x0299FFA4`**（4 mod 8）—— Thumb 的 `push {r4,r5,lr}` 是 12 字节 | `invalid_ctx` 涨到 30716，启动流程回不来 |

**规程**：要写"硬件一定如此"的常量或判据之前，先问**"这个值我在板上读过吗"**。
没读过就先读，或者写成"只统计不判失败"。

**正确的做法也有样本了**：`ARM_CPSR_KERNEL` 这个手写常量配一条
`sched_cpsr_kernel` 自检，把它与"板上真读到的 CPSR"直接对账 ——
手写常量 + 板上对账，比"推理得更仔细些"可靠。

#### 验收判据：为什么不能是"两个计数都大于零"

没有抢占时，先跑的那个线程会**一路跑完再挂起**，然后另一个才开始 ——
两个计数照样都是正的。所以判据必须是**交错**，而交错可以由线程自己见证：

```c
/* 我在跑的时候，对方是不是已经跑过了？ */
if (*p->other != 0u) p->saw_other = 1u;
```

无抢占时**第一个跑的线程永远见证不到**；判据要求**两个见证同时为 1**，
所以与"谁先跑"无关。

板上实测：

```
a=14073 b=14047 saw=(1,1) sp_bad=(0,0) done=1
switched=30 preempted=27 invalid=0
```

三条独立的量互相印证：计数几乎相等（公平）、两个见证都成立（真的交错）、
`sp_bad` 全 0（每一刻都跑在自己的栈上）。

#### 破坏性 A/B：`g_reloc_skip`

置 1 时 `sched_tick` **照做完一切**（计费、挑下一个、改状态、挪队列、收现场、
搭新帧），只在最后一步**不把新帧交出去**。

| | 两个探针线程 |
|---|---|
| 搬帧开 | `a=14073 b=14047`，`saw=(1,1)` |
| 搬帧关 | **`a=0 b=0`** —— 一次都没跑起来 |

同一段代码、同一对线程、同一段时间预算，只差这一步 ⇒ **搬帧是承重的。**

### 11. 退化实现清单（M4 期间必须持续维护）

**这张表是 M4 计划里明确要求的东西。** 理由：M4 期间会有若干处"先退化顶上"
的实现，它们**都能让上层编过、跑起来、测试也过**，但不是最终形态。
如果不在每一处都留下"这是退化 + 何时替换"的标记，它们会变成后面每一层
默认接受的前提，而那种债务要到 M7（用户态）才爆发。

| # | 退化的东西 | 当前位置 | 为什么现在可以退化 | 什么条件下必须替换 |
|---|---|---|---|---|
| D1 | **`errno` 是全局变量，不是每任务** | `src/krlibc.c` | M4-1 阶段还没有线程可谈 | **M4-6 有了 TCB 之后**，改成 per-task（与 Linux 的 `current->errno` 同构）|
| D2 | **`strtok` 的状态是全局静态变量** | `src/krlibc.c` | fs 层只有 `vfs.cpp` 的 2 处调用，暂时可控 | M4-11 有了可睡眠原语与任务之后。两个任务同时 strtok 会互相踩 —— 旧 XJ380 也是这个实现，所以不是移植引入的，但必须记下来 |
| ~~D4~~ | ~~**`select_next_task` 被简化成"取队首"**~~ | — | — | **已结案（M4-8.4）**。源 OS 的 `select_next_task_safe` 还有 avg_vruntime 闸门、fallback 扫描、`mark_task_dispatched`、`wake_sleeping_task(current)` —— **那些现在全都在**（`sched_select_next` / `sched_queue_scan` / `sched_mark_dispatched`）。<br>★ 而且它不只是"简化"：**唤醒睡眠任务的那次扫描就在里面**，取队首的写法里根本没有它 ⇒ 睡下去的任务永远醒不过来。这就是 M4-8.4 做的第一件事 |
| D5 | **`is_task_schedulable` 省掉了 `parent_group` 两条** | `src/sched.c` | 内核对线程还没有进程组（M7 才有） | M7。照抄会让**每一个**线程都不可调度（`parent_group == NULL`），所以只能先省 |
| ~~D6~~ | ~~**`sched_tick` 里的 CPU0 护栏**~~ | `src/sched_kern.c` | — | **已结案(M4-10.3,`a158e53`)**。拆掉的前提是四件都到位:每核队列、每核 idle、每核 `current_task`/`scheduler_ticks`/计数器、每核 VFP 指针。⚠ 护栏期间它还在**掩盖**一个 bug(见 D12 那条里的 `g_vfp_save_f`)|
| ~~D7~~ | ~~**异常帧可能不是 8 字节对齐**~~ | `boot/vectors.S` 的 `_vec_irq`/`_vec_svc` | — | **已结案（D7）**。量出的规模是**一轮 1688 次**（早先"20~28"是在启动早期取的，低估了）。<br>★ **修法与计划里猜的不同**：计划写的是"帧 16→18 字 + 存原始 SP"，那是想岔了 —— 问题不在"帧落在哪"，而在**调 C 的那个边界**。帧**不能挪**（`EXC_FRAME_LEAVE` 靠 `帧基址 + 64 == S` 还原被中断的 SP），而 C 函数拿到的是 **r0 里的帧指针**、不是 SP ⇒ 只要 `bic sp, sp, #7` 一条指令，回来 `mov sp, r0` 恢复即可，不动帧、不动偏移、不动断言。<br>判据是新增的硬自检 `c_handler_sp_aligned`（必须 0）：处理函数入口读自己的 SP。实测 0（修复前必然等于 1688）|<br>⚠ 同时改正三处**错误因果**：帧大小是不是 8 的倍数**决定不了** SP 对齐；M4-2 那次 Undefined 是 FPU 没使能，与对齐无关 |
| ~~D8~~ | ~~**VFP 上下文根本没保存/恢复**~~ | — | — | **已结案(M4-9.5,`53da560`)**。见下面「VFP 现场:补账」一节 —— 判据在板上成立(65/0),且有破坏性 A/B |
| ~~D9~~ | ~~**`runtime_ticks` 从不累加**~~ | — | — | **已清(M4-9.5)**：照源 OS `scheduler.cpp:398` 每 tick 加一 |
| ~~D10~~ | ~~**`sched_tick_account()` 成了死函数**~~ | — | — | **已删(M4-9.5)** |
| ~~D11~~ | ~~**`SCHED_MAX_SWITCHES_TRACKED` 成了死宏**~~ | — | — | **已删(M4-9.5)** |
| ~~D12~~ | ~~**就绪队列无锁**~~ | `src/sched.h` 的 `sched_queue_t` | — | **已结案(M4-10.4)**。每核一把 **irqsave** 锁(入队在线程上下文、选取在中断上下文 ⇒ 普通自旋锁就是**同核自死锁**),选取整段持锁 —— 与源 OS 的 `select_next_task_safe` 一致(`scheduler.cpp:325-360`)。<br>⚠ 源 OS 另有**全局** `scheduler_lock` 罩 `add_task`/`remove_task`(`:530`/`:564`),M4-10 **刻意没引**:ARM 侧现在只有 add,那把锁**没有第二个用户**(本项目删过两个这样的死物,D10/D11)。**M4-11 有线程退出时必须补上** |
| ~~D6~~ | ~~**`sched_tick` 里的 CPU0 护栏**~~ | — | — | **已结案(M4-10.3,`a158e53`)**。拆掉的前提是四件都到位:每核队列、每核 idle、每核 `current_task`/`scheduler_ticks`/计数器、每核 VFP 指针。⚠ 护栏期间它还在**掩盖**一个 bug(见 D12 那条里的 `g_vfp_save_f`) |
| ~~D12~~ | ~~**就绪队列无锁**~~ | — | — | **已结案(M4-10.4)**。每核一把 **irqsave** 锁(入队在线程上下文、选取在中断上下文 ⇒ 普通自旋锁就是**同核自死锁**),选取整段持锁 —— 与源 OS 的 `select_next_task_safe` 一致(`scheduler.cpp:325-360`)。<br>⚠ 源 OS 另有**全局** `scheduler_lock` 罩 `add_task`/`remove_task`(`:530`/`:564`),M4-10 **刻意没引**:ARM 侧现在只有 add,那把锁**没有第二个用户**(本项目删过两个这样的死物,D10/D11)。**M4-11 有线程退出时必须补上**。<br>★ 顺带:那个全局量 `g_vfp_save_f` 也是同一类问题 —— 它让"每核结构里的 `cur_vfp_f`"变成第二份真相,而双核下第二份真相互相覆盖 ⇒ 已删 |
| ~~D15~~ | ~~**新线程的 vruntime 漏了 `- WAKEUP_CREDIT`**~~ | — | — | **已结案(M4-10.1,`ccb7156`)**。见下方 M4-10 一节 |
| ~~D13~~ | ~~**串口排他用的是"关调度"，不是锁**~~ | — | — | **已结案（M4-11.1）**。原状：`console_excl_begin/end` = `sched_disable/sched_enable`，只在"单核 + 单写者"下成立，代价是报告那几秒整个系统停摆。⚠ **计划原写"可睡眠的 `mutex` 到位后换成它"—— 那句话是错的**：源 OS 的 `mutex` 是 **yield 型**（`mutex.cpp:21-22` 的注释明说"不把线程切到 WAIT"），根本没有可睡眠互斥。⇒ 已换成**源 OS 那把 yield mutex**（`src/mutex.c` 纯状态机 + `src/mutex_kern.c` 钩子），逐条语义见 `arch/mutex.h`。<br>★ 换的过程里踩到两个**只有真锁才会暴露**的坑，都写进了实现注释并进了计划 §0.5.5（48/49）：①`console.c` 原来那个**全局**嵌套计数在"第二个写者"面前是漏洞（它按深度、不按持有者 ⇒ B 线程看到 `depth != 0` 就直接打印，锁形同虚设）②源 OS 的"等锁 = `scheduler_yield()` 空转"在本移植里**会静默死锁**（它的 BSP idle 是候选，我们的不是 —— 见 D16/D17）。<br>判据：`console_excl_wait`（这把锁真被竞争过）/ `console_excl_sched_off`（报告区间停摆 ≤ 50ms；旧做法是几千 ms）/ `console_excl_alive`（报告期间状态线程照常在跑）+ 两条绊线；破坏性 A/B 见 M4-11.1 一节 |
| ★ D14 ★ | ★ **线程没有退出路径 —— 尾部那段终止循环不可达** ★ | `src/kmain.c` 的 `thread_finish()`；6 个调用点 | 自检探针"干完活"之后 `sched_park_self()` 就永久挂起了 | **M4-11.2**（★ 已决定**不回收**，见下）。源 OS 的对应物是 `pcb.cpp:501-504`（`kill_thread` 之后 `while (true) hlt`）—— 也就是说 **`wfi` 在这里是对的**（它的正当用途是"永久停住"，不是"idle 省电"），只是**现在还到不了那个循环**。<br>⚠ **但"释放栈 + 释放 TCB"不是垂死线程干的**（计划原话写错了）：源 OS 里 `kill_thread()` 只置 `DEATH`，`kill_thread0(task)` 那一行**是注释掉的**；真正的释放发生在回收路径（`kill_proc0` → `kill_thread0` → `remove_task` + `free(thread)`），而那条路对挂在 `kernel_group` 上的内核线程**不可达**（完整证据链见 `docs/PTASK.md` §2.5/§9.3）。<br>★★ **2026-09-13 决定：不改源 OS 的行为** ⇒ ARM 侧只做**两段式的第一段**（`status = DEATH` → 让出 → `wfi`），**不引入源 OS 没有的回收器**。代价与判据：内核栈池**只增不减**（`KSTACK_SLOTS` 32 × 1 MiB，启动自检约用到 30/32）⇒ `kstack_headroom` 从"D14 的临时哨兵"升级成**永久容量判据**，用量每次启动都打出来 |
| D17 | ★ **偏离（不是退化）：`mutex` 的"等锁让出"钩子不是纯 yield，而是"睡一个 tick 再重试"** ★ | `src/mutex_kern.c` 的 `hook_yield()` | 源 OS 是 `scheduler_yield(); cpu_relax();`。而 ARM 侧**两个 idle 都刻意不作调度候选**（D16），自检报告又跑在 **idle 上下文**里 ⇒ 纯 yield 的等锁者在 `sched_select_next` 的兜底链（`src/sched.c:467`）里永远赢过 idle：**拿锁的人等放锁的人、放锁的人等拿锁的人让出 CPU** —— 静默死锁，表现为"报告打了一半就没了" | **不打算改回去**（改回去就是死锁）。状态机一个字没动（仍是源 OS 的"不挂等待队列、拿不到就重试"），只把"两次重试之间那一下"从"让出"换成"让出一个 tick"。★ 这是 D16 的**连锁后果**；若将来 idle 变成候选（或报告挪出 idle 上下文），这条偏离应当被重新审视 |
| ~~★ D15 ★~~ | ~~★ **新线程的 vruntime 漏了 `- WAKEUP_CREDIT`** ★~~ | `src/sched.c` 的 `sched_entity_init()` | — | **已结案(M4-10.1,`ccb7156`)**。源 OS 是 `base > CREDIT ? base - CREDIT : 0`(`scheduler.cpp:294`),M4-8 写成了 `= base`(把参数当成了"当前时刻")⇒ 新线程比源 OS 晚 4ms 才被优先考虑。<br>★ **宿主单测当时是"跟着实现一起写错的"**:它断言 `vruntime == base`,所以一路全绿。改正时把判据**对着源 OS 逐值重写**(含 `base<credit`、`base==credit` 两个边界) |
| D16 | ★ **偏离(不是退化)：idle 的 `task_level` 我们显式设成 `TASK_IDLE_LEVEL`，源 OS 的 BSP idle 实际是 0** ★ | `src/sched_kern.c` 的 `sched_register_boot_idle` / `sched_register_ap_idle` | 源 OS 里 AP idle 显式设 `TASK_IDLE_LEVEL(1)`（`smp.cpp:154`），而 **BSP idle 从不赋 `task_level`**（`main.cpp:520-541`，memset 后保持 **0 = `TASK_KERNEL_LEVEL`**）| **不打算"照抄"这个 0**（已按意图实现）。理由：level 0 会让 BSP idle 变成**可调度候选**（`is_task_schedulable` 只排除 level 1），而它 `context0.rip == 0` ⇒ 被选中时 `timer_handle` **放弃这次切换**（白做一次派发）、并且被 EEVDF 计费。这显然是**漏赋值**而不是设计 ⇒ 我们两个 idle 都设 1，与源 OS 的**意图**（idle 不可停、不作候选）一致。★ 细节与向作者确认的问题见 `docs/PTASK.md` §2.4/§4.2 |
| ~~D3~~ | ~~内核用硬浮点编译~~ | — | — | **已结案：不是退化，是照源 OS 的设计。** 见下方「FP 上下文」一节 |


#### FP 上下文：已按源 OS 对齐（D3 结案）

加 palloc 时炸出的 FPU 雷（见上一节）牵出一个必须跟源 OS 对齐的设计点。
查 **x86 XJ380 的实际做法**：

```c
/* include/cpu/fpu.h —— 每个任务一份 */
typedef struct { uint8_t fxsave_area[512] __attribute__((aligned(16))); } fpu_context_t;
void save_fpu_context(fpu_context_t *ctx);     /* fxsave64  */
void restore_fpu_context(fpu_context_t *ctx);  /* fxrstor64 */

/* kernel/task/scheduler.cpp:121-122 —— 每次上下文切换都做 */
save_fpu_context(&current_task0->fpu_context);
restore_fpu_context(&target->fpu_context);
```

`kernel/task/pcb.cpp` 的 fork 路径 `memcpy(..., 512)` 复制它，`main.cpp` 给 idle
线程也存一份。编译选项里只有 `-mno-80387`（禁 x87），**没有 `-mno-sse`** ——
也就是说**源 OS 允许内核使用 SIMD**。

**⇒ 结论：ARM 侧照做，不要改成"内核永不碰 FP"。**

| x86 XJ380 | ARM32 对应物 |
|---|---|
| `fpu_context_t` = 512B FXSAVE 区 | VFP 上下文 = d0–d31（32×8=256B）+ FPSCR（4B），对齐 8 |
| `fxsave64` / `fxrstor64` | `vstmia` / `vldmia` 存取 d0–d31，`vmrs`/`vmsr` 读写 FPSCR |
| 放进 PCB，切换时保存/恢复 | 放进 TCB（M4-6 定结构），M4-7 的切换点调用 |
| 内核可自由用 SSE | 内核可自由用 VFP（`-mfpu=vfpv3 -mfloat-abi=hard` **保持不变**）|

**⚠ 因此 `-mgeneral-regs-only` 是错的** —— 它会让 ARM 侧偏离源 OS。

#### ★★ 但这件事**根本没有落地**（2026-09 补记）★★

上面那张表里写的"M4-7 的切换点调用"，**从来没做**。全树搜
`vstmia|vldmia|vmrs|vmsr|fmxr|fmrx` —— **零命中**；
`TCB` 里的 `vfp[64]` 与 `fpscr` 从头到尾没有任何代码读过或写过，
是纯粹的**死字段**（占了 260 字节的 TCB 体积，也占了 `sizeof(tcb)=512` 里的一半）。

也就是说：**表里写下的设计是对的，实现是零。** 这类"文档写了、代码没做"的缺口
比"没想过"更危险，因为读文档的人会以为它已经在了。

M4-9 之前它的暴露面还小（切换是罕见的主动行为）；**M4-9 之后是每 4ms 一次的
非自愿抢占**，任何一个线程里出现 `double`/`float` 就会被别的线程悄悄改掉
d0–d31 —— 而症状是**算出错的数**，不是崩溃。

⇒ 修法（照源 OS 的形状）：在 `boot/context.S` 加
`arch_vfp_save(u32 *d, u32 *fpscr)` / `arch_vfp_restore(const u32 *d, u32 fpscr)`
（`vstmia`/`vldmia` 存取 d0–d31、`vmrs`/`vmsr` 读写 FPSCR）。
**但调用点不在 `sched_tick` 的搬帧那一段** —— 见下面 M4-9.5 那一节，
那里把"为什么不能在 C 链里做"查清楚了。

### VFP 现场：补账（M4-9.5，`53da560`）

**结论：已补上，板上 65/0，并且有破坏性 A/B 证明它承重。**

#### 放在哪 —— 这个决定是查出来的，不是顺手选的

直觉做法是在 `sched_tick` 的搬帧那一段调 `arch_vfp_save/restore`。
**不行**，理由是实测出来的：

> GCC 在**本内核里**已经用 `vldr d16,[pc]; vstr d16,[rN]` 这个
> **64 位清零惯用法**（`palloc_selftest` / `percpu_table_reset` /
> `sched_entity_init` / `smp_release_cpu1` 四处都在用），
> 而 `sched_tick` 里那句 `pc->scheduler_ticks = 0u` 正是"64 位存零" ——
> 今天编成两条整数 store，**换个版本或改一行就可能变成上面那个惯用法**。

一旦那条链上任何一处碰了浮点寄存器，恢复完 next 的现场之后就会被它的尾声
（`vpop {d8-d15}` 之类）踩掉 —— 而**其余部分全是对的**。

⇒ 存与取都放进汇编，**卡在 C 的两端**：

```
_vec_irq:
    EXC_FRAME_ENTER                @ 帧已建好
    bl  arch_vfp_save_current      @ ★ 任何 C 之前 ★
    mov r0, sp
    bl  c_irq_handler              @ 这一整段爱怎么用 VFP 就怎么用
    mov sp, r0
    bl  arch_vfp_restore_current   @ ★ 所有 C 返回之后 ★
    EXC_FRAME_LEAVE                @ 之后只剩纯整数指令
```

判据里于是**不再含任何"编译器不会…"的前提**。`_vec_svc` 同样处理。

#### 那两个指针从哪来

汇编**不去算 TCB 的偏移**（那要求宿主与目标布局一致，而 TCB 有指针字段）。
它读**每核结构**里的 `cur_vfp_d` / `cur_vfp_f` —— 由 `sched_set_current()`
用 C 算好写进去。每核结构是"全 u32 字段"的（`percpu.h` 有断言），
偏移两边一致，汇编可以放心用。`sizeof(percpu_t)` 56 → 64。

两个函数都查 `percpu_t.cpu_id`，**只在 CPU0 生效** —— 否则 CPU1 的 1kHz
定时器会去消费 CPU0 布下的状态，两个核同时用一份 d0–d31。

#### 判据为什么长这样

两个线程各自把 **d0–d31 整片**填成自己的图案（**只填一次**），
然后反复读回核对，并核对 FPSCR 的舍入模式（两位 `RMode` —— FPSCR 其余
大多是**累积状态标志**，任何一次浮点运算都能置位，拿它们做地标会误判）。

- ★ **"只填一次"是判据成立的关键**：每轮重填的话，线程被切回来时会先把
  自己的图案写回去，"寄存器被对方改过"这件事就被它自己抹掉了 —— 判据永远通过。
- ★ 因此两个线程**不对称**：只有"被换下又换回来"的那一个读得到对方的图案。
  判据是"**至少一个**检出"，不是"两个都检出"。这一点写在代码注释里，
  实测也正好是 `(8183, 0)` —— **一正一零**。

#### 结果

```
正例      Sched fp : bad=(0,0) rmode_bad=(0,0) done=1        → PASS
对照组    VFP A/B  : bad=(8183,0) rmode_bad=(8184,0) done=1  → 检出
```

对照组是 `g_vfp_skip = 1`：那个变量的**存储与读取都在汇编里** ——
不能为了问一句"跳不跳"再调进 C，那就又把 C 拉回窗口里了。

实现位置：M4-9.5（已做）。**新增一处退化，就得进上面那张表。**

（后续 M4 各阶段每引入一处退化，都往这张表里加一行。表里任何一行在
对应条件满足后仍未替换，都是需要解释的。）

### M4-8.5：无饥饿判据 + 常驻饥饿监视器（已完成，板上 72/0）

**要证的是一条二值性质，不是比率**：

> 一个**可运行**的线程，永远不会被无限期地漏掉。

测法：把时间切成 **N = 64 tick** 的窗口，每个窗口里、每个"被监视且**仍可运行**"的
线程，**它自己的计数器**都必须至少涨过一次。

⚠ 它**不是**公平性测试。计划里明写"任何策略在等权 + 纯占用负载下都会通过"，
所以「公平份额 ≈ 理论值」「EEVDF 与 round-robin 有别」两条**列为不验** ——
份额类判据**没有区分能力**，窗口类有（下面那个 A/B 就是证明）。

#### 三层，各证一件事

| 层 | 证什么 | 实测 |
|---|---|---|
| 宿主 `tests/test_arm32_sched.py` §13 | **策略**：模拟 tick 时钟驱动 `sched_select_next`，穷尽扫 **K=2..8** × 两种片长 | `K=8 windows=16 misses=0 min_ticks=135` |
| 板上（相 5）| **机制**：K=4 个绝不让出的线程 + 睡眠式监视器 | `windows=23 full=23 events=0 late=0`；`phase=1598 ms observed=1562 tick`（**覆盖率 98%**）；`exit_ms=1600×4 adv=4/4` |
| 破坏性 A/B（9.78）| **检出器有区分能力**：`g_pick_sticky` = 严格非抢占 | 串口上真的打出报警行，且受害者确实没推进 |

**板上的四项判据缺一不可**（少任何一项，判据都能被"什么都没发生"骗过去）：

```
events == 0    没有任何"可运行却没推进"的 (线程,窗口) 对
late   == 0    监视器自己也没被饿着（窗口没长得离谱）
full   >= 8    ★ 非空转：真有那么多个窗口里 4 个线程都处于可运行状态
adv    == K    ★ 负载是真的：4 个线程都真的在推进
```

★ `observed` 与 `phase` 的比值是**覆盖率审计**：它回答"这段相里监视器真的在观测吗，
还是它自己也被饿着、只看了几眼"。`windows=23` 而 `observed/phase = 98%` ⇒ 真的在看。
**没有这个数，"零违反"与"零观测"在读数上长得一样。**

#### 破坏性 A/B：`g_pick_sticky`（严格非抢占）

同一段选取代码、同一批负载、**同一个监视器**，只把"选谁"改成"current 可运行就还是它"：

```
正例（相 5）  windows=23 full=23 events=0 late=0 adv=4/4
对照组        STARVATION: win=1797 tick frozen=278 ms usable=1519 missed=4/4
              STARVATION: win=1249 tick frozen=45 ms usable=1204 missed=3/4
              adv=1/4 events=7 late=2 full=2 win_max=1797 left=0 -> DETECTED
              counts=1967739,0,0,0（粘住的那一个跑满，其余三个停在 0）
```

**监视器真的报警了**（不是"数变了"），而且三个受害者的计数停在 0 ——
"可运行却被无限期漏掉"这句话在读数上是这个样子。

两处与计划原话的偏离，都写在代码里：

- `g_pick_sticky` 是**计数**而不是布尔。粘住之后**没有任何人能把它清零**
  （kmain 与监视器都轮不到 CPU）⇒ 对照组会就地挂死，而那不是"判据不承重"，
  是**测不了**。计划里"粘住 300 次"这种写法本身就要求它是一个计数。
- 粘住**不适用于 idle**：idle 的 status 是 RUNNING，照字面粘住它，启动流程把自己
  粘住、一个线程都起不来。而源 OS 在 current 是 idle 时**本来就会切走**
  （`timer_handle` 只在 `task_level != TASK_IDLE_LEVEL` 时累积时间片）
  ⇒ **排除 idle 才是 M4-8 的忠实等价物**。

#### ★ 这一步最贵的一课：判据自己会漏报

两个坑都是**判据的实现**错，不是被测代码错。两条都进了计划 §0.5.5（第 39/40 条）。

**① "有人关了调度 ⇒ 这个窗口作废"是错的判据。**

排他输出（`console_excl`）是**关调度**实现的：自检报告那几秒钟整个系统停摆，
那不是饥饿。第一版监视器见到停摆就把整个窗口扔掉 —— 上板立刻打回来：

> A/B 里监视器被粘住的窗口是 **1.2 s**，而其中 **100 ms** 是关调度的
> （对照组自己造线程的那一段）⇒ 整段被扔 ⇒ `events=0 late=0`，
> 报"**未检出**" —— **而线程确实被饿死了**。

⇒ 判据改成 **`窗口长度 − 停摆时长 = 可执行时长`**，再拿可执行时长去判：

| 场景 | 窗口 | 停摆 | 可执行 | 结论 |
|---|---|---|---|---|
| 自检报告 | 5 s | 5 s | ≈ 0 | 不作数（单列计数）|
| 粘住对照组 | 1.2 s | 0.1 s | 1.1 s | ★ 报警 ★ |
| 正常 | 64 ms | 0 | 64 ms | 判决 |

**教训：任何"作废性"判据都要问一句 —— 它会不会把我要抓的东西一起扔掉？**

**② `timer_delay_ms(N)` 不是"等 N 毫秒"，是 N 次串行 1 ms 等待。**

实现就是 `while (ms--) timer_delay_us(1000)`，而 `timer_delay_us` 从**自己进去的
那一刻**重新起算 ⇒ **被抢占多久就多花多久**。实测：

| 相 | 请求 | 实际 |
|---|---|---|
| 相 5 | 1200 ms | **2880 ms**（差额正好是负载线程占住 CPU 的 1600 ms）|
| 相 4 | 3000 ms | 约 **5.4 s** —— 那个"状态线程醒了 **5** 次（而不是 3 次）"从 M4-8.4 起就在日志里，当时没人追问 |
| A/B | 2400 ms | 4523 ms |

**内核没有问题，是用错了函数。** 但它让"相有多长"变成一个猜不出来的数，而判据的
时序（负载活多久、粘住多久）全押在它上面 ⇒ 这两处改用自己算截止时刻的
`starve_wait_ms()`（相 4 是已验证的代码，**刻意不动**，账记在 §0.5.8）。

#### 常驻监视器：它现在的价值是"回归监视器"

按现在的代码，**饿死在构造上不会发生**（`sched_select_next` 结尾的
`if (best == NULL) best = fallback;` 保证只要有候选就有人被选中）。所以这一步
不是"今天找到了 bug"，而是：

- 守住**下一次**改动 —— 尤其 M4-10 的每核队列 + 锁，"任务落在所有队列之外"
  是那里的高频形态，而本项目**已经手撞过一次同类**（M4-8.4 的两个 bug：
  `sched_kern_init()` 清空队列抹掉常驻线程、对还在队列里的节点写 `sched_next = NULL`
  截断链表）。两者的定位代价都是一次 JTAG 会话；**常驻监视器会把它们变成一行输出**。
- 补上四条机制判据里的最后一条（切换 ✅ / 抢占 ✅ / idle ✅ / **无饥饿 ✅**）。

监视器**常驻**（`smon`，睡 64 ms 醒一次），发现饥饿立刻在串口上打一行 ASCII
（`STARVATION: win=... frozen=... usable=... missed=.../...`，限量 3 行）；
观察名单由造线程的人**显式登记**、用完撤销 —— 名单里留着已挂起的线程虽然也会被
`status` 检查跳过，但那是"碰巧安全"，显式撤销让"现在没有东西在被监视"是**说出来的**。

#### 顺带修好的一处真隐患：`sched_disable/enable` 改成计数

关调度的用途有两个而且会**互相嵌套**（`console_excl` 自带计数；kmain 里"造线程"
那种多步窗口）。布尔版本只要有一次不成对、或嵌套里内层先 `enable`，就会**提前打开**
调度 —— 而"提前打开"不报任何错，只表现为"偶尔被切走一次"。

⇒ 改成深度计数，不变式：**深度 0 ⟺ 调度开着**。于是"多调一次 enable"是无害的空操作，
而不是把某个还没结束的临界区打开。新增 `sched_off_total_ns()`（累计停摆时长，
给监视器做上面那个减法）。

#### 验收

```
自检          sched_no_starvation = 1        (expect == 1)   PASS
              sched_no_starve_windows = 23   (expect >= 8)   PASS
板上总计      72 passed / 0 failed（上一阶段 70/0）
宿主          9 failed / 73 passed（9 项预先存在且无关）
```

**没有新增退化条目** —— 这一步是新判据 + 新仪器，不是退化。

### M4-10：SMP 调度 —— 两个核都在跑线程（已完成，板上 86/0）

> ★ **task 子系统（调度/线程/生命周期）的分层事实、与源 OS 的差异、以及
> M4-11 的重新规划都在 `docs/PTASK.md`** —— 本节只讲 M4-10 这一步本身。

**范围照源 OS**（★ 全树搜 `balance|migrat|load_avg|steal` **零命中** ⇒
**不发明均衡器**）：每核 idle、每核队列、创建时挑最短队列、应用级钉 CPU0。
顺手结掉 **D6**（CPU0 护栏）、**D12**（队列无锁）、**D15**（vruntime 补偿）。

#### 三个核的东西，各管一件事

| 东西 | 在哪 | 依据 |
|---|---|---|
| `current_task` / `scheduler_ticks` / `switched` / `preempted` / `invalid_ctx` / `cur_vfp_d` / `cur_vfp_f` | `percpu_t`（TPIDRPRW 取本核）| `PROCESSOR_INFO`(`include/smp/smp.h:43-46`) |
| 每核就绪队列 + 一把 **irqsave** 锁 | `sched_kern.c` 的 `kern_runq_t g_runq[]` | `select_next_task_safe` 整段持锁(`scheduler.cpp:325-360`)；源 OS 的 `spin_lock` 同样关中断并保存 RFLAGS |
| 每核 idle | `g_idle_tcb[cpu]`（静态实例）| BSP 见 `main.cpp:520-541`，AP 见 `smp.cpp:152-181` |

**选核策略放在纯逻辑层**（`sched_pick_cpu()`）—— 板上只有一组真实负载，
证不了"平局留给核号小的""没就绪的核不参与"这些边界，所以按 §4.5 的分工丢给宿主穷尽测：

```c
u32 sched_pick_cpu(i32 task_level, const u32 *queue_len, const u32 *ready, u32 cpu_num);
```

三条语义逐字照抄 `add_task()`：起点 CPU0 + **严格小于**（平局给核号小的）、
`TASK_APPLICATION_LEVEL` **跳过整个扫描**（永远 CPU0）、只按队列长度挑。
⚠ 10.5 那条 `if` **今天不可能被执行**（没有应用级线程）——
所以它的判据只能在宿主单测里（"编译过 = 零功能证据"这条规矩）。

#### ★★ 纠正一处计划里的错误断言：AP 的 idle **会**被切回来 ★★

计划原文说"AP 的 idle 也是 `current_task` + 入队，循环体是 `while (true) pause` ——
它**只被切走、不被切回**"。**错的。** 依据 `scheduler.cpp:358`：

```c
tcb_t result = best != NULL ? best : (is_current_task_runnable(current) ? current : idle);
```

`is_task_schedulable()` 把 idle 排除在候选之外(`:178`)，所以 `idle` **只能**从这条
兜底链进来 —— 它的用途正是"**本核没有可运行线程时回到 idle**"。idle 的 `ctx.pc`
在第一次被切走时就被现场帧填成非 0，从此是可恢复的普通上下文。

按错的断言实现，AP 会在最后一个线程挂起后**卡在那个已挂起的线程上**：
`is_current_task_runnable(current)` 为假、又回不到 idle ⇒ **该核永久停摆**，
而另一个核看起来一切正常。⇒ 判据写成"**切回来之后它还在跑**"：

```
Sched smp   : cpu1 switched=+16 preempted=+15 idle_harvested=1 back=1
```

`back` 的测法是"idle 的循环体就是 `pc->loops++`，线程都挂起之后再等一小段，
它必须继续涨" —— 不涨就说明 AP 卡住了。

#### 判据：三件事都成立才叫"这个核在调度"

```
Sched smp   : threads=3/3 assign=(0,3) ran=(0,3) place_bad=0
Sched smp   : cpu1 switched=+16 preempted=+15 idle_harvested=1 back=1
Sched smp   : cpu0 switched=+13 preempted=+7  runq=(15,4)
```

- **搬过帧**（`switched`）、**换下的是真实线程**（`preempted` ——
  只切 idle 的话它恒为 0，那不算"在调度"）；
- `ran=(0,3)`：核号是**线程自己**写进 `g_smp_ran[]` 的（`percpu_self()->cpu_id`
  只有本核能填对 —— 与 AM3 用 MPIDR 而不是 `cpu_id` 判"CPU1 真的起来了"同一规矩）；
- `place_bad=0`：每个新线程都落在"**造它之前**更短的那个队列"上。

#### ★ 三个"判据自己错了"（都进了计划 §0.5.5 的坑表）

**① "两个核各分到一半"是错的期望。** 源 OS 的规则比的是**队列长度**，而队列是
"全部线程的**名册**"（睡着的、挂起的都在里面）。启动到那一相时 CPU0 的名册上已经
躺着十几个已挂起的探针 ⇒ **每一个新线程都落到 CPU1**，直到 CPU1 也攒到那么多。
上板实测 `assign=(0,6) ran=(0,6)`，而**两个核都在正常调度**。
⇒ 判据改成"**规则有没有被正确执行**"，不是"负载有没有均分"。
（那条"成批倾斜"的性质本身是源 OS 的行为，已记进计划的未决项。）

**② A/B 报了一个漂亮的假 DETECTED。** 线程池只有 32 个槽（`KSTACK_SLOTS`），
而 D14 说线程**没有退出路径** ⇒ 每造一个线程永久占一个槽。启动到对照组时已用掉
二十几个 ⇒ 对照组 6 个线程**全部创建失败** ⇒ `assign=(0,0) ran=(0,0)`，
而"CPU1 一次都没切"照样成立。⇒ 判定里必须带"这一相真的发生了什么"，
否则报 **INCONCLUSIVE**；并新增自检 `kstack_headroom`（>= 4）给 D14 当哨兵。

**③ 报告之后那五组对照全是单核判据。** 相 6 把选核打开之后，VFP 对照组当场从
`bad=(7914,0)` 变成 `bad=(0,0)` 报"未检出" —— 看起来像 VFP 保存/恢复坏了，
实际是**两个核各有自己的 d0-d31**，"被对方改掉"根本不会发生。
⇒ 9.75 之前显式钉回 CPU0。**这一条最值得记**：
"判据不承重"与"判据的适用条件没了"看起来一模一样，而排查方向完全相反。

#### 10.6：顺手修掉一个"只有双核才会爆"的隐患

`arch_vfp_save_current` / `_restore_current` 原来有两道护栏：`cpu_id != 0` 就早退，
以及读一个**全局量** `g_vfp_save_f`（存 `&cur->fpscr`）。两核同时调度时，
CPU0 布的指针会被 CPU1 覆盖 ⇒ **CPU0 把 FPSCR 存进别人的 TCB**。
它今天不爆只因为护栏让 CPU1 根本不走这条路 —— 也就是说：
**护栏一直在掩盖它。** 现在汇编直接读 `percpu_t.cur_vfp_f`，全局量已删。

#### 验收

```
自检          smp_sched_two_cores / smp_sched_created / smp_sched_placement
              smp_sched_cpu1_ran / smp_sched_cpu0_ran / smp_app_level_cpu0
              smp_fp_cpu1_checked / smp_fp_cpu1_ok / smp_fp_all_ok
              smp_ap_idle / smp_ap_idle_back / smp_cpu1_sched_ready
              kstack_headroom                              全部 PASS
板上总计      86 passed / 0 failed（上一阶段 73/0）
六组 A/B      搬帧 / VFP / 扫描唤醒 / 无饥饿 / 选核 / 不换栈 —— 全部检出
              + `Preempt A/B: invalid bound: delta=53 <= 59 -> PASS`
宿主          9 failed / 73 passed（9 项预先存在且无关）
命令通道      tmp-test/shell_test.py 10/10（顺带修好了它的过期等待窗口，见坑 44）
```

**没有新增退化条目** —— D6/D12/D15 结案，新增的注意点写在计划的未决项里
（名册倾斜、栈池余量、`online` 含义加强）。

#### 收尾补掉的五个缺口（用户点名，一次补齐）

| 缺口 | 补法 | 实测 |
|---|---|---|
| ★ **CPU1 上的浮点现场从没被验过**（10.6 的理由是"两核会互踩"，而 FP 探针全钉在 CPU0）| `smp_sched_probe` 顺手核对 d0-d31（图案只填一次），而这一批探针按"最短队列"正好落在 CPU1 | `fp: bad_all=0 cpu1_bad=0 cpu1_checks=19010`；判据拆成"真的核对过"(`smp_fp_cpu1_checked`)与"没被破坏"(`smp_fp_cpu1_ok`) |
| ★ **10.5 应用级→CPU0 在板上不可达** | 新增 `sched_kthread_create_level()`（M7 建应用线程走同一条路），相 6 造一个应用级线程：**哪怕 CPU1 更空也必须落 CPU0** | `app_level cpu=0 (must be 0)`；顺带 `ran=(1,3)` ⇒ "两个核都跑过**我的**探针"成立 |
| **相 4 仍在用 `timer_delay_ms`**（N 次串行 1ms，被抢占多久就多花多久）| 改名 `wait_ms_wall()` 并把**全部 9 处**等待（相 0/2/3/4 + 三组对照组窗口）换成截止时刻语义 | 相 4 从 **5.4 s 回到 3.0 s**，整机启动短约 4 秒；`phase=XXXX ms` 从此是设计值 |
| **`invalid=0->53` 那个"时序敏感点"** | 定位 + 变成**有上界**的判据：`invalid` 每次派发决策涨一次 ⇒ 上界 = 窗口 ÷ 片长 | `delta=53 <= 59 -> PASS`，差额正是最前面那两次（那时 ca/cb 的 ctx 还是自己的 ⇒ `switched=+2`）——**两个数现在同一条账** |
| **"均衡性"** | 不是缺口而是决定：源 OS 没有周期性均衡（全树零命中）⇒ 不改；这次把**规则**两个方向都验了 | `place_bad=0`（内核级按最短队列）+ 应用级钉 CPU0 |

⚠ **补第 4 条时踩到的一条既有约定**：报告在 9.75 **之前**就打完了，所以
**对照组里算出来的判据不能当报告项**（这正是另外五组 A/B 的结论只以普通输出给出的原因）。
我第一版把 `invalid bound` 写成了报告项 ⇒ 它读到的永远是初值 0 ⇒ **一条假 FAIL**。
**"判据放在报告的哪一侧"本身是有约束的** —— 这条现在也写进了代码注释。

### M4-11.1：串口排他从"关调度"换成**一把真锁**（已完成，板上 91/0）

要证的事一句话：**排他输出不再让整个系统停摆，而互斥仍然是成立的**。

#### 交付物

| 文件 | 角色 |
|---|---|
| `include/arch/mutex.h` + `src/mutex.c` | 源 OS `kernel/task/mutex.cpp`(189 行)的**纯状态机**：不含 MMIO/CP15/内联汇编/时间源，唯一依赖是四个函数指针（取当前任务 / 让出 / 进临界区 / 出临界区）⇒ 宿主穷尽测 |
| `src/mutex_kern.c` | 四个钩子的实现 + 串口那把锁 + 两条绊线计数 + 破坏性 A/B 的开关 |
| `src/console.c` | **只做转发**（原来是"全局嵌套计数 + 只在 0→1 时叫钩子"，见坑 48） |
| `tests/test_arm32_mutex.py` | 12 节、约 90 条断言：递归/`-EDEADLK`/`-EPERM`/`-EBUSY`/`-EINVAL`/销毁后/空指针/NULL current 边界/临界区进出口配对/让出次数逐值 |

#### 语义：哪些照抄，哪些没照

照抄（逐条对着 `mutex.cpp` 的行号，见 `arch/mutex.h`）：

```
拿不到就"让出 + 重试"，**不挂等待队列、不切 WAIT**（作者原注释：yield 型）
递归计数 rcc；非递归自锁 ⇒ -EDEADLK；解锁非持有者 ⇒ -EPERM
锁着的时候销毁 ⇒ -EBUSY；已销毁 ⇒ -EINVAL；trylock 被占 ⇒ -EBUSY
trylock **一次都不让出**（否则它就不是非阻塞的）
"没有 current 任务时空闲的锁会命中 owner == current" 这个边界也照抄（宿主测试钉住）
```

不照的两处，各有理由：

- **`mutex_t.wait_queue` 字段没有搬**：源 OS 里它从头到尾没人用（只在 create/destroy 里建/销毁），是"本来打算做可睡眠、后来改成 yield"的化石 ⇒ 按本项目对死物的规矩（D10/D11）不搬。
- **等锁那一下不是纯 yield，而是"睡一个 tick 再重试"**（记为**偏离 D17**）：源 OS 的 yield 能工作，是因为它的 **BSP idle 是 level 0 = 可调度候选**；我们刻意不作候选（D16）⇒ 纯 yield 会静默死锁（坑 49）。状态机没动，动的只是钩子。

#### ★ 这一步最贵的三个坑（都进了计划 §0.5.5：48/49/50）

**① 全局嵌套计数 + 真锁 = 漏洞（坑 48）。** 第一次带锁上板的症状**不是**报错，而是
`verify_board.py` 说"报告不完整：SUMMARY 声明 90 passed，实际只解析到 85 条"，
同时"这把锁被竞争过吗"那个计数恒为 **0**。根因：`console.c` 的 `g_excl_depth` 是
**全局**的，它按**深度**而不是按**持有者**计数 ⇒ 状态线程看到 `depth == 1` 就
一声不吭地直接打印。旧做法（排他 = 关调度）下这个计数是对的，因为那时
"调度关着 ⇒ 没有第二个写者能跑"。处置：嵌套交回**按持有者**计数的递归互斥
（源 OS 的 `rec = true` + `rcc`），`console.c` 只转发。

**② 照抄"让出"会死锁（坑 49）。** 源 OS 的等锁是 `scheduler_yield()` 空转。
本移植的 idle **不作候选**，而自检报告就跑在 idle 上下文里 ⇒
等锁者 yield 之后仍是 `best`，调度器把它自己选回来（连切换都不做），
持锁的 idle 永远回不来：**拿锁的人等放锁的人、放锁的人等拿锁的人让出 CPU**。
处置见 D17（睡一个 tick）。⚠ 这一条是 D16 的**连锁后果** —— 两次偏离是连着的。

**③ 对照组的"活着"观测量不能押在别人的相位上（坑 50）。** 破坏性 A/B 连着两次报
`NOT DETECTED`，而同一份日志里两边的 `sched_off` 明明差着 1200ms（+0 vs +1200）。
因为"系统活着"取的是 **1Hz** 状态线程的唤醒计数，而窗口是 1.2s：
窗口边界正好错过一次唤醒就是 0；更早一次则是它已经卡在"等锁重试"里
（那个计数在进循环**之前**就加过了）。处置：换成**周期 64ms** 的饥饿监视器窗口计数
（前者的三十分之一）⇒ A 组十几次、B 组 0 次，确定成立。

#### 板上判据（5 条，都在报告里）

```
console_excl_wait       这把锁**真的被竞争过**吗（让出次数 > 0）。恒 0 的话"互斥成立"就是空话
                        ⚠ 适用条件：第二个写者（状态线程）存在 —— 一起判
console_excl_sched_off  报告区间里"调度被人为关掉"的毫秒数 ≤ 50（旧做法 = 整个报告长度，几千 ms）
console_excl_alive      报告区间里状态线程**醒过**（旧做法恒为 0：那几秒整个系统停摆）
console_mutex_off_wait  ★绊线★ 在"关调度"的窗口里等过锁 ⇒ yield/睡眠都失效、同核持锁者饿死
console_mutex_errors    ★绊线★ 状态机返回非 0（生产路径上不可能：递归锁 + 深度在锁那层）
```

实测（同一块 AC880-CB，多次 JTAG 加载结果一致）：

```
CHECK console_excl_wait      = 1 (expect == 1)   PASS
CHECK console_excl_sched_off = 0 (expect <= 50)  PASS
CHECK console_excl_alive     = 1 (expect == 1)   PASS
CHECK console_mutex_off_wait = 0 (expect == 0)   PASS
CHECK console_mutex_errors   = 0 (expect == 0)   PASS
Sched nostarve: K=4 windows=23 full=23 events=0 late=0 skipped=0   ← 旧做法这里是 skipped=1
```

#### 破坏性 A/B：同一段负载，只换"拿住排他"的实现

```
Console A/B : lock(1200ms)   -> sched_off=+0 ms    alive=+19 (win=+19 wake=+0 wait=+0)
Console A/B : legacy(1200ms) -> sched_off=+1200 ms alive=+0  (win=+0 wake=+0 wait=+0)
Console A/B : yield-lock vs disable-lock -> DETECTED
```

两段完全一样的负载（拿住排他、等 1200ms 墙钟、放开），`console_mutex_set_legacy(1)`
把那两句换回 `sched_disable/sched_enable`。判据是**两个侧面都换过来**才算检出：
停摆时长（0 vs 窗口长度）与"窗口里系统还在跑"（19 > 0 vs 0）。
（只想看一个侧面的话，"窗口里其实什么都没跑"也能显得 A 很好 —— 坑 43 的同一类。）

---

### M4-11.2：线程退出路径（D14 结案，板上 97/0）

要证的事一句话：**线程"干完活"走的是源 OS 那条退出路径（标记 `DEATH`），
而不是我们自己的"挂起"** —— 而按 2026-09-13 的决定，**退出不释放任何资源**。

#### 做了什么

| 项 | 内容 |
|---|---|
| `sched_thread_exit()`（`src/sched_kern.c`） | ← 源 OS `process_exit()` `pcb.cpp:494-505` + `kill_thread()` `:447-458`：唯一拒绝条件是 `TASK_IDLE_LEVEL` ⇒ `status = DEATH` ⇒ `sched_yield()` ⇒ `for(;;) arch_wfi()`（**不返回**）|
| `thread_finish()`（`src/kmain.c`） | 探针的收尾改走它（原来走 `sched_park_self()`）。★ **它的尾部循环第一次是活的**（D14 那句"不可达"到此为止）|
| `sched_park_self()` | **删除** —— 源 OS 没有这个函数，而 11.2 之后它没有调用者（D10/D11 的规矩）|
| 池容量 | `KSTACK_SLOTS` **32 → 64**（`KSTACK_L2_TABLES` 40 → 72）。理由**变了**：原来是"16 个任务 × 2 栈"，现在"**不回收 ⇒ 池容量 = 每次启动能创建的内核线程数上限**"。上限是硬的（`KSTACK_MAX_SLOTS` 就是 64）|
| 收尾不复活死者 | 新增 `probe_park()`：各对照组收尾原来无条件 `status = WAIT`，现在**已退出的（DEATH）不动** —— 否则会把"它死了"这件事从名册上抹掉，而 11.2 的判据正是去名册里数 DEATH |

#### 板上判据（6 条，都在报告里）

```
kthread_exit_reached      真的有线程走到过退出路径（非空转条件）
kthread_exit_death    ★   去**名册里数** status == DEATH 的线程 ≥ 1 ★
                          这是"退出语义照源 OS"的证据：它是 DEATH，不是 WAIT
kthread_exit_refused      "有代码想停 idle" 的次数（源 OS 也会拒绝）== 0
kthread_exit_lock_guard   ★ 绊线的**正向对照**：报告区间里 kmain 正持着串口锁，
                          问一次必须为真 ⇒ 证明下面那条绊线接在正确的信号上
kthread_exit_held_lock    "持着串口锁退出" 的次数 == 0
kstack_peak_used          池的峰值用量（数值本身要能看见）
```

`kthread_exit_held_lock` 是 **`docs/PTASK.md` §5 第 4 条耦合项**的处置：
`mutex->owner` 在源 OS 里**从不注销**，而"线程会退出"让它第一次变得可达 ——
持锁退出会把那把锁**永久**带走（下一次打印一直等下去，不报任何东西）。
按"不改源 OS 行为"的决定，处置是**报警 + 正向对照**，而不是发明
源 OS 没有的"退出时注销 owner"。

#### 实测

```
 Kernel stack: 64 slots x 1024 KB (+4 KB guard) at 0x023AA000
 Kernel stack: peak_used=20 of 64 (headroom=44)  threads: exit=16 death=16 refused=0
 CHECK kthread_exit_reached    = 1  (expect == 1)   PASS
 CHECK kthread_exit_death      = 16 (expect >= 1)   PASS
 CHECK kthread_exit_refused    = 0  (expect == 0)   PASS
 CHECK kthread_exit_lock_guard = 1  (expect == 1)   PASS
 CHECK kthread_exit_held_lock  = 0  (expect == 0)   PASS
 CHECK kstack_peak_used        = 20 (expect <= 60)  PASS
 SELF-TEST: 97 passed / 0 failed
 ...
 Kernel stack: final peak_used=33 of 64 (headroom=31)  threads: exit=25 death=25
 Boot complete: all post-report A/B groups done, entering main loop
```

★ **两个数都要看**：报告那一刻只用掉 **20** 个槽，而**报告之后**那七组对照组
还要再造十几个（按"不回收"它们同样不还）⇒ 一次启动的**真实峰值是 33**。
32 槽的旧池在这里正好越界 —— 所以"扩到 64"是**实测依据**，不是估的。

★ 顺带纠正一个先前的猜测：前几轮我一直按"启动自检要造三十来个线程、池已经贴边"
来说（那是从名册长度 `runq=(16,4)` 推的）。报告那一刻的实测是 20 —— 接近上限的
是**报告之后**那段。⇒ 容量的答案只能取"最终峰值"，所以它现在也打出来了。

#### ★ 这一步顺带炸出三个坑（51/52/53）—— 后两个是**先前就潜伏**的

11.2 写完第一次上板：报告全绿 + 我那条 A/B 检出，但**整机在报告之后崩了**。
逐行比对完整日志才看清：`No-starve A/B` / `Smp-pick A/B` 那两行压根没出现。

- **坑 51（11.1 就埋下了）**：9.76/9.77 用了 9.75 那套 `ctx` 快照/还原。
  它唯一的用途是**修 `reloc_skip` 造成的污染**；在没有污染的场景里用它，等于把
  窗口里**真的跑过的线程恢复到过期现场**（它的栈早被自己后来的执行用过）⇒
  一被调度就跳进垃圾。11.1 之前它潜伏着：那时状态线程被"关调度"冻在报告里
  （实测 `late=4539388 us`）、窗口内根本不动，于是"恢复过期现场"恰好是空操作；
  11.1 让它恢复准时（`late≈300us`）之后，这段代码**第一次真的被执行到**。
- **坑 52（更早埋下的，与本次改动无关）**：`arch_vfp_save_current` 的前提是
  `TPIDRPRW == 0 ⇒ 还没有每核结构，别碰浮点`，而 **TPIDRPRW 的复位值 UNKNOWN**
  （JTAG 的 `rst -system` / `rst -processor` 之后可能留着上一次运行的地址）；
  `percpu_init_self()` 又晚在栈池之后才跑 ⇒ 这中间的第一发 tick 会拿过期地址
  去读 `cur_vfp_d`，读到非 0 就把 d0-d15 **存进野地址**。实测 `DFAR=0x82600C02`、
  `DFSR=0x801`、pc 落在 `vstmia r0!, {d0-d15}`，而且**每次重新加载都复现**
  （崩在写 TPIDRPRW 之前 ⇒ 过期值一直留着），看起来像"改一行代码就把板子改坏"。
  ⇒ 开中断前显式 `arch_write_percpu(0)`。
- **坑 53（验证方法本身）**：报告是"报告那一刻"的快照，而报告之后还有七组破坏性
  对照组，它们坏掉的方式是**日志中途断掉**。⇒ 内核在主循环前打一行
  ` Boot complete: ...`，`verify_board.py` 把它做成**强制要求**：缺了它，
  即使 SUMMARY 全过也算失败（实测当场生效：91 passed 仍判失败）。
- **坑 54（修好一处的连带）**：11.1 之后 `shell_test.py` 的 `ver` 用例开始假失败 ——
  状态行劈在 `Switches  : 0x..` 中间。根因是 **shell 的输出从来没有排他，
  而状态行一直能抢占它**，只是 11.1 之前状态行一打印就"关调度"、别人跑不了，
  交错机会少得多。⇒ shell 的整段输出（回显 + 命令输出 + 下一个提示符）
  包进排他；单字符回显不包（逐键取锁不值当）。修复后 `shell_test.py` 回到 **10/10**。

四个坑的提交：`7afd0f7`（51）、`deb286d`（52+53）、`e5e5aba`（54）。

#### 破坏性 A/B（第八组）：同一段等待，只换"干完活之后怎么停"

```
 Exit A/B    : death=16 -> 17 (probe ran=1)  legacy: death=17 (probe ran=1)
 Exit A/B    : exit-path(DEATH) vs wait-park(WAIT) -> DETECTED
```

两个探针做的是同一件事（调 `thread_finish()`），唯一的差别是
`sched_set_exit_legacy(1)` 把退出路径退回**旧的 `WAIT` 挂起**。
两边"线程不再被选中"完全一样 ⇒ **只看"它停了没有"判不出来**；
能判出来的是名册上的状态，而那是**去名册里数**出来的
（`sched_runq_count_status()`，独立于退出路径自己报的数）。

非空转条件两项：两个探针都必须造出来、而且都真的跑到了那一步（`probe ran=1`）——
否则"计数没涨"只是因为什么都没发生（坑 43 的同一类）。

#### 明确**没有**做的事（都是决定，不是没做完）

- **不回收**：没有 reaper、没有 `remove_task`、没有 `free` TCB/栈。
  源 OS 的 `kill_thread0()` 对挂在 `kernel_group` 上的线程没有任何可达路径
  （`kill_proc` 直接拒绝），而决定是**不改源 OS 的行为**。
- **不补全局 `scheduler_lock`**：源 OS 用它罩 `add`/`remove`，我们没有 `remove`
  ⇒ 那把锁**没有第二个用户**（D10/D11 的规矩）。D12 那条"等 M4-11 有 remove 时补上"
  到此改写为**不做**，理由记在这条上。
- **不注销 `mutex->owner`**：见上面的绊线。

代价（长期约束，不是待修的缺陷）：**池只增不减** ⇒ `kstack_peak_used` 与
`kstack_headroom` 从"临时哨兵"升级为**永久容量判据**；任何"每请求一个线程"的
新用法都必须先解决容量（扩池已经用到设计上限 64）。

---

### 12. 其它待办（AM3 及以后）

- 缓存维护与 Cortex-A9/PL310 勘误 —— **L1 与 L2 均已使能**；
  588369 已规避，727915 未触及相关路径，775420 尚未处理
  （见上方缓存章节的勘误表）
- **自检报告与自动校验**（进行中）：把各项自检统一成固定格式回传，
  配 `tmp-test/verify_board.py` 抓串口解析并给出退出码 ——
  让"上板验证"成为一条命令，而不是人工读日志
- **串口 RX 命令通道**：shell 已实现，但接收方向卡在物理链路上
  （见上方第 8 节，已定位到片外，不是软件问题）
- AM3 双核：`sev` + OCM 跳板替代 x86 的 INIT-SIPI-SIPI
  （ACTLR 的 SMP 位与维护广播已经在缓存阶段置好了，少一个坑）
- SVC 入口接上真正的系统调用层（`_vec_svc` 目前只做诊断）
- 中断嵌套与在 SVC 模式下运行处理函数（现在是 IRQ 模式 + 自己的栈）
- 4KB 小页（当前只有 1MB 段；需要更细粒度才能给 DMA 缓冲单独设属性）

---

## 已修复的坑（供参考）

以下几处在板上实测踩到，都已修好并留下注释：

1. **全局定时器 64 位读取算法写反了**。
   原本要求"低 32 位两次读相等"才接受结果，但计数器在 333MHz 递增，
   两次 MMIO 读之间早就变了，于是**永远重试**。
   正确写法是"读高→读低→再读高，两次高相等即可"。

2. **`timer_init()` 排在延时调用之后**。自检延时和 `fail_stop()` 都依赖定时器在跑，
   顺序反了就变成死等。

3. **UART 轮询没有超时**。外设不响应时 `while (!(SR & TXEMPTY))` 永不退出，
   整机挂死。现在所有轮询都有自旋上限，无串口表现为可诊断的降级。

4. **`uart_init()` 解引用空指针，静默配错波特率**。
   重构时把分频搜索挪进 `uart_baud_search()`，而该函数对 `result == NULL`
   是直接返回的 —— 于是 `result->baudgen` 读的是**地址 0**。
   ARM 关闭 MMU 时地址 0 是 OCM 且可写，所以既不崩溃也不报错，
   只是把 `BAUDGEN`/`BAUDDIV` 写成垃圾值。而 `console_init()` 恰好就是用
   `NULL` 调用它的，还在 `kmain` 里紧跟一次正确的 `uart_init()` 之后执行，
   把刚配好的寄存器又覆盖掉 → 横幅必然乱码。
   **教训**：允许 `NULL` 的参数要在被调用函数内部统一转成局部变量，
   不能只在某一层做判断。

5. **JTAG 读心跳读太早，误判成"内核卡死"**。
   `magic` 在 `kmain` 开头就写，但 `DIRM0`/`TICKS`/`IRQCOUNT` 在横幅之后才写，
   而 9600 波特下横幅要 ~870 ms。验证脚本 `con` 后只等 500 ms 就读，
   于是稳定读到一片 0，看起来像"卡死在 `uart_puts`"。
   现在改为轮询 `ticks != 0` 作为门控。
   **教训**：用轮询标志位判断"子系统起来了"时，要选一个真正代表该子系统的标志，
   而不是最早写下的那个。

6. **缩短波特率测量窗口会引入系统性偏差**。
   曾把测量字符数从 200 降到 32 以减少串口噪声，结果参考时钟从精确的
   100 MHz 偏到 103.17 MHz。原因是 `UART_SR` 的 `TXEMPTY` 表示 **FIFO 空**，
   而最后一个字节此时还在移位寄存器里 —— 计时窗口实际只覆盖了 `N-1` 个字符，
   算出的波特率被高估 `N/(N-1)` 倍（N=32 → 3.2%，N=200 → 0.5%）。
   而闭环只会把"测出来的值"拉到目标，**不会察觉偏差来自测量本身**。
   `MEASURE_COUNT` 已加注释说明为什么不能调小。
