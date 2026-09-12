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
puts "内核已停下"

# 用很慢的波特率,让 64 字节的排空时间足够长、便于从 PC 侧测量
set GEN 4096
set DIV 0

for {set run 1} {$run <= 3} {incr run} {
    # 复位 TX,清空 FIFO
    mwr -force 0xE0001000 0x00000003
    mwr -force 0xE0001000 0x00000014
    mwr -force 0xE0001018 $GEN
    mwr -force 0xE0001034 $DIV

    # 等发送器空闲
    set g 0
    while {([rd 0xE000102C] & 0x08) == 0 && $g < 100000} { incr g }

    # 记录开始时刻并灌满 FIFO
    set t0 [clock milliseconds]
    for {set i 0} {$i < 64} {incr i} {
        mwr -force 0xE0001030 0x55
    }
    # 等排空
    set g 0
    while {([rd 0xE000102C] & 0x08) == 0 && $g < 500000} { incr g }
    set t1 [clock milliseconds]

    set elapsed [expr {$t1 - $t0}]
    puts [format "第 %d 次: 64 字节排空耗时 %d ms" $run $elapsed]
}
exit 0
