proc rd {a} {
    if {[catch {mrd -force $a} v]} { return -1 }
    set s [string trim $v]
    set i [string first ":" $s]
    if {$i >= 0} { set s [string trim [string range $s [expr {$i+1}] end]] }
    return [expr {"0x$s"}]
}
connect
targets -set -filter {name =~ "ARM*#0"}
puts ""
puts "标称波特率(寄存器)  ->  实测波特率(计时)"
puts "-----------------------------------------"
set seen {}
for {set i 0} {$i < 26} {incr i} {
    set nom [rd 0x00020020]
    set act [rd 0x00020024]
    if {$nom > 0 && $nom < 1000000} {
        set key "$nom/$act"
        if {[lsearch -exact $seen $key] < 0} {
            lappend seen $key
            if {$act > 0} {
                puts [format "  %-10d  ->  %-10d   比值 %.4f" $nom $act [expr {double($act)/$nom}]]
            } else {
                puts [format "  %-10d  ->  测量失败" $nom]
            }
        }
    }
    after 1500
}
exit 0
