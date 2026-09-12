# 用已知频率的外设反推 PLL 频率，进而定标 UART_REF_CLK
proc rd {a} {
    if {[catch {mrd -force $a} v]} { return -1 }
    set s [string trim $v]
    set i [string first ":" $s]
    if {$i >= 0} { set s [string trim [string range $s [expr {$i+1}] end]] }
    return [expr {"0x$s"}]
}

connect
targets -set -filter {name =~ "ARM*#0"}

set regs {
    GEM0_CLK_CTRL   0xF8000140
    GEM1_CLK_CTRL   0xF8000144
    SMC_CLK_CTRL    0xF8000148
    LQSPI_CLK_CTRL  0xF800014C
    SDIO_CLK_CTRL   0xF8000150
    UART_CLK_CTRL   0xF8000154
    SPI_CLK_CTRL    0xF8000158
    PCAP_CLK_CTRL   0xF8000164
}
set srcname {IO_PLL ARM_PLL DDR_PLL}

foreach {name addr} $regs {
    set v [rd $addr]
    set div [expr {($v >> 8) & 0x3F}]
    set src [expr {($v >> 4) & 0x3}]
    set act [expr {$v & 1}]
    puts [format "%-15s = 0x%08X  DIVISOR=%-3d (/%d)  SRCSEL=%d(%s)  CLKACT=%d" \
        $name $v $div [expr {$div+1}] $src [lindex $srcname $src] $act]
}

puts "\n=== 用已知频率反推源 PLL ==="
# SDIO 已知 100MHz;BSP: XPAR_SDHCI0_SDIO_CLK_FREQ_HZ = 100000000
set sdio [rd 0xF8000150]
set sdiv [expr {(($sdio >> 8) & 0x3F) + 1}]
puts [format "SDIO : 已知 100.000 MHz, DIVISOR 分频 /%d  -> 源 PLL = %.3f MHz" $sdiv [expr {100.0 * $sdiv}]]

# LQSPI 已知 200MHz;BSP: XPAR_QSPI_CLOCK_FREQ = 0xbebc200 = 200000000
set lqspi [rd 0xF800014C]
set ldiv [expr {(($lqspi >> 8) & 0x3F) + 1}]
puts [format "LQSPI: 已知 200.000 MHz, DIVISOR 分频 /%d  -> 源 PLL = %.3f MHz" $ldiv [expr {200.0 * $ldiv}]]

# 由此推算 UART
set uart [rd 0xF8000154]
set udiv [expr {(($uart >> 8) & 0x3F) + 1}]
set usrc [expr {($uart >> 4) & 0x3}]
puts ""
puts [format "UART : SRCSEL=%d, DIVISOR /%d" $usrc $udiv]
set pll_guess [expr {100.0 * $sdiv}]
puts [format "若 UART 源同为该 PLL:UART_REF_CLK = %.3f MHz" [expr {$pll_guess / $udiv}]]

exit 0
