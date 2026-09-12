# 探测 PS 侧 MIO 与 GPIO 配置，用于定位 PS LED

proc rd {a} {
    if {[catch {mrd -force $a} v]} { return "ERR" }
    set s [string trim $v]
    set i [string first ":" $s]
    if {$i >= 0} { return [string trim [string range $s [expr {$i+1}] end]] }
    return $s
}

connect
targets -set -filter {name =~ "ARM*#0"}

puts "=== 09_LED&SWITCH 页上的 MIO 引脚配置 ==="
puts "(L3_SEL=0 表示该引脚被配置为 GPIO)"
foreach n {7 8 12 47} {
    set addr [format 0x%08X [expr {0xF8000700 + 4*$n}]]
    set v [rd $addr]
    if {$v eq "ERR"} { puts "MIO$n  $addr = ERR"; continue }
    set val [expr {"0x$v"}]
    set l3   [expr {($val >> 11) & 0x7}]
    set l0   [expr {$val & 0x7}]
    puts [format "MIO%-3d %s = 0x%s  L3_SEL=%d(%s) L0_SEL=%d" \
        $n $addr $v $l3 [expr {$l3==0 ? "GPIO" : "非GPIO"}] $l0]
}

puts "\n=== PS GPIO 控制器 (0xE000A000) ==="
foreach {name addr} {
    DATA_LSW_0 0xE000A040
    DATA_MSW_0 0xE000A044
    DIRM_0     0xE000A204
    OEN_0      0xE000A208
    DATA_LSW_1 0xE000A048
    DATA_MSW_1 0xE000A04C
    DIRM_1     0xE000A244
    OEN_1      0xE000A248
} {
    puts [format "%-11s %s = 0x%s" $name $addr [rd $addr]]
}

puts "\n=== 逐位解读 MIO0-31 方向/输出使能 ==="
set dirm0 [rd 0xE000A204]
set oen0  [rd 0xE000A208]
set data0 [rd 0xE000A040]
if {$dirm0 ne "ERR" && $oen0 ne "ERR"} {
    set d [expr {"0x$dirm0"}]
    set o [expr {"0x$oen0"}]
    set g [expr {"0x$data0"}]
    foreach n {7 8 12 47} {
        if {$n > 31} { continue }
        puts [format "MIO%-3d DIRM=%d OEN=%d DATA=%d" \
            $n [expr {($d>>$n)&1}] [expr {($o>>$n)&1}] [expr {($g>>$n)&1}]]
    }
}

puts "\n=== 启动模式 ==="
puts "BOOT_MODE (0xF800025C) = 0x[rd 0xF800025C]  (0=JTAG, 5=SD)"

exit 0
