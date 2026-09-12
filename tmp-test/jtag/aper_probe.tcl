proc rd {a} {
    if {[catch {mrd -force $a} v]} { return -1 }
    set s [string trim $v]
    set i [string first ":" $s]
    if {$i >= 0} { set s [string trim [string range $s [expr {$i+1}] end]] }
    return [expr {"0x$s"}]
}
connect
targets -set -filter {name =~ "ARM*#0"}

set aper [rd 0xF800012C]
puts [format "APER_CLK_CTRL (0xF800012C) = 0x%08X" $aper]
puts "  位含义: 0=GEM0 1=GEM1 2=USB0 3=USB1 4=SDI0 5=SDI1 6=SPI0 7=SPI1"
puts "          8=CAN0 9=CAN1 10=I2C0 11=I2C1 12=UART0 13=UART1 14=GPIO 15=LQSPI 16=SMC 17=DMA"
foreach {nm bit} {GEM0 0 GEM1 1 SDI0 4 SPI0 6 I2C0 10 UART0 12 UART1 13 GPIO 14 LQSPI 15 SMC 16 DMA 17} {
    puts [format "  %-6s APER_CLK bit%-2d = %d" $nm $bit [expr {($aper >> $bit) & 1}]]
}

puts ""
set u0 [rd 0xF8000154]
puts [format "UART_CLK_CTRL (0xF8000154) = 0x%08X  CLKACT=%d DIVISOR=%d SRCSEL=%d" \
    $u0 [expr {$u0 & 1}] [expr {($u0 >> 8) & 0x3F}] [expr {($u0 >> 4) & 3}]]

puts ""
puts "=== 对照:已知可工作的外设(PS GPIO)==="
puts [format "GPIO 0xE000A000 DATA_LSW = 0x%08X  (非 0 说明该外设已时钟使能)" [rd 0xE000A040]]

puts ""
puts "=== AXI GPIO(PL 侧,前面已验证可用)==="
puts [format "0x41200000 = 0x%08X" [rd 0x41200000]]
exit 0
