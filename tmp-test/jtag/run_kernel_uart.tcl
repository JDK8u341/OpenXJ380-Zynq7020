# 使用「启用 UART1 的 ps7_init」+「带 AXI GPIO 的旧比特流」加载内核
#
# 关键点:ps7_init 管 PS(时钟/MIO/DDR),比特流管 PL(AXI GPIO -> LED),
# 两者相互独立,可以交叉组合。这样既能拿到串口,又不丢 LED。
#
# 用法: xsdb run_kernel_uart.tcl

set BIT "C:/Users/VeryS/Documents/fpgap/zynqs/AXI_GPIO_1/AXI_GPIO_1_SOFT/platform/hw/sdt/System_wrapper.bit"
set PS7 "C:/Users/VeryS/Documents/others/OpenXJ380/tmp-test/zynq/ps7_init_uart1.tcl"
set ELF "C:/Users/VeryS/Documents/others/OpenXJ380/out/kernel-arm.elf"

proc step {m} { puts "\n>>> $m" }
proc rd {a} {
    if {[catch {mrd -force $a} v]} { return "ERR" }
    set s [string trim $v]
    set i [string first ":" $s]
    if {$i >= 0} { return [string trim [string range $s [expr {$i+1}] end]] }
    return $s
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

step "run"
con

step "wait for heartbeat"
set magic 0
for {set i 0} {$i < 20} {incr i} {
    after 500
    set magic [rd 0x00020000]
    if {[string match "*4F583338*" $magic]} { break }
}
puts "magic    = $magic"
puts "loop     = [rd 0x00020004]"
puts "pl_led   = [rd 0x00020008]"
puts "gt       = [rd 0x00020010]"
puts "uart_clk = [rd 0x00020018]   <== 期望非 0(自标定成功)"
puts "dirm0    = [rd 0x0002001C]"
puts "uartok   = [rd 0x00020020]   <== 期望 1"

set u [rd 0x00020020]
puts ""
if {[string match "*00000001" $u]} {
    puts ">>> UART 已被内核探测到 —— 请看串口终端是否出现启动横幅"
} else {
    puts ">>> 内核仍未探测到 UART"
}
exit 0
