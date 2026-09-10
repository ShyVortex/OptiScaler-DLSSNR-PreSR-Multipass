# Reuse the verified kernel bundle from the published release. No CUDA installation is needed.
param(
    [Parameter(Mandatory = $true)]
    [string]$Destination,
    [string]$Archive
)
$ErrorActionPreference = 'Stop'
$expectedHash = '3e735f825b85872d20c35c0d75404c1a28b7b6e11ca28ef06243a18225191037'
if (Test-Path -LiteralPath $Destination) { throw 'Hybrid asset destination already exists; choose a new directory.' }
if (-not $Archive) {
    $Archive = Join-Path ([IO.Path]::GetTempPath()) ([IO.Path]::GetRandomFileName() + '.zip')
    Invoke-WebRequest -UseBasicParsing -Uri 'https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass/releases/download/v0.7.5-nr-fixes/OptiScaler-DLSSNR-v0.7.5-nr-fixes.zip' -OutFile $Archive
}
if ((Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash -ne $expectedHash) {
    throw 'Hybrid source archive checksum mismatch.'
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::OpenRead($Archive)
try {
    $prefix = 'OptiScaler/nvfp4/hybrid/'
    $entries = @($zip.Entries | Where-Object { $_.FullName.StartsWith($prefix, [StringComparison]::Ordinal) -and $_.Name })
    if ($entries.Count -ne 15) { throw 'Incomplete hybrid source archive.' }
    $assetRoot = [IO.Path]::GetFullPath((Join-Path $Destination $prefix))
    $files = foreach ($entry in $entries) {
        $relative = $entry.FullName.Substring($prefix.Length)
        $target = [IO.Path]::GetFullPath((Join-Path $assetRoot $relative))
        if (-not $target.StartsWith($assetRoot.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Invalid hybrid asset path.'
        }
        New-Item -ItemType Directory -Force -Path ([IO.Path]::GetDirectoryName($target)) | Out-Null
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $target, $false)
        [PSCustomObject]@{ path = $relative; sha256 = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant() }
    }
    @{ files = @($files) } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $Destination 'asset-manifest.json') -Encoding UTF8
    Write-Host "Verified and extracted $($files.Count) hybrid assets."
}
finally { $zip.Dispose() }
