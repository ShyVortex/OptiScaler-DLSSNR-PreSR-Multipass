param(
    [string]$Repo = (Join-Path $PSScriptRoot '../..'),
    [string]$DenoiseSource = '',
    [string]$RoutingSource = '',
    [string]$OutputDirectory = (Join-Path $PSScriptRoot 'build')
)
$ErrorActionPreference = 'Stop'
$Repo = (Resolve-Path -LiteralPath $Repo).Path
if (!$DenoiseSource) { $DenoiseSource = Join-Path $Repo 'OptiScaler/shaders/dlssnr/DlssNr_Dx12_DenoiseFirst.cpp' }
$DenoiseSource = (Resolve-Path -LiteralPath $DenoiseSource).Path
if (!$RoutingSource) { $RoutingSource = Join-Path $Repo 'OptiScaler/upscalers/IFeature_Dx12.cpp' }
$RoutingSource = (Resolve-Path -LiteralPath $RoutingSource).Path
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
Start-Transcript -Path (Join-Path $OutputDirectory 'test-run.txt') -Force | Out-Null
function Extract-Method([string]$Source, [string]$Name) {
    $match = [regex]::Match($Source, '(?:auto|void) DlssNr_Dx12::State::' + [regex]::Escape($Name) + '\(')
    if (!$match.Success) { throw "Missing production method $Name" }
    $body = $Source.IndexOf('{', $match.Index)
    $depth = 1
    $end = $body + 1
    while ($depth -gt 0 -and $end -lt $Source.Length) {
        if ($Source[$end] -eq '{') { ++$depth }
        if ($Source[$end] -eq '}') { --$depth }
        ++$end
    }
    if ($body -lt 0 -or $depth -ne 0) { throw "Unbalanced production method $Name" }
    return $Source.Substring($match.Index, $end - $match.Index) + "`n"
}
$denoise = Get-Content -Raw -LiteralPath $DenoiseSource
$runPath = Join-Path $Repo 'OptiScaler/shaders/dlssnr/DlssNr_Dx12_Run.cpp'
$runSource = Get-Content -Raw -LiteralPath $runPath
$deferredPath = Join-Path $Repo 'OptiScaler/shaders/dlssnr/DlssNr_Dx12_DeferredSr.cpp'
$statusPath = Join-Path $Repo 'OptiScaler/shaders/dlssnr/DlssNr_Dx12_Status.cpp'
$deferred = Get-Content -Raw -LiteralPath $deferredPath
$status = Get-Content -Raw -LiteralPath $statusPath
$methods = Extract-Method $denoise 'DenoiseFirstContext::After'
foreach ($name in @('Cancel', 'RetireCurrent', 'ReleaseResources')) {
    $methods += Extract-Method $denoise "DenoiseFirstContext::$name"
    $methods += Extract-Method $deferred "DeferredSrContext::$name"
}
$methods += Extract-Method $status 'RetryAfterFailure'
[IO.File]::WriteAllText((Join-Path $OutputDirectory 'production-methods.inc'), $methods)
$pendingMatch = [regex]::Match($denoise, 'if \(pending\.cmd(?: && current)?\)')
if (!$pendingMatch.Success) { throw 'Missing Before abandoned pending branch' }
$pendingStart = $pendingMatch.Index
$pendingEnd = $denoise.IndexOf('struct ResetOnGap', $pendingStart)
if ($pendingStart -lt 0 -or $pendingEnd -lt 0) { throw 'Missing Before abandoned pending segment' }
[IO.File]::WriteAllText((Join-Path $OutputDirectory 'production-abandoned-pending.inc'), $denoise.Substring($pendingStart, $pendingEnd - $pendingStart))
$modelStart = $runSource.IndexOf('bool evaluated = false;')
$modelEnd = $runSource.IndexOf('finalAnswer = passOutput;', $modelStart)
if ($modelStart -lt 0 -or $modelEnd -lt 0) { throw 'Missing actual model evaluation flag propagation segment' }
[IO.File]::WriteAllText((Join-Path $OutputDirectory 'production-model-evaluated.inc'), $runSource.Substring($modelStart, $modelEnd - $modelStart))
$runCall = [regex]::Match($denoise, 'owner\.Run\(cmd,[\s\S]*?;')
if (!$runCall.Success) { throw 'Missing Before owner.Run invocation' }
[IO.File]::WriteAllText((Join-Path $OutputDirectory 'production-before-run-call.inc'), $runCall.Value)
function Extract-IfAt([string]$Source, [int]$Start) {
    $body = $Source.IndexOf('{', $Start)
    $depth = 1
    $end = $body + 1
    while ($depth -gt 0 -and $end -lt $Source.Length) {
        if ($Source[$end] -eq '{') { ++$depth }
        if ($Source[$end] -eq '}') { --$depth }
        ++$end
    }
    if ($Start -lt 0 -or $body -lt 0 -or $depth -ne 0) { throw 'Missing Before failure block' }
    return $Source.Substring($Start, $end - $Start)
}
$failedGate = [regex]::Match($denoise, 'if \(g.failed\)\s*return handoff;')
if (!$failedGate.Success) { throw 'Missing Before failed-generation gate' }
[IO.File]::WriteAllText((Join-Path $OutputDirectory 'production-failed-gate.inc'), $failedGate.Value)
$nrCheck = $denoise.IndexOf('if (owner.nr.successfulDispatches == before)')
$capture = $denoise.IndexOf('if (owner.pipelineCapture)', $nrCheck)
if ($nrCheck -lt 0 -or $capture -lt 0) { throw 'Missing Before successful NR segment' }
[IO.File]::WriteAllText((Join-Path $OutputDirectory 'production-nr-success.inc'), $denoise.Substring($nrCheck, $capture - $nrCheck))
foreach ($failure in @(@('composition', 'Say("edit composition failed'), @('enlarge', 'Say("private DLSS SR evaluation failed'))) {
    $failureMessage = $denoise.IndexOf($failure[1])
    if ($failureMessage -lt 0) { throw "Missing Before failure branch $($failure[0])" }
    $failureStart = $denoise.LastIndexOf('if (!ok)', $failureMessage)
    [IO.File]::WriteAllText((Join-Path $OutputDirectory "production-$($failure[0])-failure.inc"), (Extract-IfAt $denoise $failureStart))
}
$routing = Get-Content -Raw -LiteralPath $RoutingSource
$routingMethods = ''
foreach ($name in @('denoiseFirst', 'nrBeforeUpscale')) {
    $match = [regex]::Match($routing, 'const bool ' + $name + '\s*=\s*[\s\S]*?;')
    if (!$match.Success) { throw "Missing production routing declaration $name" }
    $routingMethods += $match.Value + "`n"
}
$postDeclaration = [regex]::Match($routing, 'const bool nrAfterUpscale\s*=\s*[\s\S]*?;')
if ($postDeclaration.Success) {
    $routingMethods += $postDeclaration.Value + "`n"
} else {
$postPass = $routing.IndexOf('pipeline.push_back(MakeDlssNrPass(')
if ($postPass -lt 0) { throw 'Missing post-upscale scheduling branch' }
$conditionStart = $routing.LastIndexOf('if (', $postPass)
$conditionBody = $conditionStart + 4
$depth = 1
$conditionEnd = $conditionBody
while ($depth -gt 0 -and $conditionEnd -lt $routing.Length) {
    if ($routing[$conditionEnd] -eq '(') { ++$depth }
    if ($routing[$conditionEnd] -eq ')') { --$depth }
    ++$conditionEnd
}
if ($conditionStart -lt 0 -or $depth -ne 0) { throw 'Unbalanced post-upscale scheduling condition' }
$postCondition = $routing.Substring($conditionBody, $conditionEnd - $conditionBody - 1)
$routingMethods += 'const bool nrAfterUpscale = ' + $postCondition + ";`n"
}
[IO.File]::WriteAllText((Join-Path $OutputDirectory 'production-routing.inc'), $routingMethods)
foreach ($path in @($DenoiseSource, $deferredPath, $statusPath, $RoutingSource, $runPath)) {
    Write-Output ("SOURCE {0} SHA256 {1}" -f $path, (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash)
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Visual Studio C++ build tools are required.' }
$buildCmd = @"
@echo off
call "$vs/Common7/Tools/VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /I "$OutputDirectory" "$PSScriptRoot/PrivateSeamTests.cpp" /Fe:"$OutputDirectory/nr-private-seam.exe" /Fo:"$OutputDirectory/nr-private-seam.obj"
"@
[IO.File]::WriteAllText((Join-Path $OutputDirectory 'build.cmd'), $buildCmd)
& (Join-Path $OutputDirectory 'build.cmd')
if ($LASTEXITCODE) { throw 'NR private seam CPU compilation failed.' }
$testOutput = @(& (Join-Path $OutputDirectory 'nr-private-seam.exe'))
$testExit = $LASTEXITCODE
[IO.File]::WriteAllLines((Join-Path $OutputDirectory 'test-results.txt'), [string[]]$testOutput)
$testOutput | ForEach-Object { Write-Host $_ }
Stop-Transcript | Out-Null
exit $testExit
