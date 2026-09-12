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

`0x00020000` 起 9 个 32 位槽，JTAG 用 `mrd` 直接读，**不需要串口**：

| 槽 | 含义 |
|---|---|
| 0 | magic `0x4F583338`（"OXJ8"）|
| 1 | 主循环计数 |
| 2 | PL LED 图案 |
| 3 | 拨码开关值 |
| 4 | 全局定时器低 32 位 |
| 5 | PS LED 状态 |
| 6 | UART 参考时钟（0 = 未标定）|
| 7 | PS GPIO `DIRM_0` |
| 8 | UART 探测结果（1 = 存在）|
| 9 | 当前设定下实测的真实波特率 |
| 10 | 周期 tick 计数 |
| 11 | GIC 收到的中断总数 |

这条通道比串口更好用：能读任意内存、能看 PC/寄存器、崩溃后仍可读。
移植早期应当优先依赖它。

---

## 已知问题与待办

### 1. 本板串口不可用 —— 需重新生成 PL 设计（软件无法解决）

**现象**：UART0/UART1 寄存器读回恒为 0。

**根因**：现有 `AXI_GPIO_1` 比特流对应的 **PS7 配置没有启用 UART**。

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

**解决方向**（需要 Vivado）：

1. 打开 `AXI_GPIO_1_BSP.xpr` 的 Block Design
2. `processing_system7_0` → Re-customize IP → Peripheral I/O Pins
3. 勾选 **UART 1**（引脚自动选 MIO 48..49）
4. Validate → Save → Generate Bitstream
5. Export Hardware（勾 Include bitstream）得到新 XSA
6. 在 Vitis 里用新 XSA 更新平台 → 生成新的 `ps7_init.tcl`
7. 之后本驱动无需改动，`PLAT_CONSOLE_UART_BASE` 已指向 UART1

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
 UART ref clk : 100000000 Hz (source=1, self-calibrated)
 Baud         : requested=9600 actual=9600 (BAUDGEN=1736 BAUDDIV=5 err=0 ppm)
 If you can read this, the serial console works.
[XJ380/arm32] alive loop=1 led=0x10 sw=0x00 uptime=950 ms
```

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
[XJ380/arm32] alive loop=12 led=0x01 ticks=2219 irq=2219 uptime=3221 ms
[XJ380/arm32] alive loop=44 led=0x01 ticks=8661 irq=8661 uptime=9663 ms
```

| 检查项 | 结果 |
|---|---|
| tick 频率 | 2219→8661 次对应 3221→9663 ms，逐段**精确 1000 Hz** |
| 中断丢失 | `irq_count` 与 `ticks` **完全相等**（45705 = 45705），零丢失 |
| INTID | `29`（私有定时器），符合设计 |
| 虚假中断 | `spurious=0` |
| GIC 寄存器 | `GICD_CTLR=1`、`GICD_ISENABLER0` bit29=1、`GICC_CTLR=1`、`GICC_PMR=0xF0` |
| 定时器寄存器 | `LOAD=333332`、`CONTROL=0x07`（使能+自动重载+中断） |

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

配套的异常诊断（`c_data_abort_handler` 等）会打印出错地址与寄存器现场，
对应 x86 侧的 `kernel/wsod/`。**目前还没有被真实触发验证过** ——
需要一个专门的故障注入测试。

### 5. 其它待办（M2 及以后）

- MMU / 页表（`include/mm/page.h` 的 `PTE_*` 全部是 x86 语义，必须重做）
- 缓存维护与 Cortex-A9/PL310 勘误 —— 建议 vendor Xilinx 的 `xil_cache.c`
  与 `xil_errata.h`（MIT 许可），见计划 §2.15
- SMP：`sev` + OCM 跳板替代 x86 的 INIT-SIPI-SIPI
- SVC 入口接上真正的系统调用层（`_vec_svc` 目前只做诊断）
- 中断嵌套与在 SVC 模式下运行处理函数（现在是 IRQ 模式 + 自己的栈）

---

## 已修复的坑（供参考）

这三处在板上实测踩到，都已修好并留下注释：

1. **全局定时器 64 位读取算法写反了**。
   原本要求"低 32 位两次读相等"才接受结果，但计数器在 333MHz 递增，
   两次 MMIO 读之间早就变了，于是**永远重试**。
   正确写法是"读高→读低→再读高，两次高相等即可"。

2. **`timer_init()` 排在延时调用之后**。自检延时和 `fail_stop()` 都依赖定时器在跑，
   顺序反了就变成死等。

3. **UART 轮询没有超时**。外设不响应时 `while (!(SR & TXEMPTY))` 永不退出，
   整机挂死。现在所有轮询都有自旋上限，无串口表现为可诊断的降级。
