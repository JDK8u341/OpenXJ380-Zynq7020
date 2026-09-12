# arch/arm32 —— Zynq-7020 (Cortex-A9) 架构层

OpenXJ380 移植到 ARMv7-A 的第一块地基。对应 `docs/ZYNQ7020_PORT_PLAN.md`
的 M0 阶段：**工具链 → 构建系统 → 链接 → JTAG 直载 → 板级可见输出** 这条链路打通。

现状：内核能在真机上启动、点灯、并通过 OCM 心跳向 JTAG 汇报状态。
**这是一个骨架，不是可用的内核** —— 文件系统、任务调度、用户态都还没接进来。

---

## 目录结构

```
arch/arm32/
├── boot/
│   ├── start.S        启动汇编：屏蔽中断、建立各模式栈、清 BSS、跳 kmain
│   └── kernel.ld      链接脚本：加载到 DDR 0x00100000
├── include/arch/
│   ├── types.h        定宽类型（freestanding，不依赖 stdint.h）
│   ├── io.h           MMIO 读写 + 屏障语义
│   ├── cpu.h          架构接缝：中断/CP15/缓存/自旋锁
│   ├── platform.h     板级常量（Zynq-7020 / AC850+AC880）
│   ├── timer.h        全局定时器（nanoTime 的替代）
│   ├── uart_ps.h      Cadence UARTPS 驱动
│   ├── console.h      最小格式化输出
│   └── led.h          LED / 拨码开关
└── src/
    ├── kmain.c        内核入口
    ├── timer.c
    ├── uart_ps.c
    ├── console.c
    └── led.c
```

## 构建

```bash
# 生成 ARM 构建图（与 x86 图完全独立，互不影响）
python tools/gen_ninja.py --out build-arm.ninja --arch arm32

# 构建
ninja -f build-arm.ninja arm32        # 产物 out/kernel-arm.elf
```

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
1. led_init()                LED 先起来 —— 它不依赖未知时钟，是最可靠的反馈手段
2. 写 OCM 心跳 magic         JTAG 立刻可确认"内核活了"
3. timer_init()              ⚠ 必须早于任何 timer_delay_*
4. LED 上电自检              全亮 -> 全灭
5. uart_probe()              探测串口是否存在
6. 闭环收敛 UART 参考时钟 -> 初始化 -> 打印横幅
7. gic_init()                关中断下配置 Distributor / CPU Interface
8. irq_register() + 启动定时器 + gic_enable_irq()
9. irq_global_enable()       最后一步才打开 CPU 中断响应
10. 主循环                   跑马灯 + PS LED 慢闪 + 心跳刷新 + 周期串口输出
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

心跳区预留 32 槽（`0x00020000`–`0x0002007F`），故障注入选择器从
`0x00020080` 开始。这条边界由 `heartbeat.h` 里的静态断言双向锁住 ——
它曾经被踩过：选择器原先在第 16 槽，而心跳后来也扩到了第 16 槽，
一旦撞上，`fault_test_poll()` 会把心跳进度号当成注入码，
症状是"内核莫名进了 Data Abort"，而心跳本身看上去完全正常。

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
| **CH9102F** | PS USB 转串口 | `PS_UART_TXD = MIO48` / `RXD = MIO49` → **COM4** |
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
`0x00020080` 写选择器即可触发，**不需要重新烧录**。没有专门触发过，
就无法区分"处理函数写对了"和"处理函数根本没被调用、只是系统恰好没崩"。

一条命令完成"加载 → 跑起来 → 等中断子系统 → 注入 → 回读寄存器"：

```
C:\AMDDesignTools\2025.2\Vitis\bin\xsdb.bat tmp-test/jtag/run_kernel_uart.tcl <1|2|3|4>
```

配合串口抓取（会打印原始 hex，波特率不对时能区分"全 0x00 / 全 0xFF / 有字符"）：

```
python tmp-test/run_and_capture.py COM4 9600 2 <1|2|3|4>
```

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

### 7. 其它待办（M3 及以后）

- 缓存维护与 Cortex-A9/PL310 勘误 —— L1 已使能(6x);**L2(PL310)尚未配置**,
  它不在 CLIDR 里,必须走自己的寄存器(0xF8F02000)。需 vendor Xilinx 的
  `xil_cache.c` 与 `xil_errata.h`(MIT 许可),含 PL310 勘误 588369 / 727915,
  后者正是"Background Clean and Invalidate by Way 导致数据损坏",DMA 场景的典型杀手
- 按地址(MVA)的缓存维护目前只有 `arch/cpu.h` 里的朴素实现,**尚未做
  cache-line 对齐处理**,也没有勘误规避 —— 做 DMA 之前必须先解决这一块
- 设备/驱动描述层（方案已定：C 静态描述符表 + probe 循环；
  PL 部分暂缓，等真正做 PL 设计时再引入生成器）
- SMP：`sev` + OCM 跳板替代 x86 的 INIT-SIPI-SIPI
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
