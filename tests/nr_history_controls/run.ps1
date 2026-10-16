param([string]$OutputDirectory = (Join-Path $PSScriptRoot 'build'), [string]$BaselineRevision)
$ErrorActionPreference = 'Stop'
$nrRepo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$nrSource = if ($BaselineRevision) {
    (& git -C $nrRepo show "${BaselineRevision}:OptiScaler/shaders/dlssnr/DlssNr_Dx12_Status.cpp") -join "`n"
} else { Get-Content -Raw -LiteralPath (Join-Path $nrRepo 'OptiScaler/shaders/dlssnr/DlssNr_Dx12_Status.cpp') }
if ($BaselineRevision -and $LASTEXITCODE) { throw 'Cannot read baseline.' }
$nrStart = $nrSource.IndexOf('auto DlssNr_Dx12::State::ConsumeControls()')
$nrStart = $nrSource.IndexOf('{', $nrStart) + 1
$nrEnd = $nrSource.IndexOf('auto DlssNr_Dx12::State::Publish()', $nrStart)
if ($nrStart -lt 1 -or $nrEnd -lt 0) { throw 'Control method extraction failed.' }
$nrBody = $nrSource.Substring($nrStart, $nrEnd - $nrStart)
$nrBody = $nrBody.Substring(0, $nrBody.LastIndexOf('}'))
[IO.File]::WriteAllText((Join-Path $OutputDirectory 'consume-controls.inc'), $nrBody)
$nrVs = & "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$nrVs) { throw 'Visual Studio C++ tools required.' }
$nrBuild = @"
@echo off
call "$nrVs/Common7/Tools/VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /I"$OutputDirectory" /I"$PSScriptRoot" /Fo"$OutputDirectory/HistoryControlsTests.obj" /Fe"$OutputDirectory/HistoryControlsTests.exe" "$PSScriptRoot/HistoryControlsTests.cpp"
"@
[IO.File]::WriteAllText((Join-Path $OutputDirectory 'build.cmd'), $nrBuild)
& (Join-Path $OutputDirectory 'build.cmd') | Tee-Object -FilePath (Join-Path $OutputDirectory 'build.log')
if ($LASTEXITCODE) { throw 'History-control suite did not compile.' }
& (Join-Path $OutputDirectory 'HistoryControlsTests.exe') | Tee-Object -FilePath (Join-Path $OutputDirectory 'results.log')
exit $LASTEXITCODE
