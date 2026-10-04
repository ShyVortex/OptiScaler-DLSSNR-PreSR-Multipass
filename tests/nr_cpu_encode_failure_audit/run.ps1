$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$source = Get-Content -Raw -LiteralPath (Join-Path $repo 'OptiScaler\shaders\dlssnr\DlssNr_Dx12_Encode.cpp')
$declaration = Get-Content -LiteralPath (Join-Path $repo 'OptiScaler\shaders\dlssnr\DlssNr_Dx12_State.h') |
    Where-Object { $_ -match '^\s*(void|bool) EncodeInput\(EncodeContext& context\);' }
if (@($declaration).Count -ne 1) { throw 'Expected one production EncodeInput declaration.' }
$signature = [regex]::Match($source, '(void|bool) DlssNr_Dx12::State::EncodeInput\(')
if (!$signature.Success) { throw 'Production EncodeInput was not found.' }
$start = $signature.Index
$body = $source.IndexOf('{', $start)
$depth = 1
$end = $body + 1
while ($depth -gt 0 -and $end -lt $source.Length) {
    if ($source[$end] -eq '{') { ++$depth }
    if ($source[$end] -eq '}') { --$depth }
    ++$end
}
if ($body -lt 0 -or $depth -ne 0) { throw 'Unbalanced production method braces.' }
$build = Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Path $build -Force | Out-Null
[IO.File]::WriteAllText((Join-Path $build 'encode-method.inc'), $source.Substring($start, $end - $start))
[IO.File]::WriteAllText((Join-Path $build 'encode-declaration.inc'), $declaration)
& cl.exe /nologo /std:c++20 /EHsc /W4 "/I$build" "/Fo$build\EncodeFailureTests.obj" "/Fe$build\EncodeFailureTests.exe" (Join-Path $PSScriptRoot 'EncodeFailureTests.cpp')
if ($LASTEXITCODE -ne 0) { throw 'Encode failure CPU compilation failed.' }
& (Join-Path $build 'EncodeFailureTests.exe')
exit $LASTEXITCODE
