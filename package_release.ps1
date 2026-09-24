# Assemble a release zip.
#
# The build output directory is not the release: it also holds import libraries, export files, debug
# symbols, and whatever earlier experiments left behind. Shipping that folder wholesale is how a
# release ends up containing a DLL nobody meant to publish, so this copies an explicit list and
# refuses anything not on it.
#
# What is deliberately NOT here: nvngx_dlssnr.dll. That is NVIDIA's, it is not ours to redistribute,
# and the user supplies their own copy per game folder. Only the ~108 KB forwarder ships.

param(
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]*$')]
    [string]$Version = "v0.9.20",
    [switch]$SkipBuild,
    [switch]$IncludeDlssFrameGeneration,
    [switch]$AcceptNvidiaLicenses,
    [switch]$IncludeAmpereMfg,
    [switch]$AcceptAmpereMfgLicenses,
    [switch]$UpdateAmpereMfg,
    [string]$HybridAssetsDirectory,
    [string]$StreamlineArchive
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSCommandPath
$flavour = if ($IncludeDlssFrameGeneration) { '-with-dlss-fg' } else { '' }
if ($IncludeAmpereMfg) { $flavour += '-with-sm86-mfg' }

$stage = "$root\release\$Version$flavour"
$zip = "$root\release\OptiScaler-DLSSNR-$Version$flavour.zip"
if ((Test-Path -LiteralPath $stage) -or (Test-Path -LiteralPath $zip)) {
    throw 'Release output already exists. Choose a new -Version; existing packages are never deleted or overwritten.'
}
if ($IncludeDlssFrameGeneration -and -not $AcceptNvidiaLicenses) {
    throw 'Bundling NVIDIA binaries requires -AcceptNvidiaLicenses. Read docs/DLSS-FRAME-GENERATION.md first.'
}
if ($IncludeDlssFrameGeneration) {
    Write-Warning 'LOCAL USE ONLY: this DLL-containing package has not been cleared for redistribution. Publish the downloader-only variant instead; see docs/DLSS-FRAME-GENERATION.md.'
}
if ($IncludeAmpereMfg -and -not $AcceptAmpereMfgLicenses) {
    throw 'Bundling Ampere SM86 MFG binaries requires -AcceptAmpereMfgLicenses.'
}
if ($IncludeAmpereMfg) {
    Write-Warning 'Ampere SM86 MFG proxy DLL will be bundled from dlssg_for_sm86.'
}

if (-not $SkipBuild) {
    $msb = (Get-Command MSBuild.exe -ErrorAction SilentlyContinue).Source
    if (-not $msb) {
        $msb = @(
            "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe",
            "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe",
            "C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe",
            "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe"
        ) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    }
    if (-not $msb) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
        if (Test-Path -LiteralPath $vswhere) {
            $msb = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild/Current/Bin/MSBuild.exe'
        }
    }
    if (-not $msb) {
        throw 'MSBuild.exe was not found. Install Visual Studio C++ build tools or use -SkipBuild with a verified existing build.'
    }

    $forwarderProj = "$root\OptiScaler\dlssnr\forwarder\dlssnr_forwarder.vcxproj"
    if (Test-Path -LiteralPath $forwarderProj) {
        $out = & $msb $forwarderProj /p:Configuration=Release /p:Platform=x64 /v:minimal /m 2>&1
        $err = $out | Select-String "error "
        if ($err) { Write-Host "FAILED: $forwarderProj"; $err | Select-Object -First 6; exit 1 }
    }

    $out = & $msb (Join-Path $root 'OptiScaler.sln') /p:Configuration=Release /p:Platform=x64 /p:PostBuildEventUseInBuild=false /v:minimal /m 2>&1
    $err = $out | Select-String "error "
    if ($err) { Write-Host "FAILED: OptiScaler.sln"; $err | Select-Object -First 6; exit 1 }
    Write-Host "built"
}

$src = "$root\x64\Release\a"

$forwarder = "$root\OptiScaler\dlssnr\forwarder\x64\Release\a\nvngx.dll_dlssnr.dll"

