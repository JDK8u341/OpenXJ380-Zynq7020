# 判定 Zynq GPIO 的 DATA_RO 到底反映什么。
#
# 背景:上一次想用 DATA_RO 读 MIO49 焊盘电平,但作为对照的 MIO7/MIO8
# (已知 GPIO 引脚、内核在驱动)读数纹丝不动,对照失败 => 实验无效。
#
# 这次不依赖内核:直接从 JTAG 把 MIO7/MIO8 驱动成固定的低/高,
# 再读 DATA_RO 看是否跟随。跟随 => DATA_RO 可用,可以回去做焊盘实验;
# 不跟随 => DATA_RO 在这个配置下不可用,整个思路作废,不再纠缠。
#
# 同时读 DATA 寄存器(输出值)做交叉参考,分清"输出没写进去"与
# "写进去了但 DATA_RO 看不到"。

proc rd {a} {
    if {[catch {mrd -force $a} v]} { return "ERR" }
    set s [string trim $v]
    set i [string first ":" $s]
    if {$i >= 0} { return [string trim [string range $s [expr {$i + 1}] end]] }
    return $s
}
proc wr {a v} {
    if {[catch {mwr -force $a $v} e]} { return "ERR: $e" }
    return "OK"
}

set GPIO_DIRM0  0xE000A204
set GPIO_OEN0   0xE000A208
set GPIO_DATA0  0xE000A040
set GPIO_DRO0   0xE000A060
set GPIO_MD0LSW 0xE000A000
set LED_MASK    0x0180

connect
targets -set -filter {name =~ "ARM*#0"}

puts "=== 起点 ==="
puts "DIRM0   = 0x[rd $GPIO_DIRM0]   (0x180 = MIO7/MIO8 是输出)"
puts "OEN0    = 0x[rd $GPIO_OEN0]"
puts "DATA0   = 0x[rd $GPIO_DATA0]"
puts "DATA_RO0= 0x[rd $GPIO_DRO0]"

puts ""
puts "=== 强制把 MIO7/MIO8 拉低 ==="
puts "写 MASK_DATA_0_LSW = 0x01800000 : [wr $GPIO_MD0LSW 0x01800000]"
after 300
set lo_data [rd $GPIO_DATA0]
set lo_dro  [rd $GPIO_DRO0]
puts "DATA0   = 0x$lo_data"
puts "DATA_RO0= 0x$lo_dro"

puts ""
puts "=== 强制把 MIO7/MIO8 拉高 ==="
puts "写 MASK_DATA_0_LSW = 0x01800180 : [wr $GPIO_MD0LSW 0x01800180]"
after 300
set hi_data [rd $GPIO_DATA0]
set hi_dro  [rd $GPIO_DRO0]
puts "DATA0   = 0x$hi_data"
puts "DATA_RO0= 0x$hi_dro"

puts ""
puts "=== 判定 ==="
proc same {a b} { return [expr {[string equal [string trim $a] [string trim $b]] == 1}] }

if {[same $lo_data $hi_data]} {
    puts "DATA0 两次相同 -> MASK_DATA 写入没生效(或方向/使能没配对),本次实验无效"
} elseif {!([same $lo_dro $hi_dro])} {
    puts "DATA_RO0 跟随输出变化 -> **DATA_RO 可用**"
    puts "  下一步:重新采 MIO49 焊盘,并对 PC 发送的 0x00/0xFF 做对照"
} else {
    puts "DATA0 变了但 DATA_RO0 没变 -> **DATA_RO 在本配置下不反映焊盘**"
    puts "  结论:DATA_RO 这条路走不通,放弃该思路"
}

puts ""
puts "=== 恢复(交给内核下一轮自己重写)==="
puts "写 MASK_DATA_0_LSW = 0x01800180 : [wr $GPIO_MD0LSW 0x01800180]"
puts "PADTEST2_DONE"

exit 0
