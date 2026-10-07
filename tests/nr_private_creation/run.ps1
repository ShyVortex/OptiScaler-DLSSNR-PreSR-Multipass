param(
    [switch]$BaselineOnly,
    [switch]$RetirementOnly,
    [switch]$GateOnly,
    [switch]$DestructorOnly,
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '..\..\outputs\optiscaler-denoise-first-20261006\private-creation')
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
Push-Location -LiteralPath $repo
try {
    $dependencyBuild = ''
    $additionalInclude = ''
    if ($DestructorOnly) {
        $source = Get-Content -Raw -LiteralPath (Join-Path $repo 'OptiScaler\shaders\dlssnr\DlssNr_Dx12.cpp')
        $destructor = $source.IndexOf('DlssNr_Dx12::~DlssNr_Dx12()', [StringComparison]::Ordinal)
        $start = $source.IndexOf('    if (!finished', $destructor, [StringComparison]::Ordinal)
        $body = $source.IndexOf('{', $start)
        if ($destructor -lt 0 -or $start -lt 0 -or $body -lt 0) { throw 'Destructor predicate extraction failed.' }
        [IO.File]::WriteAllText((Join-Path $OutputDirectory 'destructor-predicate-under-test.inc'), $source.Substring($start, $body - $start))
        $name = 'DestructorGuardTests'
        $boundary = Join-Path $repo 'tests\nr_cpu_timing_audit'
        # Refresh the actual owner methods used by the existing surrounding fixture.
        $dependencyBuild = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$repo\tests\nr_cpu_submission_audit\run-state-allocation.ps1`"`nif errorlevel 1 exit /b 1"
        $additionalInclude = "/I`"$repo\tests\nr_cpu_submission_audit\build`""
    }
    elseif ($BaselineOnly) {
        $source = (& git show '73fab132:OptiScaler/shaders/dlssnr/DlssNr_Dx12_DeferredSr.cpp') -join "`n"
        if ($LASTEXITCODE) { throw 'Cannot read audited baseline.' }
        $start = $source.IndexOf('    // Synthetic seam ticks cannot prove', [StringComparison]::Ordinal)
        $end = $source.IndexOf('    const auto inputStates', $start, [StringComparison]::Ordinal)
        if ($start -lt 0 -or $end -lt 0) { throw 'Baseline gate extraction failed.' }
        [IO.File]::WriteAllText((Join-Path $OutputDirectory 'baseline-epoch-under-test.inc'), $source.Substring($start, $end - $start))
        $name = 'BaselineEpochTests'
        $boundary = Join-Path $repo 'tests\nr_cpu_submission_audit'
    }
    elseif ($GateOnly) {
        $source = Get-Content -Raw -LiteralPath (Join-Path $repo 'OptiScaler\shaders\dlssnr\DlssNr_Dx12_DeferredSr.cpp')
        $start = $source.IndexOf('    if (g.creation.Discarded())', [StringComparison]::Ordinal)
        $end = $source.IndexOf('    const auto inputStates', $start, [StringComparison]::Ordinal)
        if ($start -lt 0 -or $end -lt 0) { throw 'Deferred creation gate extraction failed.' }
        [IO.File]::WriteAllText((Join-Path $OutputDirectory 'deferred-gate-under-test.inc'), $source.Substring($start, $end - $start))
        $name = 'DeferredGateTests'
        $boundary = Join-Path $repo 'tests\nr_cpu_submission_audit'
    }
    elseif ($RetirementOnly) {
        $source = Get-Content -Raw -LiteralPath (Join-Path $repo 'OptiScaler\shaders\dlssnr\DlssNr_Dx12_DeferredSr.cpp')
        $start = $source.IndexOf('auto DlssNr_Dx12::State::DeferredSrContext::RetireCurrent(', [StringComparison]::Ordinal)
        if ($start -lt 0) { throw 'Deferred retirement method not found.' }
        $body = $source.IndexOf('{', $start)
        $depth = 1
        $end = $body + 1
        while ($depth -gt 0 -and $end -lt $source.Length) {
            if ($source[$end] -eq '{') { ++$depth }
            if ($source[$end] -eq '}') { --$depth }
            ++$end
        }
        if ($depth -ne 0) { throw 'Unbalanced retirement method braces.' }
        [IO.File]::WriteAllText((Join-Path $OutputDirectory 'deferred-retire-under-test.inc'), $source.Substring($start, $end - $start))
        $name = 'DeferredRetirementTests'
        $boundary = Join-Path $repo 'tests\nr_cpu_submission_audit'
    }
    else {
        $name = 'PrivateCreationTests'
        $boundary = Join-Path $repo 'tests\nr_cpu_submission_audit'
    }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$vs) { throw 'Visual Studio C++ build tools are required.' }
    $buildCmd = @"
@echo off
call "$vs/Common7/Tools/VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
$dependencyBuild
cl /nologo /std:c++20 /EHsc /W4 /I"$OutputDirectory" /I"$boundary" /I"$repo\OptiScaler" $additionalInclude /Fo"$OutputDirectory\$name.obj" /Fe"$OutputDirectory\$name.exe" "$PSScriptRoot\$name.cpp" ole32.lib
"@
    [IO.File]::WriteAllText((Join-Path $OutputDirectory "$name-build.cmd"), $buildCmd)
    & (Join-Path $OutputDirectory "$name-build.cmd")
    if ($LASTEXITCODE) { throw "$name compilation failed." }
    & (Join-Path $OutputDirectory "$name.exe") | Tee-Object -FilePath (Join-Path $OutputDirectory "$name.log")
    exit $LASTEXITCODE
}
finally { Pop-Location }