if (-not (Test-Path $forwarder)) {
    $forwarder = "$root\x64\Release\a\nvngx.dll_dlssnr.dll"
    if (Test-Path $forwarder) {
        Write-Host "forwarder: using the shared build output ($forwarder)"
    }
}

if (Test-Path $forwarder) {
    $exports = @("dlssnr_call_create", "dlssnr_call_evaluate_v2", "dlssnr_call_set_extras",
                 "dlssnr_vk_probe", "dlssnr_vk_init", "dlssnr_vk_create", "dlssnr_vk_evaluate_v2", "dlssnr_vk_release")
    $bytes = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($forwarder))
    $missing = @($exports | Where-Object { $bytes.IndexOf($_) -lt 0 })

    if ($missing.Count -gt 0) {
        Write-Host "STALE forwarder: missing $($missing -join ', ')"
        exit 1
    }
}

New-Item -ItemType Directory -Force -Path $stage | Out-Null

$buildFiles = @(
    "OptiScaler.dll",
    "!! EXTRACT ALL FILES TO GAME FOLDER !!"
)

$sourceFiles = @(
    "OptiScaler.ini",
    "setup_windows.bat",
    "setup_linux.sh",
    "README.md",
    "INSTALL-DLSSNR.md",
    "LICENSE",
    "Features.md",
    "Config.md",
    "Spoofing.md",
    "CONTRIBUTING.md"
)

foreach ($optionalDoc in @("get_streamline.ps1", "images/gh-sponsor-red.png", "images/bmac.png",
                          "OptiScaler/dlssnr/README.md", "tests/nr_private_upscaler_smoke.md")) {
    if (Test-Path -LiteralPath "$root\$optionalDoc") {
        $sourceFiles += $optionalDoc
    }
}

foreach ($f in $buildFiles) {
    $source = "$src\$f"
    if (-not (Test-Path -LiteralPath $source)) {
        # Fall back to root or buildFolder if post-build layout differs
        $sourceAlt = "$root\x64\Release\$f"
        if (Test-Path -LiteralPath $sourceAlt) {
            $source = $sourceAlt
        } elseif ($f -eq "!! EXTRACT ALL FILES TO GAME FOLDER !!") {
            [IO.File]::WriteAllText((Join-Path $stage $f), '')
            continue
        } else {
            throw "Required build output is missing: $source"
        }
    }
    Copy-Item -LiteralPath $source -Destination "$stage\$f" -Force
}

foreach ($f in $sourceFiles) {
    $source = "$root\$f"
    if (Test-Path -LiteralPath $source) {
        $dest = "$stage\$f"
        New-Item -ItemType Directory -Path (Split-Path -Parent $dest) -Force | Out-Null
        Copy-Item -LiteralPath $source -Destination $dest -Force
    }
}

foreach ($d in @("Licenses", "OptiScaler")) {
    $source = "$src\$d"
    if (Test-Path -LiteralPath $source -PathType Container) {
        Copy-Item -LiteralPath $source -Destination "$stage\$d" -Recurse -Force
    }
}

# Ensure core external libraries are staged even if post-build copy didn't run
New-Item -ItemType Directory -Force -Path "$stage\OptiScaler" | Out-Null
New-Item -ItemType Directory -Force -Path "$stage\Licenses" | Out-Null

