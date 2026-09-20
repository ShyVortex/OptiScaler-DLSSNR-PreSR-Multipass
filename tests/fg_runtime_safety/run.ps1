$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$build = Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Force -Path $build | Out-Null
$methods = ''
foreach ($entry in @(
    @{ file = 'Reflex_Hooks.cpp'; owner = 'ReflexHooks'; method = 'hkNvAPI_D3D_Sleep' },
    @{ file = 'Reflex_Hooks.cpp'; owner = 'ReflexHooks'; method = 'hkNvAPI_D3D_SetLatencyMarker' },
    @{ file = 'Streamline_Hooks.cpp'; owner = 'StreamlineHooks'; method = 'hkslPCLSetMarker' }
)) {
    $source = Get-Content -Raw -LiteralPath (Join-Path $repo "OptiScaler/hooks/$($entry.file)")
    $method = $entry.method
    $signature = [regex]::Match($source, "(?:NvAPI_Status|sl::Result) $($entry.owner)::$method\(")
    if (!$signature.Success) { throw "Missing production method $method" }
    $start = $signature.Index
    $body = $source.IndexOf('{',$start)
    $depth = 1
    $end = $body + 1
    while ($depth -gt 0 -and $end -lt $source.Length) {
        if ($source[$end] -eq '{') { ++$depth }
        if ($source[$end] -eq '}') { --$depth }
        ++$end
    }
    if ($body -lt 0 -or $depth -ne 0) { throw "Unbalanced method $method" }
    $methods += $source.Substring($start,$end-$start) + "`n"
}
[IO.File]::WriteAllText((Join-Path $build 'production-reflex.inc'),$methods)
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Visual Studio C++ build tools are required.' }
$buildCmd = @"
@echo off
call "$vs/VC/Auxiliary/Build/vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /DUNICODE /D_UNICODE /I "$repo/external/streamline" /I "$repo/external/nvapi" /I "$build" "$PSScriptRoot/ReflexTokenTests.cpp" /Fe:"$build/fg-runtime-safety.exe" /Fo:"$build/fg-runtime-safety.obj"
"@
[IO.File]::WriteAllText((Join-Path $build 'build.cmd'),$buildCmd)
& (Join-Path $build 'build.cmd')
if ($LASTEXITCODE) { throw 'FG runtime safety CPU test compilation failed.' }
& (Join-Path $build 'fg-runtime-safety.exe')
exit $LASTEXITCODE
