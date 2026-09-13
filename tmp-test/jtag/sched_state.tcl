# 数据中止之后的调度器状态:读全局量,推断"切到 acc 时用的 sp 是多少"。
# 用法: xsdb sched_state.tcl
proc rd {a} {
    if {[catch {mrd -force $a} v]} { return -1 }
    set s [string trim $v]
    set i [string first ":" $s]
    if {$i >= 0} { set s [string trim [string range $s [expr {$i+1}] end]] }
    if {[scan $s %x n] != 1} { return -1 }
    return $n
}
proc hex {v} { return [format "0x%08X" [expr {$v & 0xFFFFFFFF}]] }

# TCB 偏移(由结构体布局算出,已用反汇编 [r4,#464] -> eevdf_vruntime=0x1D0 核对)
set T_CTX_SP   0x070
set T_CTX_LR   0x074
set T_CTX_PC   0x078
set T_CTX_CPSR 0x07C
set T_KSTACK   0x184
set T_KBASE    0x188
set T_KGUARD   0x18C
set T_KSLOT    0x190
set T_OWNS     0x194
set T_NEXT     0x19C

if {[catch {connect} e]} { puts "FAIL connect: $e"; exit 1 }
targets -set -filter {name =~ "ARM*#0"}

puts "=== 调度器全局量 ==="
set cur [rd 0x00172050]
puts "g_percpu[0].current_task = [hex $cur]"
puts "g_runq[0].head           = [hex [rd 0x001720A0]]"
puts "g_runq[0].count          = [rd 0x001720A4]"
puts "g_switch_count           = [rd 0x001720B0]"
puts "g_sched_enabled          = [rd 0x0010F7B0]"
puts "g_reloc_skip             = [rd 0x001722C8]"
puts "g_tick_switched          = [rd 0x001722C0]"
puts "g_tick_preempted         = [rd 0x001722C4]"
puts "g_tick_invalid_ctx       = [rd 0x001722BC]"
puts "g_spin_ok                = [rd 0x00134528]"
puts "g_spin_sp_isolated       = [rd 0x0013452C]"
puts "g_tick_acc_ok            = [rd 0x00134508]"

puts ""
puts "=== idle TCB (0x001720B8) ==="
set ib 0x001720B8
puts "  ctx.sp   = [hex [rd [expr {$ib + $T_CTX_SP}]]]"
puts "  ctx.pc   = [hex [rd [expr {$ib + $T_CTX_PC}]]]"
puts "  ctx.cpsr = [hex [rd [expr {$ib + $T_CTX_CPSR}]]]"
puts "  kernel_stack = [hex [rd [expr {$ib + $T_KSTACK}]]]"
puts "  kstack_base  = [hex [rd [expr {$ib + $T_KBASE}]]]"

if {$cur > 0x100000 && $cur < 0x00400000} {
    puts ""
    puts "=== acc TCB ([hex $cur]) ==="
    puts "  ctx.r0   = [hex [rd [expr {$cur + 0x3C}]]]"
    puts "  ctx.sp   = [hex [rd [expr {$cur + $T_CTX_SP}]]]"
    puts "  ctx.lr   = [hex [rd [expr {$cur + $T_CTX_LR}]]]"
    puts "  ctx.pc   = [hex [rd [expr {$cur + $T_CTX_PC}]]]"
    puts "  ctx.cpsr = [hex [rd [expr {$cur + $T_CTX_CPSR}]]]"
    puts "  kernel_stack = [hex [rd [expr {$cur + $T_KSTACK}]]]"
    puts "  kstack_base  = [hex [rd [expr {$cur + $T_KBASE}]]]"
    puts "  kstack_guard = [hex [rd [expr {$cur + $T_KGUARD}]]]"
    puts "  kstack_slot  = [rd [expr {$cur + $T_KSLOT}]]"
    puts "  owns_kstack  = [rd [expr {$cur + $T_OWNS}]]"
    puts "  sched_next   = [hex [rd [expr {$cur + $T_NEXT}]]]"
    puts ""
    puts "  === 栈顶附近的内容(期望帧的 ret/sp  槽) ==="
    set sp [rd [expr {$cur + $T_CTX_SP}]]
    for {set o -80} {$o < 16} {incr o 4} {
        puts [format "    [%s] = %s" [hex [expr {$sp + $o}]] [hex [rd [expr {$sp + $o}]]]]
    }
}
exit 0