foreach ($name in @('libxess.dll', 'libxess_dx11.dll', 'libxell.dll', 'libxess_fg.dll')) {
    $extPath = "$root\external\xess\bin\$name"
    if ((Test-Path -LiteralPath $extPath) -and -not (Test-Path -LiteralPath "$stage\OptiScaler\$name")) {
        Copy-Item -LiteralPath $extPath -Destination "$stage\OptiScaler\$name" -Force
    }
}
$vkSdkPath = "$root\external\FidelityFX-SDK\PrebuiltSignedDLL\amd_fidelityfx_vk.dll"
if ((Test-Path -LiteralPath $vkSdkPath) -and -not (Test-Path -LiteralPath "$stage\OptiScaler\amd_fidelityfx_vk.dll")) {
    Copy-Item -LiteralPath $vkSdkPath -Destination "$stage\OptiScaler\amd_fidelityfx_vk.dll" -Force
}
foreach ($name in @('amd_fidelityfx_loader_dx12.dll', 'amd_fidelityfx_upscaler_dx12.dll', 'amd_fidelityfx_framegeneration_dx12.dll')) {
    $ffxPath = "$root\external\FidelityFX-SDK-v2\Kits\FidelityFX\signedbin\$name"
    if ((Test-Path -LiteralPath $ffxPath) -and -not (Test-Path -LiteralPath "$stage\OptiScaler\$name")) {
        Copy-Item -LiteralPath $ffxPath -Destination "$stage\OptiScaler\$name" -Force
    }
}
$d3d12CorePath = "$root\external\directx_agility_sdk\lib\D3D12Core.dll"
if (Test-Path -LiteralPath $d3d12CorePath) {
    New-Item -ItemType Directory -Force -Path "$stage\OptiScaler\D3D12_OptiScaler" | Out-Null
    Copy-Item -LiteralPath $d3d12CorePath -Destination "$stage\OptiScaler\D3D12_OptiScaler\D3D12Core.dll" -Force
}

# Licenses
$licMap = @{
    'external/xess/LICENSE.txt' = 'Licenses/XeSS_LICENSE.txt'
    'external/FidelityFX-SDK/docs/license.md' = 'Licenses/FidelityFX_v1_LICENSE.md'
    'external/FidelityFX-SDK-v2/docs/license.md' = 'Licenses/FidelityFX_v2_LICENSE.md'
    'external/directx_agility_sdk/LICENSE.txt' = 'Licenses/DirectX_LICENSE.txt'
    'Licenses/RenoDX_ATTRIBUTION.txt' = 'Licenses/RenoDX_ATTRIBUTION.txt'
}
foreach ($entry in $licMap.GetEnumerator()) {
    $srcPath = Join-Path $root $entry.Key
    $dstPath = Join-Path $stage $entry.Value
    if (Test-Path -LiteralPath $srcPath) {
        New-Item -ItemType Directory -Path (Split-Path -Parent $dstPath) -Force | Out-Null
        Copy-Item -LiteralPath $srcPath -Destination $dstPath -Force
    }
}

if (Test-Path $forwarder) {
    Copy-Item $forwarder "$stage\nvngx.dll_dlssnr.dll" -Force
}
if (Test-Path -LiteralPath "$root\docs") {
    Copy-Item -LiteralPath "$root\docs" -Destination "$stage\docs" -Recurse -Force
}
if (Test-Path -LiteralPath "$root\redist\streamline\manifest.json") {
    New-Item -ItemType Directory -Path "$stage\redist\streamline" -Force | Out-Null
    Copy-Item -LiteralPath "$root\redist\streamline\manifest.json" -Destination "$stage\redist\streamline\manifest.json"
}

