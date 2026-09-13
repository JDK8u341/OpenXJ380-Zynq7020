# 修正 MIO bank 1 的 IO 标准:LVCMOS33 -> LVCMOS18。
#
# ── 问题 ──────────────────────────────────────────────────────────────
# 本板(AC850-CORE + AC880-CB)的 MIO bank 1 实际供电是 1.8V:
#   核心板原理图 第2页  VCCIO_BANK1 接 VCC1P8
#   底板原理图   第4页  PS_UART_RXD/TXD 经 R150/R151(1K) 上拉到 VIO_1P8,
#                       CH9102F 的 VIO 脚同样接 VIO_1P8
#   vendor 参考工程 02_key_ctrl_led 的 .hwh:bank1 的 38 个 MIO 全是 LVCMOS 1.8V
#
# 但 opjtmp.xsa 里 54 个 MIO 全部被声明成 "LVCMOS 3.3V",于是 ps7_init
# 给 MIO16-53 写下的 MIO_PIN 里 [11:9](IO 标准字段)= 3(LVCMOS33)。
#
# 后果:**输入方向失效**。MIO49(UART1 RX) 的输入阈值按 LVCMOS33 约 2.0V,
# 而 CH9102F 只能驱动到 1.8V -> 引脚恒读低 -> 没有下降沿 -> UART 永远
# 等不到起始位 -> SR.RXEMPTY 恒为 1、RXOVR 恒为 0,看起来像"线断了"。
# 输出方向不受影响(摆幅由物理 VCCIO 决定),所以 TX 一直正常 ——
# 这正是它极具迷惑性的原因。
#
# ── 修法 ──────────────────────────────────────────────────────────────
# MIO_PIN_xx 的字段(位域取自 vendor ps7_init.c 的逐位注释):
#   [0:0] [1:1] [2:2] [4:3] [7:5] [8:8] [11:9]=IO标准 [12:12]=上拉 [13:13]
# bank1 的每个值只需把 [11:9] 从 3 改成 1,即 value - 0x400:
#   0x1600 -> 0x1200   0x16E0 -> 0x12E0   0x16E1 -> 0x12E1
# vendor 正确工程里 MIO_48 正是 0x1200 —— 与此变换一致,互为印证。
#
# 用 mask_write 只动 [11:9],其余位(功能选择 [7:5]、上拉 [12:12] 等)原样保留,
# 所以这个修正与引脚被复用成什么功能无关。
#
# ── 这不是最终修法 ────────────────────────────────────────────────────
# 正解是重新导出 XSA 时把 MIO bank 1 的电压设成 1.8V,让 Vivado 直接生成
# 正确的 ps7_init。本文件只是让板子在拿到新 XSA 之前能正常工作的补偿。

proc ps7_mio_bank1_18v_fixup {} {
    # MIO16..MIO53 -> 0xF8000700 + 4*n
    mwr -force 0xF8000008 0x0000DF0D
    for {set n 16} {$n <= 53} {incr n} {
        set addr [expr {0xF8000700 + 4 * $n}]
        # mask 0x00000E00 = [11:9];value 0x00000200 把它置成 1 = LVCMOS18
        mask_write $addr 0x00000E00 0x00000200
    }
    mwr -force 0xF8000004 0x0000767B
}

proc ps7_mio_bank1_18v_check {} {
    set bad 0
    for {set n 16} {$n <= 53} {incr n} {
        set addr [expr {0xF8000700 + 4 * $n}]
        if {[catch {mrd -force $addr} v]} { continue }
        set s [string trim [lindex [split $v ":"] end]]
        if {[scan $s %x val] != 1} { continue }
        if {((($val >> 9) & 0x7)) != 1} { incr bad }
    }
    return $bad
}
