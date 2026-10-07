param([Parameter(Mandatory=$true)][string]$OutputDirectory, [string]$Label = 'initial-state')
$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$denoisePath = "$repo/OptiScaler/shaders/dlssnr/DlssNr_Dx12_DenoiseFirst.cpp"
$resourcesPath = "$repo/OptiScaler/shaders/dlssnr/DlssNr_Dx12_Resources.cpp"
$statePath = "$repo/OptiScaler/shaders/dlssnr/DlssNr_Dx12_State.h"
$denoise = Get-Content -Raw -LiteralPath $denoisePath
$resources = Get-Content -Raw -LiteralPath $resourcesPath
$state = Get-Content -Raw -LiteralPath $statePath
function ExtractFunction([string]$source, [string]$signature) {
    $start = $source.IndexOf($signature)
    if ($start -lt 0) { throw "Missing function: $signature" }
    $open = $source.IndexOf('{', $start)
    $end = $open + 1; $depth = 1
    while ($depth -and $end -lt $source.Length) {
        if ($source[$end] -eq '{') { ++$depth }
        if ($source[$end] -eq '}') { --$depth }
        ++$end
    }
    if ($depth) { throw 'Unbalanced production function' }
    $source.Substring($start, $end-$start)
}
$declStart = $state.IndexOf('ID3D12Resource* CreateScratch(')
$declEnd = $state.IndexOf(';', $declStart)
if ($declStart -lt 0 -or $declEnd -le $declStart) { throw 'CreateScratch declaration missing' }
$lazyStart = $denoise.IndexOf('    if (step == EditOntoRaw && !g.composite)')
$lazyEnd = $denoise.IndexOf('    if (step == PrivateSr && !g.enlarger)', $lazyStart)
if ($lazyStart -lt 0 -or $lazyEnd -le $lazyStart) { throw 'Lazy allocation boundaries missing' }
$generated = (Get-Content -Raw -LiteralPath "$PSScriptRoot/InitialStateTests.cpp.in").
    Replace('// @SCRATCH_DECL@', $state.Substring($declStart, $declEnd-$declStart+1)).
    Replace('// @SCRATCH_HELPER@', (ExtractFunction $resources 'auto DlssNr_Dx12::State::CreateScratch(')).
    Replace('// @COLOR_HELPER@', (ExtractFunction $denoise 'ID3D12Resource* CreateColorStandIn(')).
    Replace('// @LAZY_ALLOCATIONS@', $denoise.Substring($lazyStart, $lazyEnd-$lazyStart))
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$out = (Resolve-Path -LiteralPath $OutputDirectory).Path
[IO.File]::WriteAllText("$out/$Label.cpp", $generated)
$sources = @($denoisePath, $resourcesPath, $statePath) | ForEach-Object { "$($_) SHA256 $((Get-FileHash -LiteralPath $_).Hash)" }
[IO.File]::WriteAllLines("$out/$Label-sources.txt", [string[]]$sources)
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Visual Studio C++ Build Tools unavailable' }
$build = @"
@echo off
call "$vs/VC/Auxiliary/Build/vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 "$out/$Label.cpp" /Fe:"$out/$Label.exe" /Fo:"$out/$Label.obj"
"@
[IO.File]::WriteAllText("$out/$Label-build.cmd", $build)
& "$out/$Label-build.cmd" 2>&1 | Tee-Object -FilePath "$out/$Label-build.txt"
if ($LASTEXITCODE) { throw 'Initial-state CPU harness build failed' }
& "$out/$Label.exe" 2>&1 | Tee-Object -FilePath "$out/$Label-results.txt"
exit $LASTEXITCODE
