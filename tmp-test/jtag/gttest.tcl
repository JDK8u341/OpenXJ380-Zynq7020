proc rd {a} {
    if {[catch {mrd -force $a} v]} { return -1 }
    set s [string trim $v]
    set i [string first ":" $s]
    if {$i >= 0} { set s [string trim [string range $s [expr {$i+1}] end]] }
    return [expr {"0x$s"}]
}
connect
targets -set -filter {name =~ "ARM*#0"}
# 用 Tcl 自己的时钟做墙钟:读两次全局定时器,间隔 10 秒
set t0 [clock milliseconds]
set c0 [rd 0xF8F00200]
after 10000
set c1 [rd 0xF8F00200]
set t1 [clock milliseconds]
set dt [expr {$t1 - $t0}]
set dc [expr {$c1 - $c0}]
if {$dc < 0} { set dc [expr {$dc + 4294967296}] }
puts "墙钟间隔 = $dt ms"
puts "定时器增量 = $dc"
puts [format "实测全局定时器频率 = %.3f MHz" [expr {$dc / ($dt / 1000.0) / 1000000.0}]]
puts ""
puts "内核假定的 CPU 频率 = 666666687 Hz -> 定时器应为 333.333 MHz"
puts "若实测值与之不符,则内核所有计时(含 UART 标定)都会按比例偏掉"
exit 0
