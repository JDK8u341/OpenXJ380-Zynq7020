# A/B 对照组之后的累计调度计数。用法: xsdb counters.tcl
proc rd {a} {
    if {[catch {mrd -force $a} v]} { return -1 }
    set s [string trim $v]
    set i [string first ":" $s]
    if {$i >= 0} { set s [string trim [string range $s [expr {$i+1}] end]] }
    if {[scan $s %x n] != 1} { return -1 }
    return $n
}
proc hex {v} { return [format "0x%08X" [expr {$v & 0xFFFFFFFF}]] }
if {[catch {connect} e]} { puts "FAIL connect: $e"; exit 1 }
targets -set -filter {name =~ "ARM*#0"}
puts "g_tick_switched   = [rd 0x001722C0]"
puts "g_tick_preempted  = [rd 0x001722C4]"
puts "g_tick_invalid    = [rd 0x001722BC]"
puts "g_switch_count    = [rd 0x001720B0]"
puts "g_reloc_skip      = [rd 0x001722C8]"
puts "g_sched_enabled   = [rd 0x0010F948]"
puts "current_task      = [hex [rd 0x00172050]]"
puts "g_runq[0].head    = [hex [rd 0x001720A0]]"
puts "g_runq[0].count   = [rd 0x001720A4]"
puts "g_boot_idle.ctxpc = [hex [rd 0x001720B8+0x78]]"
puts "g_reloc_ctl_det   = [rd 0x00134540]"
exit 0
