# 走过整个就绪队列,把名字/状态打出来。用法: xsdb walkq.tcl
proc rd {a} {
    if {[catch {mrd -force $a} v]} { return -1 }
    set s [string trim $v]
    set i [string first ":" $s]
    if {$i >= 0} { set s [string trim [string range $s [expr {$i+1}] end]] }
    if {[scan $s %x n] != 1} { return -1 }
    return $n
}
proc hex {v} { return [format "0x%08X" [expr {$v & 0xFFFFFFFF}]] }
proc name {t} {
    set nm ""
    for {set o 0x0C} {$o < 0x2C} {incr o 4} {
        set w [rd [expr {$t + $o}]]
        for {set sh 0} {$sh < 32} {incr sh 8} {
            set c [expr {($w >> $sh) & 0xFF}]
            if {$c == 0} { return $nm }
            append nm [format %c $c]
        }
    }
    return $nm
}

if {[catch {connect} e]} { puts "FAIL connect: $e"; exit 1 }
targets -set -filter {name =~ "ARM*#0"}

set H 0x001760B8
set cur [rd 0x00176050]
puts "current = [hex $cur]  status=[rd [expr {$cur + 0x2C}]] name='[name $cur]'"
puts "runq.head = [hex [rd $H]]  count = [rd [expr {$H + 4}]]"
puts "tick_switched  = [rd 0x001762D8]"
puts "tick_preempted = [rd 0x001762DC]"
puts "tick_invalid   = [rd 0x001762D4]"
puts "g_vfp_skip     = [rd 0x00110CB0]"
puts "g_reloc_skip   = [rd 0x001762E0]"
puts "g_wake_skip    = [rd 0x001760A8]"
puts "sched_enabled  = [rd 0x00110E78]"
puts ""
puts "=== 队列(最多 20 个) ==="
set n [rd $H]
for {set k 0} {$k < 20 && $n > 0x1000} {incr k} {
    set st [rd [expr {$n + 0x2C}]]
    set wt [rd [expr {$n + 0x30}]]
    puts [format "  %2d @ %s '%-6s' status=%u wak=%u pc=%s sp=%s slot=%u next=%s" \
        $k [hex $n] [name $n] $st $wt [hex [rd [expr {$n+0x78}]]] \
        [hex [rd [expr {$n+0x70}]]] [rd [expr {$n+0x190}]] [hex [rd [expr {$n+0x19C}]]]]
    set n [rd [expr {$n + 0x19C}]]
}
exit 0
