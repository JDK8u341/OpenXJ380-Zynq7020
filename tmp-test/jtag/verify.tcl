# 验证程序确实在运行：对比两次采样
proc rd {a} {
    if {[catch {mrd -force $a} v]} { return "ERR" }
    return [string trim $v]
}
proc val {s} { return [lindex $s 1] }

connect
targets -set -filter {name =~ "ARM*#0"}

puts "=== 第 1 次采样（运行中）==="
stop
set t1   [val [rd 0x00020004]]
set led1 [val [rd 0x00020008]]
set sw1  [val [rd 0x0002000C]]
set gt1  [val [rd 0x00020010]]
set gd1  [val [rd 0x41200008]]
puts "tick=$t1  led=$led1  sw=$sw1  gt=$gt1  gpio_ch2=$gd1"
con

after 2000

puts "\n=== 第 2 次采样（运行 2 秒后）==="
stop
set t2   [val [rd 0x00020004]]
set led2 [val [rd 0x00020008]]
set sw2  [val [rd 0x0002000C]]
set gt2  [val [rd 0x00020010]]
set gd2  [val [rd 0x41200008]]
puts "tick=$t2  led=$led2  sw=$sw2  gt=$gt2  gpio_ch2=$gd2"
con

puts "\n=== 判定 ==="
puts "tick: $t1 -> $t2"
puts "gt  : $gt1 -> $gt2"
if {$t1 ne $t2} {
    puts "RESULT: RUNNING  (tick 在推进)"
} else {
    puts "RESULT: STALLED  (tick 未变化)"
}

exit 0
