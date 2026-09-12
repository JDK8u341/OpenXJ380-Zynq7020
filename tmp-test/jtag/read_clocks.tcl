# 读取 Zynq PS 时钟/PLL 配置，用于计算 UART 波特率分频
# 用法: xsdb read_clocks.tcl

proc rd {addr} {
    if {[catch {mrd -force $addr} v]} { return "ERR" }
    return [string trim $v]
}

connect
targets -set -filter {name =~ "APU"}
stop

puts "=== SLCR / PLL 寄存器 ==="
foreach {name addr} {
    ARM_PLL_CTRL      0xF8000100
    DDR_PLL_CTRL      0xF8000104
    IO_PLL_CTRL       0xF8000108
    PLL_STATUS        0xF800010C
    ARM_CLK_CTRL      0xF8000120
    DDR_CLK_CTRL      0xF8000128
    UART_CLK_CTRL     0xF8000154
    FPGA0_CLK_CTRL    0xF8000170
    FPGA1_CLK_CTRL    0xF8000174
    FPGA2_CLK_CTRL    0xF8000178
    FPGA3_CLK_CTRL    0xF800017C
    LVL_SHFTR_EN      0xF8000900
    SLCR_LOCK         0xF8000004
    BOOT_MODE         0xF800025C
} {
    puts [format "%-18s %s = %s" $name $addr [rd $addr]]
}

puts "=== PC / 当前状态 ==="
puts "PC0 = [rd 0xF8F00200]"
targets -set -filter {name =~ "ARM*#0"}
puts "CORE0_PC = [string trim [mrd -force 0x0]]"

exit 0
