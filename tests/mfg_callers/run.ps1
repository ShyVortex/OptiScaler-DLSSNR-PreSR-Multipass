$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$build = Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Force -Path $build | Out-Null

function Get-ProductionFunction([string] $source, [string] $name) {
    $match = [regex]::Match($source, "static [^{;]+\b$name\(")
    if (!$match.Success) { throw "Missing production function $name" }

    $start = $match.Index
    $body = $source.IndexOf('{', $start)
    $depth = 1
    $end = $body + 1
    while ($depth -gt 0 -and $end -lt $source.Length) {
        if ($source[$end] -eq '{') { ++$depth }
        if ($source[$end] -eq '}') { --$depth }
        ++$end
    }
    if ($body -lt 0 -or $depth -ne 0) { throw "Unbalanced production function $name" }
    return $source.Substring($start, $end - $start)
}

$ngxSource = Get-Content -Raw -LiteralPath (Join-Path $repo 'OptiScaler/inputs/NVNGX_DLSS_Dx12.cpp')
$dispatchSource = Get-Content -Raw -LiteralPath (Join-Path $repo 'OptiScaler/framegen/dlssg/DLSSG_Dx12.cpp')
$parameterSource = Get-Content -Raw -LiteralPath (Join-Path $repo 'OptiScaler/NVNGX_Parameter.cpp')
$functions = @(
    Get-ProductionFunction $ngxSource 'ResolveDlssgEvaluationMaximum'
    Get-ProductionFunction $ngxSource 'CanEvaluateDlssg'
    Get-ProductionFunction $ngxSource 'ShouldApplyDlssgEvaluationOverride'
    Get-ProductionFunction $ngxSource 'ResolveDlssgEvaluationFrameCount'
    Get-ProductionFunction $ngxSource 'DirectDlssgOverrideGeneration'
    Get-ProductionFunction $ngxSource 'CommitDlssgEvaluationResult'
    Get-ProductionFunction $dispatchSource 'ResolveDlssgRuntimeMaximum'
    Get-ProductionFunction $dispatchSource 'CanDispatchDlssg'
    Get-ProductionFunction $dispatchSource 'CommitDlssgDispatchOptions'
    Get-ProductionFunction $parameterSource 'ResolveNvngxAdvertisedMfgMaximum'
) -join "`n`n"
[IO.File]::WriteAllText((Join-Path $build 'production-transactions.inc'), $functions)

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Visual Studio C++ build tools are required.' }

$buildCmd = @"
@echo off
call "$vs/VC/Auxiliary/Build/vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 "$PSScriptRoot/CallerTests.cpp" /I "$build" /Fe:"$build/mfg-callers.exe" /Fo:"$build/mfg-callers.obj"
"@
[IO.File]::WriteAllText((Join-Path $build 'build.cmd'), $buildCmd)
& (Join-Path $build 'build.cmd')
if ($LASTEXITCODE) { throw 'MFG caller CPU test compilation failed.' }
& (Join-Path $build 'mfg-callers.exe')
exit $LASTEXITCODE
