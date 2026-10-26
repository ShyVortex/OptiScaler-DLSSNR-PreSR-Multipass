$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$build = Join-Path $repo 'x64/nr-streamline-hooks'
New-Item -ItemType Directory -Force $build | Out-Null
# Compile the actual wrapper body against test application services, with real DXGI/D3D12 objects.
$source = Get-Content -Raw -LiteralPath "$repo/OptiScaler/dlssnr/DlssNr_StreamlinePicture.cpp"
Set-Content -LiteralPath "$build/StreamlinePicture.cpp" -Value ([regex]::Replace($source, '(?m)^#include[^\r\n]*', ''))
& cl.exe /nologo /std:c++20 /EHsc /W4 /DNOMINMAX "/I$build" "/Fo$build/" "/Fe$build/hooks.exe" `
    "$PSScriptRoot/nr_streamline_hooks.cpp" /link d3d12.lib dxgi.lib dxguid.lib
if ($LASTEXITCODE) { throw 'Streamline hook test build failed.' }
& "$build/hooks.exe"
if ($LASTEXITCODE) { throw 'Streamline hook test failed.' }
