proc rd {a} {
    if {[catch {mrd -force $a} v]} { return -1 }
    set s [string trim $v]
    set i [string first ":" $s]
    if {$i >= 0} { set s [string trim [string range $s [expr {$i+1}] end]] }
    return [expr {"0x$s"}]
}
connect
targets -set -filter {name =~ "ARM*#0"}
puts "=== PS 外设空间响应情况(读 ID/配置寄存器)==="
foreach {nm base off} {
    UART0   0xE0000000 0x04
    UART1   0xE0001000 0x04
    SPI0    0xE0006000 0x00
    SPI1    0xE0007000 0x00
    SDIO0   0xE0100000 0x00
    SDIO1   0xE0101000 0x00
    GEM0    0xE000B000 0x00
    GEM1    0xE000C000 0x00
    QSPI    0xE000D000 0x00
    I2C0    0xE0004000 0x00
    GPIO    0xE000A000 0x40
    SMC     0xE1000000 0x00
    SCU     0xF8F00000 0x00
    GIC_D   0xF8F01000 0x00
    GIC_C   0xF8F00100 0x00
    L2CC    0xF8F02000 0x00
    GTIMER  0xF8F00200 0x00
    XADC    0xF8007100 0x00
    SLCR    0xF8000000 0x00
} {
    set v [rd [expr {$base + $off}]]
    if {$v == 0} { set mark "读回 0 (未响应?)" } else { set mark "有响应" }
    puts [format "  %-8s 0x%08X = 0x%08X  %s" $nm $base $v $mark]
}
exit 0
