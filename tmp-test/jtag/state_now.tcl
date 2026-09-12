proc rd {a} {
    if {[catch {mrd -force $a} v]} { return "ERR" }
    return [string trim $v]
}
connect
targets -set -filter {name =~ "ARM*#0"}
puts "PC        = [lindex [rrd pc] 1]"
puts "magic     = [rd 0x00020000]   (4F583338 = 我的内核)"
puts "loop      = [rd 0x00020004]"
puts "pl_led    = [rd 0x00020008]"
puts "uartok    = [rd 0x00020020]"
puts "UART1 MR  = [rd 0xE0001004]"
puts "MIO48     = [rd 0xF80007C0]"
exit 0
