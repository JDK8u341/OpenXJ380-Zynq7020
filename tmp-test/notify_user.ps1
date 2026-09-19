<#
    提醒人来做某件事 —— 弹一个窗口出来。

    为什么要有它(用户 2026-09-19 的要求):
      这个移植工程里有一类任务**必须由人动手**才能继续 —— 典型就是
      "把 SD 卡插到板上"以及"确认板子/串口接好"。而代理只能看串口与文件,
      **没有办法确认人是否看到了某句话**;把"需要插卡"写在报告末尾,
      很容易被下一条消息淹没。所以:需要人操作时**弹窗**,而且弹窗里
      必须写清"要做什么、做完怎么告诉我"。

    用法:
      powershell -File tmp-test/notify_user.ps1 -Title "..." -Message "..."

    ⚠ 它是**幂等无害**的:只弹窗,不改任何状态、不碰硬件。
      弹窗会一直停在那里等人点确定 —— 所以调用方应该放到后台跑
      (否则命令行会一直卡住)。
    ⚠ 编码:本文件是 **无 BOM 的 UTF-8**。第一版曾**直接语法错误**
      (报"字符串缺少终止符",连 catch 都没跑到) —— 而原因就是脚本里的中文
      被按非 UTF-8 解。当前这版在本机 `powershell` 5.1 与 `pwsh` 下都实测可用;
      **若哪天再出现同样的解析错误,先怀疑编码**(给文件加 UTF-8 BOM 最稳)。
#>

param(
    [Parameter(Mandatory = $true)][string]$Title,
    [Parameter(Mandatory = $true)][string]$Message
)

$ErrorActionPreference = "Stop"

try {
    Add-Type -AssemblyName System.Windows.Forms | Out-Null
    [System.Windows.Forms.MessageBox]::Show(
        $Message,
        $Title,
        [System.Windows.Forms.MessageBoxButtons]::OK,
        [System.Windows.Forms.MessageBoxIcon]::Information) | Out-Null
    Write-Output "[notify] 弹窗已关闭: $Title"
}
catch {
    # 没有桌面会话(纯 SSH / CI)时不能什么都不做 —— 退化成终端横幅
    Write-Output ("=" * 72)
    Write-Output "!! $Title"
    Write-Output ("=" * 72)
    Write-Output $Message
    Write-Output ("=" * 72)
    Write-Output "[notify] 无法弹窗,已改用终端输出: $($_.Exception.Message)"
}
