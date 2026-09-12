proc rd {a} {
    if {[catch {mrd -force $a} v]} { return "ERR" }
    set s [string trim $v]
    set i [string first ":" $s]
    if {$i >= 0} { return [string trim [string range $s [expr {$i+1}] end]] }
    return $s
}
connect
targets -set -filter {name =~ "ARM*#0"}
puts "=== GIC Distributor ==="
puts "  GICD_CTLR (使能)      = 0x[rd 0xF8F01000]"
puts "  GICD_ISENABLER0       = 0x[rd 0xF8F01100]   <- bit29 = 私有定时器"
puts "  GICD_IPRIORITYR[29]   = 0x[rd 0xF8F041D]"
puts ""
puts "=== GIC CPU Interface ==="
puts "  GICC_CTLR (使能)      = 0x[rd 0xF8F00100]"
puts "  GICC_PMR (优先级掩码) = 0x[rd 0xF8F00104]"
puts ""
puts "=== Cortex-A9 私有定时器 ==="
puts "  LOAD    = [rd 0xF8F00600]   <- 期望 333332 (1ms @333.33MHz)"
puts "  CONTROL = 0x[rd 0xF8F00608]   <- bit0=使能 bit1=自动重载 bit2=中断"
set c0 [rd 0xF8F00604]
after 300
set c1 [rd 0xF8F00604]
puts "  COUNTER = $c0 -> $c1"
if {$c0 ne $c1} { puts "  => 定时器在计数,中断在产生" } else { puts "  => 计数未变!" }
puts ""
puts "=== 内核心跳(含中断统计) ==="
foreach {i nm} {10 TICKS 11 IRQCOUNT} {
    set a [format "0x%08X" [expr {0x20000 + 4*$i}]]
    puts "  HB[$i] $nm = [rd $a]"
}
exit 0
