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

槽 12–15 是为一个具体陷阱加的：**闭环收敛成功返回的值，和收敛失败后由调用方
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

配套的异常诊断（`c_data_abort_handler` / `c_prefetch_abort_handler` /
`c_undef_handler` / `c_svc_handler`）会打印出错地址与寄存器现场，
对应 x86 侧的 `kernel/wsod/`。

**这些处理函数是"出了事才跑"的代码，正常路径永远走不到** ——
所以专门做了故障注入（`src/fault_test.c`）：内核启动后用 JTAG 往
`0x00020040` 写选择器即可触发，**不需要重新烧录**。没有专门触发过，
就无法区分"处理函数写对了"和"处理函数根本没被调用、只是系统恰好没崩"。

一条命令完成"加载 → 跑起来 → 等中断子系统 → 注入 → 回读寄存器"：

```
C:\AMDDesignTools\2025.2\Vitis\bin\xsdb.bat tmp-test/jtag/run_kernel_uart.tcl <1|2|3|4>
```

配合串口抓取（会打印原始 hex，波特率不对时能区分"全 0x00 / 全 0xFF / 有字符"）：

```
python tmp-test/run_and_capture.py COM4 9600 2 <1|2|3|4>
```

**实测结果**（四个用例各跑一遍，注入前 `ticks` 与 `irqcount` 均相等）：

| 选择器 | 异常 | 关键证据 |
|---|---|---|
| 1 | Data Abort | `DFAR=0x50000000`、`DFSR=0x08`（synchronous external abort）、`pc=0x001008CC`、`r3=0x50000000` |
| 2 | Undefined Instruction | `pc=0x0010091C`（UDF 指令处）、`r4=0x00000002`（选择器）|
| 3 | Prefetch Abort | `IFAR=0x50000000`、`IFSR=0x08`、`pc=0x50000000`、`r4=0x00000003` |
| 4 | SVC | `!!! SVC (no syscall layer yet) - diagnostic only, returning !!!`，随后 `SVC returned normally` |

前三个停机 PC 分别为 `0x001000D8` / `0x00100104` / `0x0010011C`，
正是 `boot/vectors.S` 中三个向量各自的 `wfe` 自旋循环 ——
证明处理函数返回后确实停在了预期位置。

**SVC 与前三个有本质区别**：它的向量 `_vec_svc` 没有 `wfe` 自旋，
处理完就 `pop {r0-r12, lr}` / `movs pc, lr` 返回到 SVC 的下一条指令。
所以它的处理函数**不能**打印 "System halted"，否则会让人以为系统停了。
用例 4 输出末尾的 `SVC returned normally` 顺带验证了这条返回路径 ——
**那正是将来系统调用要走的机制**（`movs pc, lr` 在跳转的同时把 SPSR 拷回 CPSR）。

**踩坑提醒**：`DFSR`/`IFSR` 的故障状态在 **bit [4:0]**，不是 [3:0]。
`0x08` 在 [3:0] 下看着像"域故障"，实际 `[4:0]=0b01000` 是
"Synchronous external abort"（访问了没有从设备的地址）。上一版就写错了这个
提示，反而误导排障 —— 现在用 `fsr_status_text()` 显式查表。

### 5. 其它待办（M2 及以后）

- MMU / 页表（`include/mm/page.h` 的 `PTE_*` 全部是 x86 语义，必须重做）
- 缓存维护与 Cortex-A9/PL310 勘误 —— 建议 vendor Xilinx 的 `xil_cache.c`
  与 `xil_errata.h`（MIT 许可），见计划 §2.15
- SMP：`sev` + OCM 跳板替代 x86 的 INIT-SIPI-SIPI
- SVC 入口接上真正的系统调用层（`_vec_svc` 目前只做诊断）
- 中断嵌套与在 SVC 模式下运行处理函数（现在是 IRQ 模式 + 自己的栈）

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
