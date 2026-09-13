# MIO bank 电压前置校验(以及给旧 XSA 用的补偿)。
#
# ── 背景 ──────────────────────────────────────────────────────────────
# 本板(AC850-CORE + AC880-CB)的 MIO bank 1 实际供电是 1.8V:
#   核心板原理图 第2页  VCCIO_BANK1 接 VCC1P8
#   核心板原理图 第3页  VCC3P3 接 VCCIO_BANK0(bank 0 是 3.3V)
#   底板原理图   第4页  PS_UART_RXD/TXD 经 R150/R151(1K) 上拉到 VIO_1P8,
#                       CH9102F 的 VIO 脚同样接 VIO_1P8
#   vendor 参考工程 02_key_ctrl_led 的 .hwh:bank1 的 38 个 MIO 全是 LVCMOS 1.8V
#
# 旧的 opjtmp.xsa 把 54 个 MIO 全部声明成 "LVCMOS 3.3V",于是 ps7_init 给
# MIO16-53 写下的 MIO_PIN 里 [11:9](IO 标准)= 3(LVCMOS33),而不是 1(LVCMOS18)。
#
# 后果:**输入方向失效**。MIO49(UART1 RX) 的输入阈值按 LVCMOS33 约 2.0V,
# 而 CH9102F 只能驱动到 1.8V -> 引脚恒读低 -> 没有下降沿 -> UART 永远等不到
# 起始位 -> SR.RXEMPTY 恒为 1、RXOVR 恒为 0,**看起来和"线断了"一模一样**。
# 输出方向不受影响(摆幅由物理 VCCIO 决定),所以 TX 一直正常 —— 这正是它
# 极具迷惑性的原因。
#
# 影响不止串口:bank 1 是 MIO16-53,以太网 / USB / SD 的输入都在里面。
#
# ── 现状:已从源头修好 ────────────────────────────────────────────────
# 2026-09-13 重新导出的 XSA 已把 bank 1 改成 1.8V,Vivado 现在原生生成
#   MIO_PIN_48 = 0x000012E0   MIO_PIN_49 = 0x000012E1
# 与当初手工推导的补偿值(0x16E0/0x16E1 减 0x400)**逐位相同**,互为印证。
# 所以加载器现在**只做校验、不再改寄存器**。
#
# 为什么保留校验而不是直接删掉:这类配置错误的表现是"寄存器全对但收不到
# 数据",没有任何报错。把它变成加载时的一条硬断言,下次换 XSA 出错会**当场
# 大声失败**,而不是再花一轮去查线。
#
# ── 位域(取自 vendor ps7_init.c 的逐位注释)──────────────────────────
#   [0:0] [1:1] [2:2] [4:3] [7:5] [8:8] [11:9]=IO标准 [12:12]=上拉 [13:13]
#   [11:9]: 3 = LVCMOS33, 1 = LVCMOS18
# bank1 的每个值只需把 [11:9] 从 3 改成 1,即 value - 0x400:
#   0x1600 -> 0x1200   0x16E0 -> 0x12E0   0x16E1 -> 0x12E1
#
# bank 0(MIO0-15)不用动:两边声明都是 3.3V,与硬件一致(逐脚核对过)。

# 返回 MIO16-53 里 [11:9] != 1(即不是 LVCMOS18)的个数。0 表示配置正确。
proc ps7_mio_bank1_check {} {
    set bad 0
    for {set n 16} {$n <= 53} {incr n} {
        set addr [expr {0xF8000700 + 4 * $n}]
        if {[catch {mrd -force $addr} v]} { incr bad; continue }
        set s [string trim [lindex [split $v ":"] end]]
        if {[scan $s %x val] != 1} { incr bad; continue }
        if {((($val >> 9) & 0x7)) != 1} { incr bad }
    }
    return $bad
}

# 打印前若干个不合规的引脚,便于定位。
proc ps7_mio_bank1_report {} {
    for {set n 16} {$n <= 53} {incr n} {
        set addr [expr {0xF8000700 + 4 * $n}]
        if {[catch {mrd -force $addr} v]} { continue }
        set s [string trim [lindex [split $v ":"] end]]
        if {[scan $s %x val] != 1} { continue }
        if {((($val >> 9) & 0x7)) != 1} {
            puts "    MIO$n = 0x[format %08X $val]  \[11:9]=[expr {($val >> 9) & 0x7}]"
        }
    }
}

# 给旧 XSA 用的补偿。新的 XSA 不需要,加载器也不调用它 ——
# 留着是为了万一回退到旧 XSA 时有个现成的东西可用。
proc ps7_mio_bank1_18v_fixup {} {
    mwr -force 0xF8000008 0x0000DF0D
    for {set n 16} {$n <= 53} {incr n} {
        set addr [expr {0xF8000700 + 4 * $n}]
        # mask 0x00000E00 = [11:9];value 0x00000200 把它置成 1 = LVCMOS18
        mask_write $addr 0x00000E00 0x00000200
    }
    mwr -force 0xF8000004 0x0000767B
}
