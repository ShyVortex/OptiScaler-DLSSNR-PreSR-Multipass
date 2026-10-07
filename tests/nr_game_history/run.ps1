param(
    [string]$Repo = (Join-Path $PSScriptRoot '../..'),
    [string]$Source = '',
    [string]$OutputDirectory = (Join-Path $PSScriptRoot 'build')
)
$ErrorActionPreference = 'Stop'
$Repo = (Resolve-Path -LiteralPath $Repo).Path
if (!$Source) { $Source = Join-Path $Repo 'OptiScaler/upscalers/IFeature_Dx12.cpp' }
$Source = (Resolve-Path -LiteralPath $Source).Path
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
Start-Transcript -Path (Join-Path $OutputDirectory 'test-run.txt') -Force | Out-Null
$production = Get-Content -Raw -LiteralPath $Source
$start = $production.IndexOf('UpscalerTime->Start(InCommandList);')
$end = $production.IndexOf('if (denoiseFirst)', $start)
if ($start -lt 0 -or $end -lt 0) { throw 'Missing game evaluate/reset boundary' }
[IO.File]::WriteAllText((Join-Path $OutputDirectory 'production-evaluate.inc'), $production.Substring($start, $end - $start))
Write-Output ("SOURCE {0} SHA256 {1}" -f $Source, (Get-FileHash -Algorithm SHA256 -LiteralPath $Source).Hash)
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Visual Studio C++ Build Tools are required.' }
$buildCmd = @"
@echo off
call "$vs/Common7/Tools/VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /I "$OutputDirectory" "$PSScriptRoot/GameHistoryTests.cpp" /Fe:"$OutputDirectory/nr-game-history.exe" /Fo:"$OutputDirectory/nr-game-history.obj"
"@
[IO.File]::WriteAllText((Join-Path $OutputDirectory 'build.cmd'), $buildCmd)
& (Join-Path $OutputDirectory 'build.cmd')
if ($LASTEXITCODE) { throw 'Game history CPU test compilation failed.' }
$testOutput = @(& (Join-Path $OutputDirectory 'nr-game-history.exe'))
$testExit = $LASTEXITCODE
[IO.File]::WriteAllLines((Join-Path $OutputDirectory 'test-results.txt'), [string[]]$testOutput)
$testOutput | ForEach-Object { Write-Host $_ }
Stop-Transcript | Out-Null
exit $testExit
