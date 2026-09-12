proc rd {a} {
    if {[catch {mrd -force $a} v]} { return -1 }
    set s [string trim $v]
    set i [string first ":" $s]
    if {$i >= 0} { set s [string trim [string range $s [expr {$i+1}] end]] }
    return [expr {"0x$s"}]
}
connect
targets -set -filter {name =~ "ARM*#0"}

puts "=== 修改前 ==="
puts "MIO_PIN_48 = 0x[format %08X [rd 0xF80007C0]]"
puts "MIO_PIN_49 = 0x[format %08X [rd 0xF80007C4]]"
puts "UART1 MR   = 0x[format %08X [rd 0xE0001004]]"
puts "UART1 CR   = 0x[format %08X [rd 0xE0001000]]"

puts ""
puts "=== 应用 SPA 工程的 UART1 引脚配置 ==="
mwr -force 0xF8000008 0x0000DF0D
mwr -force 0xF80007C0 0x000012E0
mwr -force 0xF80007C4 0x000012E1
set aper [rd 0xF800012C]
mwr -force 0xF800012C [expr {$aper | 0x2000}]
after 200

puts "MIO_PIN_48 = 0x[format %08X [rd 0xF80007C0]]"
puts "MIO_PIN_49 = 0x[format %08X [rd 0xF80007C4]]"
puts "APER_CLK   = 0x[format %08X [rd 0xF800012C]]"
puts "UART_CLK   = 0x[format %08X [rd 0xF8000154]]"

puts ""
puts "=== 修改后:UART1 是否活过来了 ==="
puts "UART1 CR   = 0x[format %08X [rd 0xE0001000]]"
puts "UART1 MR   = 0x[format %08X [rd 0xE0001004]]"
puts "UART1 SR   = 0x[format %08X [rd 0xE000102C]]"
puts "(MR 读到 0x20 就说明外设已响应)"
exit 0
