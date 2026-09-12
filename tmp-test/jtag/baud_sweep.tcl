proc rd {a} {
    if {[catch {mrd -force $a} v]} { return -1 }
    set s [string trim $v]
    set i [string first ":" $s]
    if {$i >= 0} { set s [string trim [string range $s [expr {$i+1}] end]] }
    return [expr {"0x$s"}]
}
proc sendstr {s} {
    foreach ch [split $s ""] {
        set guard 0
        while {([rd 0xE000102C] & 0x10) != 0 && $guard < 200000} { incr guard }
        mwr -force 0xE0001030 [scan $ch %c]
    }
}
proc trybaud {gen div tag} {
    mwr -force 0xE0001018 $gen
    mwr -force 0xE0001034 $div
    mwr -force 0xE0001000 0x00000003
    mwr -force 0xE0001000 0x00000014
    after 100
    sendstr "\r\n\r\n==== MARKER $tag  GEN=$gen DIV=$div ====\r\n"
    after 600
}

connect
targets -set -filter {name =~ "ARM*#0"}
stop
puts "内核已暂停,下面只由 JTAG 驱动 UART"

# 候选:按不同参考时钟倒推 BAUDGEN
trybaud 457 0 "A-52.6MHz"
trybaud 731 0 "B-84.2MHz"
trybaud 124 0 "C-14.3MHz"
trybaud 609 0 "D-70.2MHz"
trybaud 548 0 "E-63.2MHz"
trybaud 868 0 "F-100MHz"

puts "扫描完成"
con
exit 0