if (Test-Path -LiteralPath "$stage\OptiScaler\streamline") {
    throw 'REFUSING: the build output contains an unmanaged Streamline stack. Move it aside and use -IncludeDlssFrameGeneration.'
}
if ($IncludeDlssFrameGeneration -and (Test-Path -LiteralPath "$root\get_streamline.ps1")) {
    & "$root\get_streamline.ps1" -Destination "$stage\OptiScaler\streamline" `
        -ArchivePath $StreamlineArchive -AcceptNvidiaLicenses
}

# Logging on, in the release only.
$iniPath = "$stage\OptiScaler.ini"
$ini = Get-Content $iniPath -Raw
$ini = $ini -replace '(?m)^LogToFile=auto', 'LogToFile=true'
$ini = $ini -replace '(?m)^LogLevel=auto', 'LogLevel=2'

Set-Content $iniPath $ini -Encoding utf8 -NoNewline

$check = Select-String -Path $iniPath -Pattern '^LogToFile=|^LogLevel=' | ForEach-Object { $_.Line }
Write-Host "log settings: $($check -join ', ')"

$targetProcess = Select-String -Path $iniPath -Pattern '^TargetProcessName=' | Select-Object -First 1
if ($targetProcess.Line -ne 'TargetProcessName=auto') {
    throw "REFUSING: portable package has a game-specific process filter: $($targetProcess.Line)"
}
Write-Host "process filter: portable (TargetProcessName=auto)"

Get-ChildItem $stage -Recurse -Include *.exp, *.lib, *.pdb, *.ilk, *latewarp* | Remove-Item -Force

$on = Select-String -Path "$stage\OptiScaler.ini" -Pattern '^Enabled=true'
if ($on) {
    Write-Host "REFUSING: the packaged ini has features switched on:"
    $on | ForEach-Object { "  line $($_.LineNumber): $($_.Line)" }
    exit 1
}
Write-Host "ini verified: nothing switched on by default"

foreach ($key in @('FinishedPicture', 'DeferredDLSS', 'ResidualFG', 'ResidualFGApproxCamera', 'UnlockPasses', 'AmpereMfgUnlock')) {
    if ($ini -match "(?mi)^$key=true\s*$") {
        throw "REFUSING: experimental option $key is enabled in the portable package"
    }
}

if (Get-ChildItem -LiteralPath $stage -Recurse -File | Where-Object { $_.Name -ieq 'nvngx_dlssnr.dll' }) {
    throw 'REFUSING: proprietary nvngx_dlssnr.dll is present in the staging directory'
}
if (-not $IncludeDlssFrameGeneration) {
    $nvidiaRuntime = Get-ChildItem -LiteralPath $stage -Recurse -File | Where-Object {
        $_.Name -match '^(nvngx_dlss.*|sl\..*)\.dll$'
    }
    if ($nvidiaRuntime) { throw 'REFUSING: NVIDIA runtime DLLs found in the public downloader-only package' }
}

$crossGenHash = 'E67DEE209320CDAFE0E93E45675D7AA34323A53ACC57A72B2E40A181581C989A'
if (Test-Path -LiteralPath "$stage\INSTALL-DLSSNR.md") {
    if ((Get-Content -LiteralPath "$stage\INSTALL-DLSSNR.md" -Raw).IndexOf($crossGenHash, [StringComparison]::OrdinalIgnoreCase) -lt 0) {
        throw "REFUSING: cross-generation runtime hash is missing from INSTALL-DLSSNR.md"
    }
    Write-Host "cross-generation guidance: present and hash-pinned"
}

if ($HybridAssetsDirectory) {
    $manifest = Get-Content -LiteralPath (Join-Path $HybridAssetsDirectory 'asset-manifest.json') -Raw | ConvertFrom-Json
    foreach ($item in $manifest.files) {
        $assetRoot = [IO.Path]::GetFullPath((Join-Path $HybridAssetsDirectory 'OptiScaler/nvfp4/hybrid'))
        $source = [IO.Path]::GetFullPath((Join-Path $assetRoot $item.path))
        if (-not $source.StartsWith($assetRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid hybrid asset path' }
        if ((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ne $item.sha256) { throw "Hybrid asset hash mismatch: $source" }
        $target = Join-Path "$stage/OptiScaler/nvfp4/hybrid" $item.path
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
        Copy-Item -LiteralPath $source -Destination $target
    }
}

if ($IncludeAmpereMfg) {
    $sm86Src = "$root\dlssg_for_sm86"
    $sm86Dll = "$sm86Src\version.dll"
    if (-not (Test-Path -LiteralPath $sm86Dll) -and (Test-Path -LiteralPath "$sm86Src\sdli1995\version.dll")) {
        $sm86Src = "$sm86Src\sdli1995"
        $sm86Dll = "$sm86Src\version.dll"
    }
    if ($UpdateAmpereMfg -or (-not (Test-Path -LiteralPath $sm86Dll))) {
        Write-Host "Acquiring verified SM75/SM86 0.3.5 runtime from upstream..."
        if (-not (Test-Path -LiteralPath $sm86Src)) {
            New-Item -ItemType Directory -Force -Path $sm86Src | Out-Null
        }
        if (-not (Test-Path -LiteralPath "$sm86Src\.git")) {
            git init $sm86Src
            git -C $sm86Src remote add origin https://github.com/sdli1995/dlssg_for_sm86.git
        }
        git -C $sm86Src fetch --depth 1 origin 9621db573e07ed54f50c15bbb585ed9a7bdfac28
        if ($LASTEXITCODE -ne 0) { throw 'Cannot fetch pinned SM75/SM86 0.3.5 runtime' }
        git -C $sm86Src checkout --detach FETCH_HEAD
    }
    if (-not (Test-Path -LiteralPath $sm86Dll)) {
        throw "Ampere/Turing SM86/SM75 binary not found at $sm86Dll"
    }
    $sm86DestDir = "$stage\OptiScaler\dlssg_sm86"
    New-Item -ItemType Directory -Force -Path $sm86DestDir | Out-Null
    Copy-Item -LiteralPath $sm86Dll -Destination "$sm86DestDir\dlssg_sm86.dll"
    $sm86Ini = "$sm86Src\dlssg_sm86.ini"
    if (Test-Path -LiteralPath $sm86Ini) {
        Copy-Item -LiteralPath $sm86Ini -Destination "$sm86DestDir\dlssg_sm86.ini"
    }
    $sm86ReshadeIni = "$sm86Src\ReShade.ini"
    if (Test-Path -LiteralPath $sm86ReshadeIni) {
        Copy-Item -LiteralPath $sm86ReshadeIni -Destination "$sm86DestDir\ReShade.ini"
    }
    $sm863101Dll = "$sm86Src\310.1\version.dll"
    if (Test-Path -LiteralPath $sm863101Dll) {
        $sm863101DestDir = "$sm86DestDir\310.1"
        New-Item -ItemType Directory -Force -Path $sm863101DestDir | Out-Null
        Copy-Item -LiteralPath $sm863101Dll -Destination "$sm863101DestDir\dlssg_sm86.dll"
    }
    $notices = "$sm86Src\THIRD_PARTY_NOTICES.txt"
    if (Test-Path -LiteralPath $notices) {
        Copy-Item -LiteralPath $notices -Destination "$sm86DestDir\THIRD_PARTY_NOTICES.txt"
    }
    $readme = "$sm86Src\README.en.md"
    if (Test-Path -LiteralPath $readme) {
        Copy-Item -LiteralPath $readme -Destination "$sm86DestDir\README.en.md"
    }
    $docs = "$sm86Src\docs"
    if (Test-Path -LiteralPath $docs) {
        Copy-Item -LiteralPath $docs -Destination "$sm86DestDir\docs" -Recurse -Force
    }
    Write-Host "RTX 20/30 (SM75/SM86) MFG: dlssg_sm86.dll, dlssg_sm86.ini, 310.1 runtime (if present), documentation, and notices staged"
}

$checksumLines = Get-ChildItem -LiteralPath $stage -Recurse -File |
    Where-Object { $_.Name -ne 'SHA256SUMS.txt' } |
    Sort-Object FullName |
    ForEach-Object {
        $stageClean = $stage.TrimEnd('\', '/')
        $relative = if ($_.FullName.StartsWith($stageClean, [System.StringComparison]::OrdinalIgnoreCase)) {
            $_.FullName.Substring($stageClean.Length).TrimStart('\', '/').Replace('\', '/')
        } else {
            $_.Name
        }
        "{0} *{1}" -f (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash, $relative
    }
[IO.File]::WriteAllLines("$stage\SHA256SUMS.txt", $checksumLines, [Text.UTF8Encoding]::new($false))
Write-Host "checksums: $($checksumLines.Count) files"

Push-Location $stage
& 7z a -tzip -mx=9 -mcu=on $zip . | Out-Null
if ($LASTEXITCODE -ne 0) { throw "7z failed with exit code $LASTEXITCODE" }
Pop-Location

Write-Host ""
Write-Host "staged at $stage"
Get-ChildItem $stage | ForEach-Object { "  {0,-42} {1,10:N0}" -f $_.Name, $_.Length }
Write-Host ""
Write-Host ("zip: {0}  ({1:N1} MB)" -f $zip, ((Get-Item $zip).Length / 1MB))
