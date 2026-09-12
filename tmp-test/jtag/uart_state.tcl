proc rd {a} {
    if {[catch {mrd -force $a} v]} { return "ERR" }
    return [string trim $v]
}
connect
targets -set -filter {name =~ "ARM*#0"}
puts "HB[8] UARTOK  = [rd 0x00020020]   (1 = 探测到 UART)"
puts "HB[6] UARTCLK  = [rd 0x00020018]"
puts ""
puts "=== 内核是否成功打开了 APER 时钟? ==="
puts "APER_CLK_CTRL = [rd 0xF800012C]"
puts "  (期望 bit12 或 bit13 被置起)"
puts ""
puts "=== UART0 寄存器 ==="
puts "CR      = [rd 0xE0000000]"
puts "MR      = [rd 0xE0000004]"
puts "SR      = [rd 0xE000002C]"
puts "BAUDGEN = [rd 0xE0000018]"
puts "BAUDDIV = [rd 0xE0000034]"
exit 0
