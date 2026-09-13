# 使用「启用 UART1 的 ps7_init」+「带 AXI GPIO 的旧比特流」加载内核
#
# 关键点:ps7_init 管 PS(时钟/MIO/DDR),比特流管 PL(AXI GPIO -> LED),
# 两者相互独立,可以交叉组合。这样既能拿到串口,又不丢 LED。
#
# 用法: xsdb run_kernel_uart.tcl

set BIT "C:/Users/VeryS/Documents/fpgap/zynqs/AXI_GPIO_1/AXI_GPIO_1_SOFT/platform/hw/sdt/System_wrapper.bit"
set PS7 "C:/Users/VeryS/Documents/others/OpenXJ380/tmp-test/zynq/ps7_init_uart1.tcl"
set MIOFIX "C:/Users/VeryS/Documents/others/OpenXJ380/tmp-test/zynq/ps7_mio_bank1_18v.tcl"
set ELF "C:/Users/VeryS/Documents/others/OpenXJ380/out/kernel-arm.elf"

proc step {m} { puts "\n>>> $m" }
proc rd {a} {
    if {[catch {mrd -force $a} v]} { return "ERR" }
    set s [string trim $v]
    set i [string first ":" $s]
    if {$i >= 0} { return [string trim [string range $s [expr {$i+1}] end]] }
    return $s
}
# 数值版读寄存器。用 scan 而不是 expr {0x...}:
# expr 对 mrd 返回的异常文本(而非纯十六进制)会直接报错,
# 而 scan 解析失败时返回 0,可以稳妥地兜到 -1。
proc rdn {a} {
    set s [rd $a]
    if {[string equal $s "ERR"]} { return -1 }
    if {[scan $s %x n] != 1} { return -1 }
    return $n
}

# 判断"读到的是不是零"。直接做字符串比较,绕开进制解析 ——
# 早期轮询只关心"还没写" 与 "写了非零" 两种状态。
proc is_zero {s} {
    if {[string equal $s "ERR"]} { return 0 }
    if {[string equal $s "00000000"]} { return 1 }
    if {[scan $s %x n] != 1} { return 0 }
    return [expr {$n == 0}]
}

# 读 ARM 核心寄存器。用 catch 包住:不是所有版本都能读 DFAR/IFAR,
# 读不到时应当继续往下走而不是中断整段验证。
proc rr {reg} {
    if {[catch {rrd $reg} v]} { return "n/a" }
    set v [string trim $v]
    if {[llength $v] >= 2} { return [lindex $v 1] }
    return $v
}

step "connect"
if {[catch {connect} e]} { puts "FAIL connect: $e"; exit 1 }
puts "OK"

step "reset system"
if {[catch {targets -set -filter {name =~ "APU"}} e]} { puts "FAIL: 找不到 APU,板子可能需断电重启"; exit 1 }
rst -system
after 2000
puts "OK"

step "ps7_init (来自 opjtmp.xsa,已启用 UART1)"
targets -set -filter {name =~ "APU"}
if {[catch {source $PS7} e]} { puts "FAIL source: $e"; exit 1 }
if {[catch {ps7_init} e]}    { puts "FAIL ps7_init: $e"; exit 1 }
if {[catch {ps7_post_config} e]} { puts "FAIL post_config: $e"; exit 1 }
puts "OK"

# opjtmp.xsa 把 MIO bank 1 的电压声明成了 3.3V,而本板实际是 1.8V
# (核心板 VCCIO_BANK1 接 VCC1P8)。不修的话 MIO16-53 的输入阈值按
# LVCMOS33(~2.0V) 走,而 CH9102F 只能驱动到 1.8V —— UART1 RX(MIO49)
# 会恒读低,一个字节都收不到,且没有任何报错。详见该文件的注释。
step "修正 MIO bank1 IO 标准 (LVCMOS33 -> LVCMOS18)"
if {[catch {source $MIOFIX} e]} { puts "FAIL source miofix: $e"; exit 1 }
if {[catch {ps7_mio_bank1_18v_fixup} e]} { puts "FAIL miofix: $e"; exit 1 }
set mio_bad [ps7_mio_bank1_18v_check]
if {$mio_bad == 0} {
    puts "OK  MIO16-53 全部为 LVCMOS18"
} else {
    puts "警告: 仍有 $mio_bad 个 MIO 不是 LVCMOS18"
}

puts ""
puts "=== 关键寄存器核对 ==="
puts "MIO_PIN_48  = 0x[rd 0xF80007C0]   (期望 000016E0 = UART1_TX)"
puts "MIO_PIN_49  = 0x[rd 0xF80007C4]   (期望 000016E1 = UART1_RX)"
puts "APER_CLK    = 0x[rd 0xF800012C]"
puts "UART_CLK    = 0x[rd 0xF8000154]"
puts "UART1 MR    = 0x[rd 0xE0001004]   <== 非 0 就说明 UART1 活了"

step "load bitstream (AXI GPIO -> LED)"
targets -set -filter {name =~ "xc7z020"}
if {[catch {fpga -f $BIT} e]} { puts "FAIL fpga: $e"; exit 1 }
puts "OK"

