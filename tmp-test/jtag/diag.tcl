# 诊断：程序下载后为什么没跑起来
# 不复位，直接查看核心状态与内存内容

proc rd {a} {
    if {[catch {mrd -force $a} v]} { return "ERR" }
    return [string trim $v]
}
proc rreg {r} {
    if {[catch {rrd $r} v]} { return "ERR" }
    return [string trim $v]
}

connect

puts "=== 核心状态 ==="
targets -set -filter {name =~ "ARM*#0"}
puts "PC    = [rreg pc]"
puts "CPSR  = [rreg cpsr]"
puts "SCTLR = [rreg sctlr]"
puts "DFSR  = [rreg dfsr]"
puts "DFAR  = [rreg dfar]"
puts "IFSR  = [rreg ifsr]"
puts "IFAR  = [rreg ifar]"

puts "\n=== 内存内容 ==="
puts "0x00100000 (程序入口, 期望 E59FD0?? / 我们的代码) = [rd 0x00100000]"
puts "0x00100004 = [rd 0x00100004]"
puts "0x00020000 (心跳区, 期望 0 或 magic) = [rd 0x00020000]"

puts "\n=== OCM / 地址映射配置 ==="
puts "OCM_CFG (0xF8000910) = [rd 0xF8000910]"
puts "LVL_SHFTR_EN (0xF8000900) = [rd 0xF8000900]"
puts "FPGA0_CLK_CTRL (0xF8000170) = [rd 0xF8000170]"
puts "A9_CPU_RST_CTRL? (0xF8000244) = [rd 0xF8000244]"

puts "\n=== AXI GPIO 当前值 ==="
puts "ch1 DATA (0x41200000) = [rd 0x41200000]"
puts "ch1 TRI  (0x41200004) = [rd 0x41200004]"
puts "ch2 DATA (0x41200008) = [rd 0x41200008]"
puts "ch2 TRI  (0x4120000C) = [rd 0x4120000C]"

exit 0
