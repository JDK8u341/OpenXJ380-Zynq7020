proc rd {a} {
    if {[catch {mrd -force $a} v]} { return "ERR" }
    return [string trim $v]
}
connect
targets -set -filter {name =~ "ARM*#0"}
puts "当前 PC = [lindex [rrd pc] 1]"
puts "等待 12 秒让内核完成初始化..."
after 12000
puts ""
puts "HB[0]  magic   = [rd 0x00020000]"
puts "HB[1]  loop    = [rd 0x00020004]"
puts "HB[4]  gt      = [rd 0x00020010]"
puts "HB[6]  uartclk = [rd 0x00020018]"
puts "HB[8]  uartok  = [rd 0x00020020]"
puts "HB[10] ticks   = [rd 0x00020028]"
puts "HB[11] irq     = [rd 0x0002002C]"
puts ""
puts "PC = [lindex [rrd pc] 1]"
exit 0
