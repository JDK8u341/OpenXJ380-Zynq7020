proc rd {a} {
    if {[catch {mrd -force $a} v]} { return -1 }
    set s [string trim $v]
    set i [string first ":" $s]
    if {$i >= 0} { set s [string trim [string range $s [expr {$i+1}] end]] }
    return [expr {"0x$s"}]
}
connect
targets -set -filter {name =~ "ARM*#0"}
puts "=== 内核运行后的 UART1 寄存器 ==="
puts [format "CR      = 0x%08X   (bit4=TXEN should be 1)" [rd 0xE0001000]]
puts [format "MR      = 0x%08X" [rd 0xE0001004]]
puts [format "BAUDGEN = %d" [rd 0xE0001018]]
puts [format "BAUDDIV = %d" [rd 0xE0001034]]
puts [format "SR      = 0x%08X" [rd 0xE000102C]]
puts ""
puts "=== 从 JTAG 直接往 TX FIFO 发一串字符(绕过内核) ==="
set msg "JTAG-UART-TEST-ABC-12345\r\n"
foreach ch [split $msg ""] {
    set sr [rd 0xE000102C]
    set guard 0
    while {($sr & 0x10) != 0 && $guard < 100000} {
        set sr [rd 0xE000102C]
        incr guard
    }
    mwr -force 0xE0001030 [scan $ch %c]
}
after 200
puts "已发送: [string trim $msg]"
puts [format "发送后 SR = 0x%08X   (bit3=TXEMPTY 应为 1)" [rd 0xE000102C]]
exit 0
