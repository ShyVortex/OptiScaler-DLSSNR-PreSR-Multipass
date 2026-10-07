param([Parameter(Mandatory=$true)][string]$OutputDirectory, [string]$Label = 'guide-validation')
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$source = Get-Content -Raw -LiteralPath "$repo/OptiScaler/shaders/dlssnr/DlssNr_Upscaler_Dx12.cpp"
$header = Get-Content -Raw -LiteralPath "$repo/OptiScaler/shaders/dlssnr/DlssNr_Upscaler_Dx12.h"
# Extract actual production bodies through the first GPU boundary. No source-presence assertions.
$structStart = $header.IndexOf('namespace DlssNr')
$structEnd = $header.IndexOf('// One independent history')
$initStart = $source.IndexOf('        width = info.width;', $source.IndexOf('    bool Init('))
$initEnd = $source.IndexOf('        if (backend == PrivateUpscaler::DLSS)', $initStart)
$evalStart = $source.IndexOf('        auto* color = f.color.resource;', $source.IndexOf('    bool Evaluate('))
$evalEnd = $source.IndexOf('        bool result = false;', $evalStart)
if ($structStart -lt 0 -or $structEnd -le $structStart -or $initStart -lt 0 -or $initEnd -le $initStart -or
    $evalStart -lt 0 -or $evalEnd -le $evalStart) { throw 'Production extraction boundaries not found.' }
$generated = (Get-Content -Raw -LiteralPath "$PSScriptRoot/nr_private_guide_validation.cpp.in").
    Replace('@STRUCTS@', $header.Substring($structStart, $structEnd-$structStart)).
    Replace('@INIT@', $source.Substring($initStart, $initEnd-$initStart)).
    Replace('@EVALUATE@', $source.Substring($evalStart, $evalEnd-$evalStart))
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$out = (Resolve-Path -LiteralPath $OutputDirectory).Path
[IO.File]::WriteAllText("$out/$Label.cpp", $generated)
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$installation) { throw 'Visual Studio C++ build tools not found.' }
$vcvars = Join-Path $installation 'VC/Auxiliary/Build/vcvars64.bat'
$build = @"
@echo off
call "$vcvars" >nul
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 "$out/$Label.cpp" /Fe:"$out/$Label.exe" /Fo:"$out/$Label.obj"
"@
[IO.File]::WriteAllText("$out/$Label-build.cmd", $build)
& "$out/$Label-build.cmd" 2>&1 | Tee-Object -FilePath "$out/$Label-build.txt"
if ($LASTEXITCODE) { throw 'CPU validation harness build failed.' }
& "$out/$Label.exe" 2>&1 | Tee-Object -FilePath "$out/$Label-results.txt"
exit $LASTEXITCODE
