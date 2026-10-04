param([switch]$Legacy, [string]$OutputDirectory = (Join-Path $env:TEMP 'OptiScaler-menu-viewport'))
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$imgui = Join-Path $repo 'OptiScaler/include/imgui'
$source = Join-Path $PSScriptRoot 'menu_viewport_unit.cpp'
$exe = Join-Path $OutputDirectory ('menu-viewport' + $(if ($Legacy) { '-legacy' }) + '.exe')
$vsdev = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat'
if (-not (Test-Path -LiteralPath $vsdev)) { throw 'Visual Studio developer tools were not found.' }
$define = if ($Legacy) { '/DMENU_VIEWPORT_LEGACY' } else { '' }
$command = '"' + $vsdev + '" -arch=x64 >nul && cl /nologo /std:c++20 /EHsc /W3 /MT ' + $define + ' /I"' + $repo + '\OptiScaler" /I"' + $repo + '\OptiScaler\include" /I"' + $repo + '\external\freetype" /Fe:"' + $exe + '" "' + $source + '" "' + $imgui + '\imgui.cpp" "' + $imgui + '\imgui_draw.cpp" "' + $imgui + '\imgui_tables.cpp" "' + $imgui + '\imgui_widgets.cpp" "' + $imgui + '\misc\freetype\imgui_freetype.cpp" "' + $repo + '\external\freetype\freetype.lib" user32.lib shell32.lib imm32.lib'
Push-Location -LiteralPath $OutputDirectory
try {
    & cmd.exe /d /s /c $command
    if ($LASTEXITCODE -ne 0) { throw 'Menu viewport test compilation failed.' }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "Menu viewport test failed ($LASTEXITCODE)." }
} finally { Pop-Location }
