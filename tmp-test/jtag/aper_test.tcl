proc rd {a} {
    if {[catch {mrd -force $a} v]} { return "ERR" }
    set s [string trim $v]
    set i [string first ":" $s]
    if {$i >= 0} { set s [string trim [string range $s [expr {$i+1}] end]] }
    return [expr {"0x$s"}]
}
connect
targets -set -filter {name =~ "ARM*#0"}

puts "=== 步骤 1: 解锁 SLCR ==="
mwr -force 0xF8000008 0x0000DF0D
puts "SLCR_LOCK  = 0x[format %08X [rd 0xF8000004]]  (0=已解锁)"
puts "解锁前 APER = 0x[format %08X [rd 0xF800012C]]"

puts ""
puts "=== 步骤 2: 置位 UART0/UART1 的 APER 时钟 ==="
set aper [rd 0xF800012C]
mwr -force 0xF800012C [expr {$aper | 0xC00}]
after 100
puts "写入后 APER = 0x[format %08X [rd 0xF800012C]]"

puts ""
puts "=== 步骤 3: UART 是否开始响应 ==="
puts "UART0 CR = 0x[format %08X [rd 0xE0000000]]"
puts "UART0 SR = 0x[format %08X [rd 0xE000002C]]"
puts "UART1 CR = 0x[format %08X [rd 0xE0001000]]"

puts ""
puts "=== 步骤 4: 重新锁定 SLCR ==="
mwr -force 0xF8000004 0x0000767B
puts "SLCR_LOCK  = 0x[format %08X [rd 0xF8000004]]"
exit 0