step "download kernel"
targets -set -filter {name =~ "ARM*#0"}
rst -processor
if {[catch {dow $ELF} e]} { puts "FAIL dow: $e"; exit 1 }
puts "OK  PC = [lindex [rrd pc] 1]"

# 清空心跳区(0x20000..0x2003F)。
#
# 为什么必须做:OCM 不会被 rst -processor 清零,上一次运行留下的 magic
# 会让下面"等 magic 出现"的轮询立刻命中,于是在新内核还没走到写心跳之前
# 就把整片区域读成 0 —— 看上去像"内核卡住了",其实是读了残影。
# 这个坑真实发生过:串口上主循环明明在跑,JTAG 却报 loop=0/ticks=0。
step "clear heartbeat (防陈旧数据)"
for {set a 0x00020000} {$a <= 0x0002003C} {incr a 4} {
    mwr -force $a 0
}
puts "OK"

step "run"
con

step "wait for heartbeat"
set magic [rd 0x00020000]
for {set i 0} {$i < 20} {incr i} {
    after 250
    set magic [rd 0x00020000]
    if {[string match "*4F583338*" $magic]} { break }
}
puts "magic    = $magic"

# 仅仅"magic 出现"不代表可以读整片心跳区。
#
# magic 在 kmain 开头就写了,而 DIRM0/TICKS/IRQCOUNT 分别在横幅之后、
# 中断起来之后才写。9600 波特下横幅本身要 ~870ms,所以"看到 magic 立刻读"
# 会稳定地读到一片 0 —— 曾经据此误判成"内核卡在 uart_puts"。
# 这里改成等真正代表子系统起来的信号:周期 tick 计数非 0。
step "wait for IRQ subsystem (ticks != 0)"
set ticks_raw "00000000"
for {set i 0} {$i < 40} {incr i} {
    after 250
    set ticks_raw [rd 0x00020028]
    if {![is_zero $ticks_raw]} { break }
}
puts "ticks    = $ticks_raw"

# 再多给一点时间,让主循环跑几轮,把 LOOP/GT/SW 等槽位填上
after 1500

puts "loop     = [rd 0x00020004]"
puts "pl_led   = [rd 0x00020008]"
puts "gt       = [rd 0x00020010]"
puts "uart_clk = [rd 0x00020018]"
puts "dirm0    = [rd 0x0002001C]"
puts "uartok   = [rd 0x00020020]"
puts "measbaud = [rd 0x00020024]"
puts "ticks    = [rd 0x00020028]"
puts "irqcount = [rd 0x0002002C]"
puts "clksrc   = [rd 0x00020030]   <== 1=闭环收敛 2=兜底猜值"
puts "conviter = [rd 0x00020034]   <== 闭环迭代次数,0 表示没进迭代"
puts "baudgen  = [rd 0x00020038]"
puts "bauddiv  = [rd 0x0002003C]"
puts "mmustage = [rd 0x00020040]   <== MMU: 0=未开始 1=已建表 2=已配 3=正要开 4=已开 0xEE=自检失败"
puts "ledcheck = [rd 0x00020054]   <== AXI GPIO 写回读失配数;0=全对 0xFFFFFFFF=驱动没 probe 上"

# 交叉校验:ticks 应当约等于 uptime(ms),两者不符说明有中断被吞
set t [rdn 0x00020028]
set q [rdn 0x0002002C]
puts ""
puts ">>> ticks=$t irqcount=$q"
if {$t > 0 && $t == $q} {
    puts ">>> 1kHz tick 与 GIC 转发计数一致,零丢失"
} elseif {$t > 0} {
    puts ">>> 警告: ticks 与 irqcount 不一致,可能有中断未 EOI"
} else {
    puts ">>> 警告: 始终没有 tick,中断子系统未起来"
}

set u [rd 0x00020020]
puts ""
if {[string match "*00000001" $u]} {
    puts ">>> UART 已被内核探测到 —— 请看串口终端是否出现启动横幅"
} else {
    puts ">>> 内核仍未探测到 UART"
}

# 可选:注入故障。
#
# 用法: xsdb run_kernel_uart.tcl <selector>
#   1 = Data Abort   2 = Undefined Instruction   3 = Prefetch Abort
#
# 放在最后是有意的:只有先确认内核正常跑起来了,注入才有意义 ——
# 否则"触发了异常"和"内核本来就没起来"在串口上长得一样。
set sel [lindex $argv 0]
if {$sel ne "" && $sel != 0} {
    step "注入故障 selector=$sel"
    mwr -force 0x00020080 $sel
    puts "已写入 FAULT_SEL"

    # 异常处理函数会打完现场再停机,9600 波特下需要一点时间把日志吐完
    after 4000

    puts "FAULT_SEL  = [rd 0x00020080]  (内核应已清零,避免反复触发)"
    puts "PC         = [rr pc]"
    puts "DFAR       = [rr dfar]"
    puts "DFSR       = [rr dfsr]"
    puts "IFAR       = [rr ifar]"
    puts "IFSR       = [rr ifsr]"
}
exit 0
