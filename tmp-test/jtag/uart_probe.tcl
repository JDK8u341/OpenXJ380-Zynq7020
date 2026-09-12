proc rd {a} {
    if {[catch {mrd -force $a} v]} { return -1 }
    set s [string trim $v]
    set i [string first ":" $s]
    if {$i >= 0} { set s [string trim [string range $s [expr {$i+1}] end]] }
    return [expr {"0x$s"}]
}
connect
targets -set -filter {name =~ "ARM*#0"}
puts "=== UART 候选引脚的 MIO 配置 ==="
puts "(L3_SEL 非 0 表示分配给外设;UART0/UART1 的 L3_SEL 通常为 4)"
foreach n {14 15 18 19 46 47 48 49} {
    set v [rd [expr {0xF8000700 + 4*$n}]]
    set l3 [expr {($v >> 11) & 0x7}]
    set l2 [expr {($v >> 6) & 0x7}]
    set l1 [expr {($v >> 3) & 0x7}]
    set l0 [expr {$v & 0x7}]
    puts [format "MIO%-3d = 0x%08X  L0=%d L1=%d L2=%d L3=%d %s" $n $v $l0 $l1 $l2 $l3 \
        [expr {$l3 != 0 ? "<- 分配给外设" : ""}]]
}
puts ""
puts "=== 两个 UART 的当前状态 ==="
foreach {name base} {UART0 0xE0000000 UART1 0xE0001000} {
    set cr [rd $base]
    set sr [rd [expr {$base + 0x2C}]]
    set bg [rd [expr {$base + 0x18}]]
    set bd [rd [expr {$base + 0x34}]]
    puts [format "%-6s CR=0x%08X SR=0x%08X BAUDGEN=%d BAUDDIV=%d" $name $cr $sr $bg $bd]
}
exit 0
