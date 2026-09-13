# 数据中止后的现场:只连、只读,不复位。
# 用法: xsdb dump_state.tcl
proc rr {reg} {
    if {[catch {rrd $reg} v]} { return "n/a" }
    set v [string trim $v]
    if {[llength $v] >= 2} { return [lindex $v 1] }
    return $v
}
proc rd {a} {
    if {[catch {mrd -force $a} v]} { return "ERR" }
    set s [string trim $v]
    set i [string first ":" $s]
    if {$i >= 0} { return [string trim [string range $s [expr {$i+1}] end]] }
    return $s
}

if {[catch {connect} e]} { puts "FAIL connect: $e"; exit 1 }
targets -set -filter {name =~ "ARM*#0"}

puts "=== ARM core registers ==="
foreach r {pc cpsr spsr lr r0 r1 r2 r3 r4 r5 r6 r7 r8 r9 r10 r11 r12 sp_usr sp_svc sp_irq sp_abt sp_und sp_fiq} {
    puts [format "%-8s = %s" $r [rr $r]]
}
puts "dfar     = [rr dfar]"
puts "dfsr     = [rr dfsr]"
puts "ifar     = [rr ifar]"
puts "ifsr     = [rr ifsr]"
puts "dfar_bank? (mrc p15 c6 c0 0) = [rr dfar]"
puts "TTBR0    = [rr ttbr0]"
puts "SCTLR    = [rr sctlr]"
puts "DACR     = [rr dacr]"

puts ""
puts "=== 反查:sched 相关全局量 ==="
# 从 ELF 的符号表拿到地址(由调用方在命令行给出更方便,这里只 dump 已知区)
puts "heartbeat:"
foreach a {0x00020000 0x00020028 0x0002002C 0x00020040} {
    puts [format "  [0x%08X] = %s" $a [rd $a]]
}
exit 0
