# 读取 ps7_init 之后的时钟配置，用于计算 UART_REF_CLK
proc rd {a} {
    if {[catch {mrd -force $a} v]} { return -1 }
    set s [string trim $v]
    set i [string first ":" $s]
    if {$i >= 0} { set s [string trim [string range $s [expr {$i+1}] end]] }
    return [expr {"0x$s"}]
}

connect
targets -set -filter {name =~ "ARM*#0"}

set arm_pll  [rd 0xF8000100]
set ddr_pll  [rd 0xF8000104]
set io_pll   [rd 0xF8000108]
set pll_sts  [rd 0xF800010C]
set arm_clk  [rd 0xF8000120]
set uart_clk [rd 0xF8000154]
set fclk0    [rd 0xF8000170]

puts [format "ARM_PLL_CTRL  = 0x%08X  FBDIV=%d FBIDIV=%d" $arm_pll [expr {$arm_pll & 0xFFF}] [expr {($arm_pll >> 12) & 0x7F}]]
puts [format "IO_PLL_CTRL   = 0x%08X  FBDIV=%d FBIDIV=%d" $io_pll  [expr {$io_pll  & 0xFFF}] [expr {($io_pll  >> 12) & 0x7F}]]
puts [format "DDR_PLL_CTRL  = 0x%08X" $ddr_pll]
puts [format "PLL_STATUS    = 0x%08X" $pll_sts]
puts [format "ARM_CLK_CTRL  = 0x%08X  DIVISOR=%d SRCSEL=%d" $arm_clk  [expr {($arm_clk  >> 8) & 0x3F}] [expr {($arm_clk  >> 4) & 0x3}]]
puts [format "UART_CLK_CTRL = 0x%08X  DIVISOR=%d SRCSEL=%d CLKACT=%d" $uart_clk [expr {($uart_clk >> 8) & 0x3F}] [expr {($uart_clk >> 4) & 0x3}] [expr {$uart_clk & 1}]]
puts [format "FPGA0_CLK_CTRL= 0x%08X  DIVISOR0=%d DIVISOR1=%d SRCSEL=%d CLKACT=%d" $fclk0 [expr {($fclk0 >> 8) & 0x3F}] [expr {($fclk0 >> 20) & 0x3F}] [expr {($fclk0 >> 4) & 0x3}] [expr {$fclk0 & 1}]]

# Zynq: Fref = 33.3333 MHz
set fref 33.333333
foreach {name pll} [list ARM $arm_pll IO $io_pll] {
    set fbdiv  [expr {$pll & 0xFFF}]
    set fbidiv [expr {($pll >> 12) & 0x7F}]
    if {$fbidiv == 0} { set fbidiv 1 }
    set f [expr {$fref * $fbdiv / $fbidiv}]
    puts [format "%s PLL 频率 = %.3f MHz" $name $f]
}

exit 0
