param([string[]]$Case)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$build = Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Force -Path $build | Out-Null
$config = Get-Content -Raw -LiteralPath (Join-Path $repo 'OptiScaler/Config.h')
$start = $config.IndexOf('enum HasDefaultValue')
$end = $config.IndexOf('constexpr inline int UnboundKey', $start)
if ($start -lt 0 -or $end -lt $start) { throw 'Missing production CustomOptional definition' }
[IO.File]::WriteAllText((Join-Path $build 'production-config.inc'), $config.Substring($start, $end - $start))
$menu = Get-Content -Raw -LiteralPath (Join-Path $repo 'OptiScaler/menu/menu_common.cpp')
$gate = [regex]::Match($menu, 'const bool disableXeMfg = [^;]+;')
if (!$gate.Success) { throw 'Missing production XeMFG checkbox gate' }
[IO.File]::WriteAllText((Join-Path $build 'production-menu-gate.inc'), $gate.Value)
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Visual Studio C++ build tools are required.' }
$commands = @"
@echo off
call "$vs/VC/Auxiliary/Build/vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /DUNICODE /D_UNICODE /FI"$PSScriptRoot/Mocks.h" /I "$build" /I "$PSScriptRoot/seams" /I "$repo/OptiScaler" "$PSScriptRoot/ConfigTests.cpp" /Fe:"$build/xemfg-config.exe" /Fo:"$build/xemfg-config.obj"
"@
[IO.File]::WriteAllText((Join-Path $build 'build.cmd'), $commands)
& (Join-Path $build 'build.cmd')
if ($LASTEXITCODE) { throw 'Production XeMFG configuration regression compilation failed.' }
if (!$Case.Count) {
    $Case = @('default-off','saved-off','explicit-on','external-conflict','ada-conflict','ampere-conflict',
        'scanner-end','ceiling-live','menu-recovery')
}
$failed = @()
foreach ($name in $Case) {
    & (Join-Path $build 'xemfg-config.exe') $name
    if ($LASTEXITCODE) { $failed += $name }
}
if ($failed.Count) { throw "XeMFG configuration failures: $($failed -join ', ')" }
Write-Output "$($Case.Count) production-backed configuration/scanner scenarios passed; no DLL or GPU workload."
