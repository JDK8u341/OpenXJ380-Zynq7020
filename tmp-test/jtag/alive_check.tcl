proc rd {a} {
    if {[catch {mrd -force $a} v]} { return "ERR" }
    return [string trim $v]
}
connect
targets -set -filter {name =~ "ARM*#0"}
puts "PC    = [lindex [rrd pc] 1]"
puts "loop  = [rd 0x00020004]"
puts "gt    = [rd 0x00020010]"
puts "uartok= [rd 0x00020020]"
puts "UART1 SR = [rd 0xE000102C]  (bit3=TXEMPTY)"
exit 0
