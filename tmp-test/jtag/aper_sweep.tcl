proc rd {a} {
    if {[catch {mrd -force $a} v]} { return -1 }
    set s [string trim $v]
    set i [string first ":" $s]
    if {$i >= 0} { set s [string trim [string range $s [expr {$i+1}] end]] }
    return [expr {"0x$s"}]
}
connect
targets -set -filter {name =~ "ARM*#0"}
mwr -force 0xF8000008 0x0000DF0D

puts "=== 基线 ==="
puts "APER = 0x[format %08X [rd 0xF800012C]]   UART1_MR = 0x[format %08X [rd 0xE0001004]]"

puts ""
puts "=== 试验 A: APER 全置 1 ==="
mwr -force 0xF800012C 0x03FFFFFF
after 200
puts "APER = 0x[format %08X [rd 0xF800012C]]   UART1_MR = 0x[format %08X [rd 0xE0001004]]   UART0_MR = 0x[format %08X [rd 0xE0000004]]"

puts ""
puts "=== 试验 B: 逐位扫描,找让 UART1_MR 非 0 的那一位 ==="
set base 0x01CC040D
for {set b 0} {$b < 26} {incr b} {
    mwr -force 0xF800012C [expr {$base | (1 << $b)}]
    after 30
    set mr [rd 0xE0001004]
    if {$mr != 0} {
        puts "  >>> bit $b 置起后 UART1_MR = 0x[format %08X $mr]  <== 就是这一位"
    }
}
puts "扫描结束"
mwr -force 0xF8000004 0x0000767B
exit 0
