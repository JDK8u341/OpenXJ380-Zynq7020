proc rd {a} {
    if {[catch {mrd -force $a} v]} { return -1 }
    set s [string trim $v]
    set i [string first ":" $s]
    if {$i >= 0} { set s [string trim [string range $s [expr {$i+1}] end]] }
    return [expr {"0x$s"}]
}
proc sendn {ch n} {
    set guard 0
    while {([rd 0xE000102C] & 0x10) != 0 && $guard < 500000} { incr guard }
    for {set i 0} {$i < $n} {incr i} {
        set g2 0
        while {([rd 0xE000102C] & 0x10) != 0 && $g2 < 500000} { incr g2 }
        mwr -force 0xE0001030 [scan $ch %c]
    }
}
proc trygen {gen ch} {
    # 复位 -> 设分频 -> 使能 TX+RX(关键:0x14,别写成 0x20 那是 TXDIS)
    mwr -force 0xE0001000 0x00000003
    mwr -force 0xE0001004 0x00000020
    mwr -force 0xE0001018 $gen
    mwr -force 0xE0001034 0
    mwr -force 0xE0001000 0x00000014
    after 200
    sendn $ch 120
}

connect
targets -set -filter {name =~ "ARM*#0"}
stop
after 300
puts "已停下内核"

trygen 124 "A"
after 400
trygen 457 "B"
after 400
trygen 731 "C"
after 400
trygen 868 "D"
after 400
trygen 609 "E"
after 400
trygen 548 "F"
after 400
puts "扫描完成: A=124(14.3M) B=457(52.6M) C=731(84.2M) D=868(100M) E=609(70.2M) F=548(63.2M)"
exit 0
