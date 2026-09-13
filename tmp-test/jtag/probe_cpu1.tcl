# 可靠地探查 CPU1 状态(第一版忘了先选目标,读回一片 ERR)。
#
# 目的:确认在"JTAG 直接加载、不跑 BootROM"的流程下,CPU1 到底停在哪儿,
# 以及 SLCR.A9_CPU_RST_CTRL 是什么状态。这决定了能否直接用
# UG585 §6.1.10 的 "写 0xFFFFFFF0 + SEV" 协议。

proc rd {a} {
    if {[catch {mrd -force $a} v]} { return "ERR" }
    set s [string trim $v]
    set i [string first ":" $s]
    if {$i >= 0} { return [string trim [string range $s [expr {$i + 1}] end]] }
    return $s
}
proc rdn {a} {
    set s [rd $a]
    if {$s eq "ERR"} { return -1 }
    if {[scan $s %x n] != 1} { return -1 }
    return $n
}
proc rr {reg} {
    if {[catch {rrd $reg} v]} { return "n/a" }
    set v [string trim $v]
    if {[llength $v] >= 2} { return [lindex $v 1] }
    return $v
}
proc hex32 {n} { return [format 0x%08X $n] }

# 必须先把目标选到 CPU0,否则 mrd 没有地址空间上下文,一律 ERR
connect
targets -set -filter {name =~ "ARM*#0"}

puts "=== SLCR A9_CPU_RST_CTRL (0xF8000244) 连读三次 ==="
for {set i 0} {$i < 3} {incr i} {
    set v [rdn 0xF8000244]
    puts "  [$i] raw=0x[format %08X $v]  RST0=[expr {($v >> 0) & 1}] RST1=[expr {($v >> 1) & 1}] CLKSTOP0=[expr {($v >> 2) & 1}] CLKSTOP1=[expr {($v >> 3) & 1}]"
    after 200
}

puts ""
puts "=== BootROM 的 CPU1 停机循环 (0xFFFFFFE0..0xFFFFFFF4) ==="
for {set a 0xFFFFFFE0} {$a <= 0xFFFFFFF4} {incr a 4} {
    puts "  [hex32 $a] = 0x[rd $a]"
}

puts ""
puts "=== 0xFFFFFFF0 就是 CPU1 的入口地址槽 ==="
puts "  值 = 0x[rd 0xFFFFFFF0]"

puts ""
puts "=== 两颗核的现场 ==="
foreach core {0 1} {
    if {[catch {targets -set -filter "name =~ \"ARM*#$core\""} e]} {
        puts "  CPU$core 选不到目标"
        continue
    }
    puts "  CPU$core  PC=0x[rr pc]  CPSR=0x[rr cpsr]  LR=0x[rr lr]"
}

targets -set -filter {name =~ "ARM*#0"}
puts ""
puts "CPUPROBE2_DONE"
exit 0
