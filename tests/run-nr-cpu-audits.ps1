param([string]$OutputDirectory = (Join-Path $env:TEMP 'OptiScaler-nr-cpu-audits'))
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
Push-Location -LiteralPath $repo
try {
    foreach ($test in @(
        @('nr_cpu_submission_audit', 'SubmissionResetTests'),
        @('nr_cpu_timing_audit', 'TimingResetTests'),
        @('nr_cpu_threaded_audit', 'ThreadedSubmissionTests'),
        @('nr_cpu_descriptor_audit', 'DescriptorSlotsTests')
    )) {
        $directory = Join-Path $PSScriptRoot $test[0]
        $name = $test[1]
        & cl.exe /nologo /std:c++20 /EHsc /W4 "/I$directory" "/I$repo\OptiScaler" `
            "/Fo$OutputDirectory\$name.obj" "/Fe$OutputDirectory\$name.exe" "$directory\$name.cpp" ole32.lib
        if ($LASTEXITCODE) { throw "$name compilation failed" }
        & "$OutputDirectory\$name.exe"
        if ($LASTEXITCODE) { throw "$name failed" }
    }
    & "$OutputDirectory\ThreadedSubmissionTests.exe" --post-only
    if ($LASTEXITCODE -ne 1) { throw 'Post-only negative control must fail with exit 1' }
    foreach ($runner in @(
        'nr_cpu_owner_aggregation_audit\run.ps1',
        'nr_cpu_submission_audit\run-state-allocation.ps1',
        'nr_cpu_encode_failure_audit\run.ps1'
    )) {
        & (Join-Path $PSScriptRoot $runner)
        if ($LASTEXITCODE) { throw "$runner failed" }
    }
    Write-Output 'All seven NR CPU audit groups passed; the post-only negative control failed as intended.'
}
finally { Pop-Location }
