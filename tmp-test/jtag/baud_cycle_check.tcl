proc rd {a} {
    if {[catch {mrd -force $a} v]} { return "ERR" }
    set s [string trim $v]
    set i [string first ":" $s]
    if {$i >= 0} { return [string trim [string range $s [expr {$i+1}] end]] }
    return $s
}
connect
targets -set -filter {name =~ "ARM*#0"}
puts "观察心跳槽 8（当前波特率）随时间变化:"
for {set i 0} {$i < 8} {incr i} {
    set v [rd 0x00020020]
    puts "  t+[expr {$i*2}]s : HB[8] = 0x$v"
    after 2000
}
puts ""
puts "=== UART1 寄存器 ==="
puts "CR      = 0x[rd 0xE0001000]"
puts "BAUDGEN = [rd 0xE0001018]"
puts "BAUDDIV = [rd 0xE0001034]"
exit 0
