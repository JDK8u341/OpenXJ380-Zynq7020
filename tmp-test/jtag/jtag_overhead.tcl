proc rd {a} {
    if {[catch {mrd -force $a} v]} { return -1 }
    set s [string trim $v]
    set i [string first ":" $s]
    if {$i >= 0} { set s [string trim [string range $s [expr {$i+1}] end]] }
    return [expr {"0x$s"}]
}
connect
targets -set -filter {name =~ "ARM*#0"}
stop
after 200

# 1) 测 JTAG 写 64 次的纯开销:TX 关闭,写进去也不会发出去
mwr -force 0xE0001000 0x00000020
set t0 [clock milliseconds]
for {set i 0} {$i < 64} {incr i} { mwr -force 0xE0001030 0x55 }
set t1 [clock milliseconds]
puts [format "JTAG 写 64 次的纯开销 = %d ms" [expr {$t1 - $t0}]]

# 2) 测 mrd 轮询一次的往返开销
set t0 [clock milliseconds]
for {set i 0} {$i < 64} {incr i} { rd 0xE000102C }
set t1 [clock milliseconds]
puts [format "JTAG 读 64 次的纯开销 = %d ms" [expr {$t1 - $t0}]]
exit 0
