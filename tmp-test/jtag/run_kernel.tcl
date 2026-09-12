# 加载并运行 ARM32 内核（M0 骨架）
#
# 流程: 复位 -> ps7_init -> ps7_post_config -> 比特流 -> 下载 -> 运行 -> 读心跳
#
# 用法: xsdb run_kernel.tcl

set BIT "C:/Users/VeryS/Documents/fpgap/zynqs/AXI_GPIO_1/AXI_GPIO_1_SOFT/platform/hw/sdt/System_wrapper.bit"
set PS7 "C:/Users/VeryS/Documents/fpgap/zynqs/AXI_GPIO_1/AXI_GPIO_1_SOFT/platform/hw/sdt/ps7_init.tcl"
set ELF "C:/Users/VeryS/Documents/others/OpenXJ380/out/kernel-arm.elf"

proc step {m} { puts "\n>>> $m" }
proc rd {a} {
    if {[catch {mrd -force $a} v]} { return "ERR" }
    return [string trim $v]
}

step "connect"
if {[catch {connect} e]} { puts "FAIL connect: $e"; exit 1 }
puts "OK"

step "reset system"
targets -set -filter {name =~ "APU"}
rst -system
after 2000
puts "OK"

step "ps7_init / ps7_post_config"
targets -set -filter {name =~ "APU"}
if {[catch {source $PS7} e]} { puts "FAIL source: $e"; exit 1 }
if {[catch {ps7_init} e]}    { puts "FAIL ps7_init: $e"; exit 1 }
if {[catch {ps7_post_config} e]} { puts "FAIL post_config: $e"; exit 1 }
puts "OK  LVL_SHFTR_EN = [rd 0xF8000900]"

step "load bitstream (PL: AXI GPIO -> LEDs)"
targets -set -filter {name =~ "xc7z020"}
if {[catch {fpga -f $BIT} e]} { puts "FAIL fpga: $e"; exit 1 }
puts "OK"

step "download kernel"
puts "ELF = $ELF"
targets -set -filter {name =~ "ARM*#0"}
rst -processor
if {[catch {dow $ELF} e]} { puts "FAIL dow: $e"; exit 1 }
puts "OK  PC = [lindex [rrd pc] 1]"

step "run"
con

# 等待内核跑起来。注意 kmain 里先做 LED 自检(约 300ms),
# 然后做 UART 时钟自标定(要发 1000 字节,几十毫秒),之后才打印横幅。
step "wait for heartbeat (OCM 0x00020000)"
set magic 0
for {set i 0} {$i < 20} {incr i} {
    after 500
    set magic [rd 0x00020000]
    if {[string match "*4F583338*" $magic]} { break }
}
puts "HEARTBEAT[0] magic     = $magic   (期望 4F583338 = \"OXJ8\")"
puts "HEARTBEAT[1] loop      = [rd 0x00020004]"
puts "HEARTBEAT[2] pl_led    = [rd 0x00020008]"
puts "HEARTBEAT[3] switches  = [rd 0x0002000C]"
puts "HEARTBEAT[4] gt        = [rd 0x00020010]"
puts "HEARTBEAT[5] ps_led    = [rd 0x00020014]"
puts "HEARTBEAT[6] uart_clk  = [rd 0x00020018]  <- UART 自标定结果(十六进制)"
puts "HEARTBEAT[7] dirm0     = [rd 0x0002001C]"

# 把 uart_clk 换算成十进制便于阅读
set u [rd 0x00020018]
set i [string first ":" $u]
if {$i >= 0} { set hex [string trim [string range $u [expr {$i+1}] end]] } else { set hex $u }
if {[catch {expr {"0x$hex"}} clk]} { set clk -1 }
puts ""
puts ">>> UART_REF_CLK 自标定 = $clk Hz  (约 [expr {$clk/1000000}] MHz)"
puts ">>> 请对照串口输出:若横幅可读,波特率配置正确"

if {[string match "*4F583338*" $magic]} {
    puts "\n>>> RESULT: KERNEL RUNNING"
} else {
    puts "\n>>> RESULT: 未检测到心跳"
}
exit 0
