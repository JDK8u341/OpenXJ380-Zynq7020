# 从 Ghostscript 生成的原理图 PDF 中提取文本
# 关键点：PDF 的 FlateDecode 流是 zlib 格式（有 2 字节头），
#         而 .NET DeflateStream 期望裸 deflate，必须跳过 2 字节。
param(
    [Parameter(Mandatory=$true)][string]$Pdf,
    [string]$Out
)
Add-Type -AssemblyName System.IO.Compression

$bytes = [System.IO.File]::ReadAllBytes($Pdf)
$latin = [System.Text.Encoding]::GetEncoding(28591).GetString($bytes)
$sb = New-Object System.Text.StringBuilder
$ok = 0; $fail = 0

foreach ($m in [regex]::Matches($latin, '[^a-zA-Z]stream\r?\n')) {
    $start = $m.Index + $m.Length
    $end = $latin.IndexOf('endstream', $start)
    if ($end -lt 0) { continue }
    $e = $end
    while ($e -gt $start -and ($latin[$e-1] -eq "`n" -or $latin[$e-1] -eq "`r")) { $e-- }
    if ($e - $start -le 3) { continue }

    $z = $start + 2                      # 跳过 zlib 头
    try {
        $seg = New-Object byte[] ($e - $z)
        [Array]::Copy($bytes, $z, $seg, 0, $e - $z)
        $ms = New-Object System.IO.MemoryStream(, $seg)
        $ds = New-Object System.IO.Compression.DeflateStream($ms, [System.IO.Compression.CompressionMode]::Decompress)
        $o  = New-Object System.IO.MemoryStream
        $ds.CopyTo($o); $ds.Dispose(); $ms.Dispose()
        [void]$sb.Append([System.Text.Encoding]::GetEncoding(28591).GetString($o.ToArray()))
        $ok++
    } catch { $fail++ }
}

$text = $sb.ToString()
if ($Out) {
    Set-Content -Path $Out -Value $text -Encoding UTF8
    Write-Host "解压成功 $ok / 失败 $fail -> $Out ($($text.Length) 字符)"
} else {
    Write-Host "解压成功 $ok / 失败 $fail ($($text.Length) 字符)"
}
return $text
