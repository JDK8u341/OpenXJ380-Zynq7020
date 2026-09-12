# 探测 JTAG 链路：列出目标与 IDCODE
# 用法: xsdb probe.tcl

if {[catch {connect} err]} {
    puts "RESULT: CONNECT_FAILED"
    puts "DETAIL: $err"
    exit 1
}

puts "RESULT: CONNECT_OK"

if {[catch {puts "TARGETS:\n[targets]"} err]} {
    puts "TARGETS_ERROR: $err"
}

# 报告每个目标的 IDCODE / 名称
foreach t [targets] {
    puts "TARGET_ENTRY: $t"
}

puts "DONE"
exit 0
