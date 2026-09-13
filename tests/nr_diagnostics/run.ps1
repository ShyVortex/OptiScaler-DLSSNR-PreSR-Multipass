# Run from a Visual Studio developer PowerShell. No NVIDIA runtime or game required.
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$out = Join-Path ([IO.Path]::GetTempPath()) ('nr-diagnostics-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path "$out/proxies" -Force | Out-Null
foreach ($header in @('pch.h', 'Config.h', 'State.h', 'Util.h', 'proxies/NVNGX_Proxy.h')) {
    Set-Content -LiteralPath "$out/$header" -Value '// Supplied by Adapter.h.'
}
& cl.exe /nologo /std:c++20 /EHsc /MD /DNOMINMAX "/FI$PSScriptRoot/Adapter.h" "/I$out" `
    "/I$repo/external/nvngx_dlss_sdk" "$PSScriptRoot/CallbackTests.cpp" `
    "$repo/OptiScaler/dlssnr/DlssNr_NgxDiagnostics.cpp" "/Fe:$out/tests.exe" "/Fo:$out/" /link d3d12.lib dxgi.lib
if ($LASTEXITCODE) { throw 'Diagnostics test compilation failed.' }
& "$out/tests.exe"
if ($LASTEXITCODE) { throw 'Diagnostics regression failed.' }
