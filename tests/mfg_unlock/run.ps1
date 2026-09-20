param([string[]]$Case)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$validation = Join-Path (Split-Path $repo -Parent) 'mfg-validation'
$out = Join-Path $validation 'build'
$seams = Join-Path $PSScriptRoot 'seams'
New-Item -ItemType Directory -Force -Path $out | Out-Null

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual C++ build tools were not found' }

$build = @"
@echo off
call "$vs/VC/Auxiliary/Build/vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /DUNICODE /D_UNICODE /DOPTISCALER_RTX40_MFG /DMFG_UNLOCK_TESTING /FI"$PSScriptRoot/Mocks.h" /I "$seams" /I "$repo/OptiScaler" /c "$repo/OptiScaler/framegen/dlssg/MfgUnlock.cpp" /Fo:"$out/MfgUnlock.obj"
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /wd4244 /DUNICODE /D_UNICODE /DOPTISCALER_RTX40_MFG /DMFG_UNLOCK_TESTING /FI"$PSScriptRoot/Mocks.h" /I "$seams" /I "$repo/OptiScaler" /c "$repo/OptiScaler/scanner/scanner.cpp" /Fo:"$out/scanner.obj"
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /DUNICODE /D_UNICODE /DOPTISCALER_RTX40_MFG /DMFG_UNLOCK_TESTING /FI"$PSScriptRoot/Mocks.h" /I "$seams" /I "$repo/OptiScaler" /c "$PSScriptRoot/PatchTests.cpp" /Fo:"$out/PatchTests.obj"
if errorlevel 1 exit /b 1
link /nologo "$out/MfgUnlock.obj" "$out/scanner.obj" "$out/PatchTests.obj" /out:"$out/mfg-patch.exe"
"@
$buildFile = Join-Path $out 'build.cmd'
Set-Content -LiteralPath $buildFile -Value $build
& $buildFile
if ($LASTEXITCODE) { throw 'MFG CPU regression build failed' }

$cases = @(
    'disabled', 'blackwell', 'ampere', 'other-vendor', 'ampere-option', 'external',
    'missing-gate', 'duplicate-gate', 'mixed-families', 'unknown', 'direct-unknown',
    'legacy', 'legacy-kernel-default-off', '3109', 'kernel-enabled', 'kernel-missing',
    'kernel-malformed', 'protect-failure', 'restore-failure', 'flush-failure',
    'kernel-transaction-failure', 'rollback-failure', 'reference-failure',
    'success-retains-module', 'terminal-no-retry'
)
if ($Case.Count -gt 0) { $cases = $Case }

$passed = 0
$failed = @()
foreach ($name in $cases) {
    & "$out/mfg-patch.exe" $name
    if ($LASTEXITCODE) { $failed += $name }
    else { ++$passed }
}

Write-Host "RESULT: $passed passed; $($failed.Count) failed"
if ($failed.Count -gt 0) { throw "MFG CPU regressions failed: $($failed -join ', ')" }
