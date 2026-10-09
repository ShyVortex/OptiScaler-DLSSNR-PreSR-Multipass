$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$build = Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Force -Path $build | Out-Null
$source = Get-Content -Raw -LiteralPath (Join-Path $repo 'OptiScaler/framegen/xefg/XeFG_Dx12.cpp')
$signature = [regex]::Match($source, 'bool XeFG_Dx12::SetInterpolatedFrameCount\(')
if (!$signature.Success) { throw 'Missing production XeFG count method' }
$start = $signature.Index
$body = $source.IndexOf('{', $start)
$depth = 1
$end = $body + 1
while ($depth -gt 0 -and $end -lt $source.Length) {
    if ($source[$end] -eq '{') { ++$depth }
    if ($source[$end] -eq '}') { --$depth }
    ++$end
}
if ($body -lt 0 -or $depth -ne 0) { throw 'Unbalanced production XeFG count method' }
[IO.File]::WriteAllText((Join-Path $build 'production-count.inc'), $source.Substring($start, $end - $start))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Visual Studio C++ build tools are required.' }
$commands = @"
@echo off
call "$vs/VC/Auxiliary/Build/vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /DUNICODE /D_UNICODE /I "$build" /I "$repo/external/xess/inc/xess_fg" "$PSScriptRoot/CountTests.cpp" /Fe:"$build/xefg-count.exe" /Fo:"$build/xefg-count.obj"
"@
[IO.File]::WriteAllText((Join-Path $build 'build.cmd'), $commands)
& (Join-Path $build 'build.cmd')
if ($LASTEXITCODE) { throw 'Production XeFG count regression compilation failed.' }
& (Join-Path $build 'xefg-count.exe')
exit $LASTEXITCODE
