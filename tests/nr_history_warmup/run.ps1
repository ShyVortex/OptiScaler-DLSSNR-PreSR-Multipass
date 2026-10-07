param([string]$Repo = (Join-Path $PSScriptRoot '../..'), [string]$OutputDirectory = (Join-Path $PSScriptRoot 'build'))
$ErrorActionPreference = 'Stop'
$Repo = (Resolve-Path -LiteralPath $Repo).Path
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
# Reuse the production extraction and routing compiler already exercised by the seam suite.
& (Join-Path $PSScriptRoot '../nr_private_seam/run.ps1') -Repo $Repo -OutputDirectory $OutputDirectory
if ($LASTEXITCODE) { throw 'Prerequisite seam suite failed.' }
Start-Transcript -Path (Join-Path $OutputDirectory 'warmup-run.txt') -Force | Out-Null
$modelsPath = Join-Path $Repo 'OptiScaler/shaders/dlssnr/DlssNr_Dx12_Models.cpp'
$models = Get-Content -Raw -LiteralPath $modelsPath
$start = $models.IndexOf('const bool resolutionChanged =')
$end = $models.IndexOf('if (cropColor && nr.activeColor == nullptr)', $start)
if ($start -lt 0 -or $end -lt 0) { throw 'Missing actual model preparation retry/allocation prefix' }
[IO.File]::WriteAllText((Join-Path $OutputDirectory 'production-model-preparation.inc'), $models.Substring($start, $end-$start))
$beforePath = Join-Path $Repo 'OptiScaler/shaders/dlssnr/DlssNr_Dx12_DenoiseFirst.cpp'
$before = Get-Content -Raw -LiteralPath $beforePath
$call = [regex]::Match($before, '(?:handoff\.(?:modelAttempted|ownsNrHistory)\s*=\s*true;\s*)?owner\.Run\(cmd,[\s\S]*?;')
if (!$call.Success) { throw 'Missing private NR invocation' }
[IO.File]::WriteAllText((Join-Path $OutputDirectory 'production-private-invocation.inc'), $call.Value)
Write-Output ("SOURCE {0} SHA256 {1}" -f $modelsPath, (Get-FileHash -Algorithm SHA256 -LiteralPath $modelsPath).Hash)
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$buildCmd = @"
@echo off
call "$vs/Common7/Tools/VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /I "$OutputDirectory" "$PSScriptRoot/WarmupTests.cpp" /Fe:"$OutputDirectory/nr-history-warmup.exe" /Fo:"$OutputDirectory/nr-history-warmup.obj"
"@
[IO.File]::WriteAllText((Join-Path $OutputDirectory 'warmup-build.cmd'), $buildCmd)
& (Join-Path $OutputDirectory 'warmup-build.cmd')
if ($LASTEXITCODE) { throw 'Warmup CPU compilation failed.' }
$results = @(& (Join-Path $OutputDirectory 'nr-history-warmup.exe'))
$testExit = $LASTEXITCODE
[IO.File]::WriteAllLines((Join-Path $OutputDirectory 'warmup-results.txt'), [string[]]$results)
$results | ForEach-Object { Write-Host $_ }
Stop-Transcript | Out-Null
exit $testExit
