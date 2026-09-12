proc rd {a} {
    if {[catch {mrd -force $a} v]} { return -1 }
    set s [string trim $v]
    set i [string first ":" $s]
    if {$i >= 0} { set s [string trim [string range $s [expr {$i+1}] end]] }
    return [expr {"0x$s"}]
}
connect
targets -set -filter {name =~ "ARM*#0"}
puts "=== PS GPIO bank1 (MIO32-53) 方向/使能 ==="
set d1 [rd 0xE000A244]
set o1 [rd 0xE000A248]
puts [format "DIRM_1 = 0x%08X   OEN_1 = 0x%08X" $d1 $o1]
puts "bit 表示 MIO(32+bit):"
foreach b {14 15 16 17} {
    set mio [expr {32+$b}]
    if {$mio == 46 || $mio == 47 || $mio == 48 || $mio == 49} {
        puts [format "  MIO%-3d DIRM=%d OEN=%d" $mio [expr {($d1>>$b)&1}] [expr {($o1>>$b)&1}]]
    }
}
puts ""
puts "=== bank0 (MIO0-31) 现状 ==="
puts [format "DIRM_0 = 0x%08X   OEN_0 = 0x%08X" [rd 0xE000A204] [rd 0xE000A208]]
puts ""
puts "=== 关键:MIO48/49 是否被 UART1 独占 ==="
puts [format "MIO_PIN_48 = 0x%08X" [rd 0xF80007C0]]
puts [format "MIO_PIN_49 = 0x%08X" [rd 0xF80007C4]]
exit 0
