proc rd {a} {
    if {[catch {mrd -force $a} v]} { return "ERR" }
    return [string trim $v]
}
connect
targets -set -filter {name =~ "ARM*#0"}
puts "=== 核心状态 ==="
puts "PC   = [lindex [rrd pc] 1]"
puts "CPSR = [lindex [rrd cpsr] 1]"
puts "LR   = [lindex [rrd lr] 1]"
puts ""
puts "=== 心跳全 9 槽 ==="
foreach {i nm} {0 MAGIC 1 LOOP 2 PL_LED 3 SW 4 GT 5 PS_LED 6 UARTCLK 7 DIRM0 8 UARTOK} {
    set a [format 0x%08X [expr {0x20000 + 4*$i}]]
    puts [format "HB[%d] %-8s %s = %s" $i $nm $a [rd $a]]
}
puts ""
puts "=== 内核代码区抽查 ==="
puts "0x100000 (_start)   = [rd 0x100000]"
puts "0x101000            = [rd 0x101000]"
puts ""
puts "=== 符号 kmain = 0x1007ac(上一次构建),看该处是否为函数序言 ==="
puts "0x1007ac = [rd 0x1007ac]"
puts "0x1007b0 = [rd 0x1007b0]"
exit 0
