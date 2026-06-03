# Builds the C# POC three ways for comparison:
#   1) Framework-dependent (needs .NET 10 runtime) -> out\fdd
#   2) Native AOT, self-contained, single file     -> out\aot
#   3) Native AOT + LZMA compression (PeekDesktop)  -> out\aot-lzma
# Reports the resulting executable sizes.

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$proj = Join-Path $here 'AltWindowCycle.csproj'

$fdd = Join-Path $here 'out\fdd'
$aot = Join-Path $here 'out\aot'
$lzma = Join-Path $here 'out\aot-lzma'

Write-Host '== Framework-dependent publish ==' -ForegroundColor Cyan
dotnet publish $proj -c Release -r win-x64 --self-contained false `
    -p:PublishAot=false -o $fdd | Out-Null

Write-Host '== Native AOT publish ==' -ForegroundColor Cyan
dotnet publish $proj -c Release -r win-x64 -o $aot | Out-Null

Write-Host '== Native AOT + LZMA publish ==' -ForegroundColor Cyan
dotnet publish $proj -c Release -r win-x64 -p:AotCompress=true -o $lzma | Out-Null

function Show-Size($label, $path) {
    if (Test-Path $path) {
        $sz = (Get-Item $path).Length
        Write-Host ("{0}: {1} ({2:N0} bytes / {3:N1} KB)" -f $label, $path, $sz, ($sz / 1KB))
    } else {
        Write-Warning "$label not found: $path"
    }
}

Show-Size 'FDD exe     ' (Join-Path $fdd 'AltWindowCycle.exe')
Show-Size 'AOT exe     ' (Join-Path $aot 'AltWindowCycle.exe')
Show-Size 'AOT+LZMA exe' (Join-Path $lzma 'AltWindowCycle.exe')
