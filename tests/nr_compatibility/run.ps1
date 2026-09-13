# Requires a Visual Studio developer PowerShell and an NVIDIA GPU. Uses supplied local DLLs only.
param(
    [Parameter(Mandatory)][string]$Driver,
    [Parameter(Mandatory)][string]$RuntimeDirectory,
    [string[]]$AdditionalRuntimeDirectories = @(),
    [string[]]$NonRuntimeDirectories = @()
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot/../..").Path
$build = Join-Path ([IO.Path]::GetTempPath()) ('nr-compatibility-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path $build | Out-Null
foreach ($header in @('pch.h', 'Logger.h')) {
    Set-Content -LiteralPath "$build/$header" -Value '// Dependencies supplied by Adapter.h.'
}
& cl.exe /nologo /std:c++20 /EHsc /O2 /MD /W4 "$PSScriptRoot/ImportTests.cpp" "/Fo$build/imports.obj" "/Fe:$build/imports.exe"
if ($LASTEXITCODE) { throw 'Import test compilation failed.' }
& "$build/imports.exe"
if ($LASTEXITCODE) { throw 'Import regression failed.' }
& cl.exe /nologo /std:c++20 /EHsc /O2 /MD /W4 "/FI$PSScriptRoot/Adapter.h" "/I$build" `
    "/I$repo/external/nvngx_dlss_sdk" "$PSScriptRoot/HardwareSmoke.cpp" `
    "$repo/OptiScaler/dlssnr/DlssNr_CompatibilityRuntime.cpp" "/Fo$build/" "/Fe:$build/smoke.exe" `
    /link d3d12.lib dxgi.lib
if ($LASTEXITCODE) { throw 'Compatibility smoke compilation failed.' }
New-Item -ItemType Directory -Path "$build/non-runtime" | Out-Null
Set-Content -LiteralPath "$build/non-runtime/fixture.cpp" -Value 'extern "C" __declspec(dllexport) int Fixture() { return 1; }'
& cl.exe /nologo /LD /MD "$build/non-runtime/fixture.cpp" "/Fo$build/non-runtime/" "/Fe:$build/non-runtime/nvngx_dlssnr.dll"
if ($LASTEXITCODE) { throw 'Non-runtime fixture compilation failed.' }
foreach ($directory in (@("$build/non-runtime") + $NonRuntimeDirectories)) {
    & "$build/smoke.exe" $Driver $directory --reject
    if ($LASTEXITCODE) { throw "Runtime rejection test failed: $directory" }
}
& "$build/smoke.exe" $Driver $RuntimeDirectory
if ($LASTEXITCODE) { throw 'Compatibility GPU smoke failed.' }
& "$build/smoke.exe" $Driver $RuntimeDirectory --preloaded
if ($LASTEXITCODE) { throw 'Preloaded/wrapped runtime smoke failed.' }
& "$build/smoke.exe" $Driver $RuntimeDirectory --owners
if ($LASTEXITCODE) { throw 'Concurrent owner acquisition failed.' }
foreach ($directory in $AdditionalRuntimeDirectories) {
    & "$build/smoke.exe" $Driver $directory --direct
    if ($LASTEXITCODE) { throw "Direct-runtime GPU smoke failed: $directory" }
}
