$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$source = Get-Content -Raw -LiteralPath (Join-Path $repo 'OptiScaler\shaders\dlssnr\DlssNr_Dx12.cpp')
$build = Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Path $build -Force | Out-Null

function Export-Body([string] $signature, [string] $name, [string] $suffix = '') {
    $start = $source.IndexOf($signature, [StringComparison]::Ordinal)
    if ($start -lt 0) { throw "Production declaration not found: $signature" }
    $body = $source.IndexOf('{', $start)
    if ($body -lt 0) { throw "Production body not found: $signature" }
    $depth = 1
    $end = $body + 1
    while ($depth -gt 0 -and $end -lt $source.Length) {
        if ($source[$end] -eq '{') { ++$depth }
        if ($source[$end] -eq '}') { --$depth }
        ++$end
    }
    if ($depth -ne 0) { throw "Unbalanced production body: $signature" }
    [IO.File]::WriteAllText((Join-Path $build $name), $source.Substring($start, $end - $start) + $suffix)
}

# Mechanical extraction: neither the aggregation algorithm nor its scope is copied into the fixture.
Export-Body 'GpuSubmission BeginFinishedPictureSubmission(UINT count,' 'owner-aggregation.inc'
Export-Body 'struct NrNotificationScope' 'notification-scope.inc' ';'
& cl.exe /nologo /std:c++20 /EHsc /W4 "/I$PSScriptRoot" "/I$repo\OptiScaler" "/I$build" "/Fo$build\OwnerAggregationTests.obj" "/Fe$build\OwnerAggregationTests.exe" (Join-Path $PSScriptRoot 'OwnerAggregationTests.cpp')
if ($LASTEXITCODE -ne 0) { throw 'Owner aggregation CPU compilation failed.' }
& (Join-Path $build 'OwnerAggregationTests.exe')
exit $LASTEXITCODE
