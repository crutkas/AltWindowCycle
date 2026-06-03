# Builds the C++ POC with the VS 2026 (v18) x64 toolchain.
# Output: out\AltWindowCycle.exe  (static CRT, size-optimized, no console)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$out = Join-Path $here 'out'
New-Item -ItemType Directory -Force -Path $out | Out-Null

$vcvars = 'C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

$src = Join-Path $here 'AltWindowCycle.cpp'
$cmd = "call `"$vcvars`" >nul && cd /d `"$out`" && cl /nologo /W4 /EHsc /O1 /Os /GL /MT /DUNICODE /D_UNICODE `"$src`" /Fe:AltWindowCycle.exe /link /SUBSYSTEM:WINDOWS /LTCG /OPT:REF /OPT:ICF"
& $env:ComSpec /c $cmd

$exe = Join-Path $out 'AltWindowCycle.exe'
if (Test-Path $exe) {
    $sz = (Get-Item $exe).Length
    Write-Host ("Built: {0} ({1:N0} bytes / {2:N1} KB)" -f $exe, $sz, ($sz / 1KB))
} else {
    throw 'Build failed: exe not produced'
}
