# 故障注入:往选择器地址写码,触发内核的对应异常
# 用法: xsdb inject_fault.tcl <selector>
#   1 = Data Abort   2 = Undefined Instruction   3 = Prefetch Abort

set sel [lindex $argv 0]
if {$sel eq ""} { set sel 1 }

proc rd {a} {
    if {[catch {mrd -force $a} v]} { return "ERR" }
    return [string trim $v]
}

connect
targets -set -filter {name =~ "ARM*#0"}

puts "注入前:HB[0] magic = [rd 0x00020000]  (确认内核在跑)"
puts "注入:FAULT_SEL(0x00020080) <- $sel"
mwr -force 0x00020080 $sel

after 1500
puts "注入后:FAULT_SEL = [rd 0x00020080]  (内核应已把它清 0 并触发异常)"
puts "注入后:PC = [lindex [rrd pc] 1]"
exit 0
