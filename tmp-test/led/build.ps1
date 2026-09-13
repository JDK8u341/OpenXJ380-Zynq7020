# 构建 AC880 LED 测试程序
# 用法: pwsh -File build.ps1
#
# 工具链在哪来自仓库根目录的 config.py(换机器只改那一个文件)。
# 改完 config.py 后本脚本会自动取到新值,这里不用动。

$ErrorActionPreference = 'Stop'

$REPO = (Resolve-Path "$PSScriptRoot\..\..").Path
$PY   = if ($env:PYTHON) { $env:PYTHON } else { 'python' }
$GNU  = (& $PY "$REPO\config.py" --get toolchain.dir).Trim()
if (-not (Test-Path $GNU)) {
    throw "config.py 给出的工具链目录不存在: $GNU`n  请改 $REPO\config.py 里的 VITIS_DIR,然后跑 python config.py 自检"
}
$BIN  = "$GNU\bin"
$CC   = "$BIN\arm-none-eabi-gcc.exe"
$OBJDUMP = "$BIN\arm-none-eabi-objdump.exe"
$READELF = "$BIN\arm-none-eabi-readelf.exe"

# libgcc：ARM state / EABI 变体（见文档 §6.7）
$LIBGCC = "$GNU\aarch32-xilinx-eabi\usr\lib\arm-xilinx-eabi\13.3.0\libgcc.a"

$ROOT = $PSScriptRoot
$OUT  = "$ROOT\out"
New-Item -ItemType Directory -Force $OUT | Out-Null

# 目标：Zynq-7020 = 双核 Cortex-A9，ARMv7-A，VFPv3，硬浮点（对齐 AMD BSP）
$ARCH = @(
    '-mcpu=cortex-a9', '-marm', '-mfpu=vfpv3', '-mfloat-abi=hard',
    '-mno-unaligned-access'
)

$CFLAGS = $ARCH + @(
    '-ffreestanding', '-nostdlib', '-nostdinc', '-fno-builtin',
    '-fno-stack-protector', '-fno-exceptions',
    '-fno-tree-loop-distribute-patterns',
    '-std=gnu11', '-O2', '-g', '-Wall'
)

Write-Host '=== 编译 ===' -ForegroundColor Cyan
& $CC @CFLAGS -c "$ROOT\start.S" -o "$OUT\start.o"
if ($LASTEXITCODE -ne 0) { throw 'start.S 编译失败' }
& $CC @CFLAGS -c "$ROOT\led.c" -o "$OUT\led.o"
if ($LASTEXITCODE -ne 0) { throw 'led.c 编译失败' }

Write-Host '=== 链接（含 -lgcc）===' -ForegroundColor Cyan
& $CC @ARCH -nostdlib -T "$ROOT\link.ld" "$OUT\start.o" "$OUT\led.o" $LIBGCC -o "$OUT\led.elf"
if ($LASTEXITCODE -ne 0) { throw '链接失败' }

Write-Host ''
Write-Host '=== 产物校验 ===' -ForegroundColor Cyan
& $READELF -h "$OUT\led.elf" | Select-String 'Class|Machine|Entry|Flags'
Write-Host ''
& $READELF -S "$OUT\led.elf" | Select-String '\.text|\.rodata|\.data|\.bss'
Write-Host ''
Write-Host "ELF: $OUT\led.elf"
Write-Host ("大小: {0:N0} bytes" -f (Get-Item "$OUT\led.elf").Length)
