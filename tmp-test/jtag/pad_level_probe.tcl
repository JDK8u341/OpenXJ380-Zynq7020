# 直接读 MIO49 焊盘电平 —— 判定 PC->板 方向到底断在哪里。
#
# 上一次尝试失败的原因:引脚复用给 UART1 之后,GPIO 的 DATA_RO 看不到焊盘
# (实测 MIO48/49 都恒读 0,而 MIO48 明明在被板子驱动)。所以这次先把
# MIO_PIN_49 临时改成 GPIO,读完再改回 UART1。
#
# 对照很重要:MIO7/MIO8 是已知的 GPIO 引脚,内核正在上面跑跑马灯,
# 所以 bank0 的 DATA_RO bit7/bit8 必须随心跳里的 led 值变化。
# 对照不成立 => 本次实验无效,不能得出任何结论。
#
# 寄存器偏移来源:
#   gpiops_v3_14/src/xgpiops_hw.h  DATA_RO=0x60, bank 步进 0x04
#     bank0(MIO0-31)  = 0xE000A060
#     bank1(MIO32-53) = 0xE000A064   bit16=MIO48  bit17=MIO49
#   ps7_init 里 MIO7/MIO8 = 0x0600 (GPIO),MIO48/49 = 0x16E0/0x16E1 (UART1)
#   MIO_PIN_xx 属 SLCR,写入前必须解锁。

proc now {} {
    if {[catch {clock milliseconds} t]} { return [expr {[clock seconds] * 1000}] }
    return $t
}

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

set SLCR_UNLOCK 0xF8000008
set SLCR_LOCK   0xF8000004
set MIO7        0xF800071C
set MIO48PIN    0xF80007C0
set MIO49PIN    0xF80007C4

connect
targets -set -filter {name =~ "ARM*#0"}

puts "=== 阶段 A:对照检查(证明 DATA_RO 对 GPIO 引脚确实有效)==="
puts "MIO_PIN_7   = 0x[rd $MIO7]      (ps7_init 写 0x0600 = GPIO)"
puts "MIO_PIN_48  = 0x[rd $MIO48PIN]  (UART1 TX)"
puts "MIO_PIN_49  = 0x[rd $MIO49PIN]  (UART1 RX)"
puts ""
puts "取 6 个 bank0 采样点,心跳 led 槽(0x20008)一起读:"
for {set i 0} {$i < 6} {incr i} {
    puts "CTRL [now] 0x[rd 0xE000A060] led=0x[rd 0x00020008]"
    after 900
}

puts ""
puts "=== 阶段 B:把 MIO49 临时改成 GPIO 输入,读焊盘 ==="
puts "unlock SLCR : [wr $SLCR_UNLOCK 0xDF0D]"
set old49 [rd $MIO49PIN]
puts "原 MIO_PIN_49 = 0x$old49"
# 0x0600 是 ps7_init 给 MIO7/MIO8 的 GPIO 值;保留原值的 bit0(PUS 上拉)
puts "写 0x0601    : [wr $MIO49PIN 0x0601]"
after 100
puts "读回 MIO_PIN_49 = 0x[rd $MIO49PIN]"
puts ""
puts "MIO49 作为 GPIO 时的 bank1 DATA_RO:"
for {set i 0} {$i < 6} {incr i} {
    puts "SAMPLE [now] [rd 0xE000A064] 0x[rd 0xE000A060]"
    after 400
}

puts ""
puts "=== 还原 ==="
puts "写回 0x$old49 : [wr $MIO49PIN 0x$old49]"
puts "读回 MIO_PIN_49 = 0x[rd $MIO49PIN]"
puts "lock SLCR     : [wr $SLCR_LOCK 0x767B]"
puts "PADTEST_DONE [now]"

exit 0
