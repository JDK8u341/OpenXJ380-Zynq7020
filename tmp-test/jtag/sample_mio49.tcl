# 采样 MIO49(PS UART1 RX)引脚上的实际电平,用来判定 PC->板 方向到底是
# 「线没接上」还是「接上了但控制器看不见」。
#
# 原理:Zynq 的 GPIO DATA_RO 读的是 MIO 焊盘上的真实电平,与引脚当前被
# 复用成什么功能无关。MIO[53:32] 属于 bank1,所以:
#   DATA_RO(bank1) = GPIO_BASE + 0x60 + 1*0x04 = 0xE000A064
#   bit16 = MIO48(UART1 TX)   bit17 = MIO49(UART1 RX)
# 偏移来自 Xilinx 自己的 gpiops_v3_14/src/xgpiops_hw.h,不是凭记忆写的。
#
# 配合 tmp-test/pad_probe.py 使用:PC 端交替连发 0x00(压低)与 0xFF(保持高),
# 若 bit17 跟着翻转 => 线路通;若纹丝不动 => 线路不通。
#
# 只读采样,不复位、不下载,可以直接在正在运行的内核上跑。

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

connect

targets -set -filter {name =~ "ARM*#0"}

puts "PADPROBE_MIO48PIN [rd 0xF80007C0]"
puts "PADPROBE_MIO49PIN [rd 0xF80007C4]"
puts "PADPROBE_READY [now]"

# 60 秒 x 250ms = 240 个采样点。PC 端每 4 秒翻转一次,一个周期约 16 个点,
# 时间轴上足够看清楚对应关系。
for {set i 0} {$i < 240} {incr i} {
    puts "SAMPLE [now] [rd 0xE000A064] [rd 0xE000A060]"
    after 250
}

puts "PADPROBE_DONE [now]"
exit 0
