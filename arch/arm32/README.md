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
| 19 | `0x4C` | 缓存维护自检（0 = 通过）|
| 20 | `0x50` | L2 有效性：128KB 工作集关/开 L2 的耗时比（实测 1.42）|

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

翻 `D:\BaiduNetdiskDownload` 里的官方资料（底板原理图、核心板原理图、
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

**当前验收：25 passed / 1 failed。**

未通过项 `irq_spurious = 1285`（判据 `== 0`）。已确认的事实：

- `irq_spurious` 读的是 `g_irq_stats.spurious_count`，即 **CPU0** 的虚假中断数；
- 连续 5 次采样（间隔 800ms）**冻结在 1285 不再增长**，而同期 `irq` 仍在按 1kHz 稳定上涨；
- `unhandled = 0`、`last_intid = 29`（定时器）—— 也就是说定时器本身没有异常。

所以它是**启动期间的一次性爆发**，不是持续的竞态。**但根因尚未定位，不当作已解决。**
下一步的隔离办法：把 SGI 那一段单独关掉再跑一次，看它是否回到 0，
以此判断爆发是否来自 SGI 风暴。在查清之前，这一项保持 FAIL。

### 10. 其它待办（AM3 及以后）

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
