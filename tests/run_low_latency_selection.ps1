$ErrorActionPreference = 'Stop'
$build = Join-Path ([System.IO.Path]::GetTempPath()) ('low-latency-selection-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path $build | Out-Null
$source = Join-Path $PSScriptRoot 'low_latency_selection_unit.cpp'
$exe = Join-Path $build 'selection.exe'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer was not found.' }
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $vs) { throw 'Visual Studio C++ build tools were not found.' }
$vsdev = Join-Path $vs 'Common7\Tools\VsDevCmd.bat'
if (-not (Test-Path -LiteralPath $vsdev)) { throw 'Visual Studio developer tools were not found.' }
$obj = Join-Path $build 'selection.obj'
$command = '"' + $vsdev + '" -arch=x64 >nul && cl /nologo /std:c++20 /EHsc /W4 /WX /Fo:"' + $obj + '" /Fe:"' + $exe + '" "' + $source + '"'
& cmd.exe /d /s /c $command
if ($LASTEXITCODE -ne 0) { throw 'Low-latency selection test compilation failed.' }
& $exe
if ($LASTEXITCODE -ne 0) { throw "Low-latency selection test failed ($LASTEXITCODE)." }
Write-Output 'Low-latency selection regression passed (100000 updates per concurrent thread).'
