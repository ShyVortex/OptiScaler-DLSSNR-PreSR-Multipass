$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$build = Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Path $build -Force | Out-Null

function Extract-Admission($sourcePath, $className, $outputPath) {
    $source = Get-Content -Raw -LiteralPath $sourcePath
    $signature = "bool ${className}::Dispatch("
    $start = $source.IndexOf($signature, [StringComparison]::Ordinal)
    if ($start -lt 0) { throw "Dispatch was not found in $sourcePath" }
    $end = $source.IndexOf('    ScopedGpuTime_Dx12 scopedGpuTime(', $start, [StringComparison]::Ordinal)
    if ($end -lt 0) { throw "Dispatch rendering boundary was not found in $sourcePath" }
    # Mechanical build generation: production signature and admission statements.
    # Replace only the remaining rendering pipeline with a boundary sentinel.
    $prefix = $source.Substring($start, $end - $start)
    [IO.File]::WriteAllText($outputPath, $prefix + "    (void)cmdList;`n    (void)state;`n    return true;`n}`n")
}

Extract-Admission (Join-Path $repo 'OptiScaler\shaders\render_ui\RUI_Dx12.cpp') `
    'RUI_Dx12' (Join-Path $build 'rui-admission.inc')
Extract-Admission (Join-Path $repo 'OptiScaler\shaders\hudless_compare\HC_Dx12.cpp') `
    'HC_Dx12' (Join-Path $build 'hc-admission.inc')

& cl.exe /nologo /std:c++20 /EHsc /W4 /WX "/I$build" `
    "/Fo$build\DispatchAdmissionTests.obj" "/Fe$build\DispatchAdmissionTests.exe" `
    (Join-Path $PSScriptRoot 'DispatchAdmissionTests.cpp')
if ($LASTEXITCODE -ne 0) { throw 'Dispatch admission test compilation failed.' }
& (Join-Path $build 'DispatchAdmissionTests.exe')
exit $LASTEXITCODE
