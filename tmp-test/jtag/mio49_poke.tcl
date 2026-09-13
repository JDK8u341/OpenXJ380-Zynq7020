# 通过 JTAG 写 MIO_PIN_49。用法: xsdb mio49_poke.tcl <value>
#
# 用于破坏性 A/B:证明 [11:9](IO 标准字段)确实是"接收方向能不能用"的承重位,
# 而不是碰巧改好了别的东西。

proc wr {a v} {
    if {[catch {mwr -force $a $v} e]} { return "ERR: $e" }
    return "OK"
}

set value [lindex $argv 0]
if {$value eq ""} { puts "用法: mio49_poke.tcl <value>"; exit 1 }

connect
targets -set -filter {name =~ "ARM*#0"}

# MIO_PIN_xx 属 SLCR,写入前必须解锁
mwr -force 0xF8000008 0x0000DF0D
puts "MIO49_WRITE [wr 0xF80007C4 $value]"
mwr -force 0xF8000004 0x0000767B
after 100

if {[catch {mrd -force 0xF80007C4} r]} {
    puts "MIO49_READBACK ERR"
} else {
    set s [string trim [lindex [split $r ":"] end]]
    puts "MIO49_READBACK $s"
}
exit 0
