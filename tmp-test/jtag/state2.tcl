# 只连、只读:调度器状态 + 心跳是否在推进。用法: xsdb state2.tcl
proc rd {a} {
    if {[catch {mrd -force $a} v]} { return -1 }
    set s [string trim $v]
    set i [string first ":" $s]
    if {$i >= 0} { set s [string trim [string range $s [expr {$i+1}] end]] }
    if {[scan $s %x n] != 1} { return -1 }
    return $n
}
proc hex {v} { return [format "0x%08X" [expr {$v & 0xFFFFFFFF}]] }

set T_CTX_SP   0x070
set T_CTX_PC   0x078
set T_CTX_CPSR 0x07C
set T_STAT     0x02C
set T_KSTACK   0x184
set T_KBASE    0x188
set T_KSLOT    0x190
set T_NEXT     0x19C

if {[catch {connect} e]} { puts "FAIL connect: $e"; exit 1 }
targets -set -filter {name =~ "ARM*#0"}

puts "=== 活性:ticks 两次采样 ==="
set t1 [rd 0x00020028]
after 400
set t2 [rd 0x00020028]
puts "ticks: $t1 -> $t2  ([expr {$t2 - $t1}] in 400ms)"

puts ""
puts "=== 调度器 ==="
puts "current_task   = [hex [rd 0x00172050]]"
puts "g_runq[0].head = [hex [rd 0x001720A0]]"
puts "g_runq[0].cnt  = [rd 0x001720A4]"
puts "g_switch_count = [rd 0x001720B0]"
puts "g_sched_enabled= [rd 0x0010F948]"
puts "g_reloc_skip   = [rd 0x001722C8]"
puts "tick_switched  = [rd 0x001722C0]"
puts "tick_preempted = [rd 0x001722C4]"
puts "tick_invalid   = [rd 0x001722BC]"
puts "g_spin_ok      = [rd 0x00134528]"
puts "g_spin_sp_iso  = [rd 0x0013452C]"
puts "g_cpsr_kernel  = [rd 0x00134538]"
puts "g_cpsr_seen    = [hex [rd 0x0013453C]]"

puts ""
puts "=== g_spin[2] (0x001338C0) 每个 20 字节 ==="
foreach i {0 1} {
    set b [expr {0x001338C0 + $i * 20}]
    puts "  [$i] count=[rd $b] saw_other=[rd [expr {$b+4}]] sp_bad=[rd [expr {$b+8}]] done=[rd [expr {$b+12}]] other=[hex [rd [expr {$b+16}]]]"
}

puts ""
puts "=== 遍历就绪队列(最多 8 个) ==="
set n [rd 0x001720A0]
for {set k 0} {$k < 8 && $n > 0x100000} {incr k} {
    puts "  node $k @ [hex $n]: pc=[hex [rd [expr {$n+$T_CTX_PC}]]] sp=[hex [rd [expr {$n+$T_CTX_SP}]]] cpsr=[hex [rd [expr {$n+$T_CTX_CPSR}]]] status=[rd [expr {$n+$T_STAT}]] slot=[rd [expr {$n+$T_KSLOT}]] base=[hex [rd [expr {$n+$T_KBASE}]]] top=[hex [rd [expr {$n+$T_KSTACK}]]]"
    set n [rd [expr {$n+$T_NEXT}]]
    if {$k > 6} { break }
}

set cur [rd 0x00172050]
if {$cur > 0x100000 && $cur < 0x400000} {
    puts ""
    puts "=== current TCB [hex $cur] ==="
    puts "  pc=[hex [rd [expr {$cur+$T_CTX_PC}]]] sp=[hex [rd [expr {$cur+$T_CTX_SP}]]] cpsr=[hex [rd [expr {$cur+$T_CTX_CPSR}]]] status=[rd [expr {$cur+$T_STAT}]] slot=[rd [expr {$cur+$T_KSLOT}]]"
    puts "  name bytes:"
    set nm ""
    for {set o 0x0C} {$o < 0x2C} {incr o 4} {
        set w [rd [expr {$cur+$o}]]
        foreach sh {0 8 16 24} {
            set c [expr {($w >> $sh) & 0xFF}]
            if {$c == 0} { break }
            append nm [format %c $c]
        }
        if {[string length $nm] > 0 && [string index $nm end] == "\x00"} { break }
    }
    puts "    '$nm'"
}
exit 0
