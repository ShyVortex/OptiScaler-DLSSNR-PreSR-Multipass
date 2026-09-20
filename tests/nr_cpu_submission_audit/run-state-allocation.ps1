$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
function Extract-ProductionMethod($sourcePath, $signature, $outputPath) {
$source = Get-Content -Raw -LiteralPath $sourcePath
$start = $source.IndexOf($signature, [StringComparison]::Ordinal)
if ($start -lt 0) { throw 'Production method was not found.' }
$body = $source.IndexOf('{', $start)
$depth = 1
$end = $body + 1
while ($depth -gt 0 -and $end -lt $source.Length) {
    if ($source[$end] -eq '{') { ++$depth }
    if ($source[$end] -eq '}') { --$depth }
    ++$end
}
if ($depth -ne 0) { throw 'Unbalanced production method braces.' }
# Mechanical build generation of the actual method, not a copied ownership algorithm.
[IO.File]::WriteAllText($outputPath, $source.Substring($start, $end - $start))
}
$build = Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Path $build -Force | Out-Null
Extract-ProductionMethod (Join-Path $repo 'OptiScaler\shaders\dlssnr\DlssNr_Dx12_FinishedQueue.cpp') `
    'auto DlssNr_Dx12::State::BeginFinishedPictureSubmission(' (Join-Path $build 'state-method-under-test.inc')
Extract-ProductionMethod (Join-Path $repo 'OptiScaler\shaders\dlssnr\DlssNr_Dx12_Enlarge.cpp') `
    'void DlssNr_Dx12::State::CollectEnlargers(' (Join-Path $build 'enlarger-collector-under-test.inc')
$boundary = Join-Path $repo 'tests\nr_cpu_timing_audit'
& cl.exe /nologo /std:c++20 /EHsc /W4 "/I$build" "/I$boundary" "/I$repo\OptiScaler" `
    "/Fo$build\StateSubmissionTests.obj" "/Fe$build\StateSubmissionTests.exe" (Join-Path $PSScriptRoot 'StateSubmissionTests.cpp') ole32.lib
if ($LASTEXITCODE -ne 0) { throw 'State allocation regression compilation failed.' }
& (Join-Path $build 'StateSubmissionTests.exe')
exit $LASTEXITCODE
