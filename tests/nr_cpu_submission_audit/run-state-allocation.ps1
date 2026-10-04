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
Extract-ProductionMethod (Join-Path $repo 'OptiScaler\shaders\dlssnr\DlssNr_Dx12_FinishedQueue.cpp') `
    'void DlssNr_Dx12::State::QuarantineFinishedPictureSubmission(' (Join-Path $build 'state-quarantine-under-test.inc')
Extract-ProductionMethod (Join-Path $repo 'OptiScaler\shaders\dlssnr\DlssNr_Dx12_Enlarge.cpp') `
    'void DlssNr_Dx12::State::CollectEnlargers(' (Join-Path $build 'enlarger-collector-under-test.inc')
foreach ($method in @('Arm', 'Cancel', 'Finished', 'DiscardUnsubmitted')) {
    Extract-ProductionMethod (Join-Path $repo 'OptiScaler\shaders\dlssnr\DlssNr_Dx12_Late.cpp') `
        "auto DlssNr_Dx12::State::LateContext::$method(" (Join-Path $build "late-$method-under-test.inc")
}
Extract-ProductionMethod (Join-Path $repo 'OptiScaler\shaders\dlssnr\DlssNr_Dx12_FinishedQueue.cpp') `
    'auto DlssNr_Dx12::State::FinishedPictureResetCommandList(' (Join-Path $build 'state-reset-under-test.inc')
Extract-ProductionMethod (Join-Path $repo 'OptiScaler\shaders\dlssnr\DlssNr_Dx12_FinishedQueue.cpp') `
    'auto DlssNr_Dx12::State::WaitForFinishedPicture(' (Join-Path $build 'state-wait-under-test.inc')
Extract-ProductionMethod (Join-Path $repo 'OptiScaler\shaders\dlssnr\DlssNr_Dx12.cpp') `
    'bool DlssNr_Dx12::ReadyToDestroy(' (Join-Path $build 'owner-ready-under-test.inc')
# Compile the complete selection region, stopping before resource allocation/dispatch.
# The CPU fixture provides the device, config and frame inputs; selection stays production code.
$compose = Get-Content -Raw -LiteralPath (Join-Path $repo 'OptiScaler\shaders\dlssnr\DlssNr_Dx12_FinishedCompose.cpp')
$selectionStart = $compose.IndexOf('    LateContext::Slot* latest = nullptr;', [StringComparison]::Ordinal)
$selectionEnd = $compose.IndexOf('    if (!latest)', $selectionStart, [StringComparison]::Ordinal)
if ($selectionStart -lt 0 -or $selectionEnd -lt 0) { throw 'Production composition selection was not found.' }
[IO.File]::WriteAllText((Join-Path $build 'late-selection-under-test.inc'),
    $compose.Substring($selectionStart, $selectionEnd - $selectionStart))
$closeStart = $compose.IndexOf('    if (FAILED(cmd->Close()))', [StringComparison]::Ordinal)
$closeEnd = $compose.IndexOf('    ID3D12CommandList* lists[]', $closeStart, [StringComparison]::Ordinal)
if ($closeStart -lt 0 -or $closeEnd -lt 0) { throw 'Production composition close handling was not found.' }
[IO.File]::WriteAllText((Join-Path $build 'late-close-under-test.inc'),
    $compose.Substring($closeStart, $closeEnd - $closeStart))
$late = Get-Content -Raw -LiteralPath (Join-Path $repo 'OptiScaler\shaders\dlssnr\DlssNr_Dx12_Late.cpp')
$acquireStart = $late.IndexOf('    Slot* next = nullptr;', [StringComparison]::Ordinal)
$acquireEnd = $late.IndexOf('    if (!next)', $acquireStart, [StringComparison]::Ordinal)
if ($acquireStart -lt 0 -or $acquireEnd -lt 0) { throw 'Production acquisition selection was not found.' }
[IO.File]::WriteAllText((Join-Path $build 'late-acquire-under-test.inc'),
    $late.Substring($acquireStart, $acquireEnd - $acquireStart))
$boundary = Join-Path $repo 'tests\nr_cpu_timing_audit'
& cl.exe /nologo /std:c++20 /EHsc /W4 "/I$build" "/I$boundary" "/I$repo\OptiScaler" `
    "/Fo$build\StateSubmissionTests.obj" "/Fe$build\StateSubmissionTests.exe" (Join-Path $PSScriptRoot 'StateSubmissionTests.cpp') ole32.lib
if ($LASTEXITCODE -ne 0) { throw 'State allocation regression compilation failed.' }
& (Join-Path $build 'StateSubmissionTests.exe')
exit $LASTEXITCODE
