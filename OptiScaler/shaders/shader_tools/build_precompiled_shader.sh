#!/usr/bin/env bash
# Linux counterpart of the DXC (DX12) step of build_precompiled_shader.bat.
#
# Runs the bundled dxc.exe / dxil.dll under Wine, validates with the bundled dxv.exe and writes the header with
# create_header.py. The output is byte-identical to the Windows tools (verified by rebuilding the committed NR
# spatial shaders). FXC / DX11 blobs are NOT built here: under Wine fxc.exe uses Wine's own D3DCOMPILER_47.dll.
#
# Usage: build_precompiled_shader.sh [-o OUT_BASE] [-s SYMBOL] SHADER_BASE [-- EXTRA_DXC_ARGS...]
#   SHADER_BASE  path without extension; compiles SHADER_BASE.hlsl
#   -o OUT_BASE  output base (default SHADER_BASE); writes OUT_BASE_Shader.cso and OUT_BASE_Shader.h
#   -s SYMBOL    array name in the header (default: basename of OUT_BASE + "_cso")
# Environment: SHADER_WINEPREFIX reuses a Wine prefix; otherwise a throwaway one is created and removed.

set -euo pipefail

usage()
{
    sed -n '8,13p' "$0" >&2
    exit 1
}

outBase=""
symbol=""
while getopts "o:s:" opt; do
    case "$opt" in
    o) outBase="$OPTARG" ;;
    s) symbol="$OPTARG" ;;
    *) usage ;;
    esac
done
shift $((OPTIND - 1))
[ $# -ge 1 ] || usage
shaderBase="$1"
shift
[ "${1:-}" = "--" ] && shift
extraArgs=("$@")

toolsDir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source="$(realpath "$shaderBase.hlsl")"
[ -f "$source" ] || { echo "error: $shaderBase.hlsl not found" >&2; exit 1; }
outBase="$(realpath -m "${outBase:-$shaderBase}")"
symbol="${symbol:-$(basename "$outBase")_cso}"
cso="${outBase}_Shader.cso"
header="${outBase}_Shader.h"

command -v wine >/dev/null || { echo "error: wine is required" >&2; exit 1; }
command -v python3 >/dev/null || { echo "error: python3 is required" >&2; exit 1; }

if [ -n "${SHADER_WINEPREFIX:-}" ]; then
    export WINEPREFIX="$SHADER_WINEPREFIX"
else
    WINEPREFIX="$(mktemp -d "${TMPDIR:-/tmp}/optiscaler-shader-wine.XXXXXX")"
    export WINEPREFIX
    trap 'wineserver -k >/dev/null 2>&1 || true; rm -rf "$WINEPREFIX"' EXIT
fi
export WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml="

rm -f "$cso"
echo "Creating Dx12 CSO: $cso"
(cd "$(dirname "$source")" &&
    wine "$toolsDir/dxc.exe" -T cs_6_0 -E CSMain -O3 -Qstrip_debug -Qstrip_reflect "${extraArgs[@]}" \
        "$(basename "$source")" -Fo "Z:$cso" 2>&1 | grep -v -i -E "mesa|pci id|\.desktop" || true)
[ -s "$cso" ] || { echo "error: dxc produced no output for $source" >&2; exit 1; }

echo "Validating with dxv"
validation="$(wine "$toolsDir/dxv.exe" "Z:$cso" 2>&1 || true)"
if ! grep -q "Validation succeeded" <<<"$validation"; then
    echo "error: dxv validation failed for $cso" >&2
    grep -v -i -E "mesa|pci id|\.desktop" <<<"$validation" >&2 || true
    exit 1
fi

echo "Creating Dx12 Header: $header"
python3 -I "$toolsDir/create_header.py" "$cso" "$header" "$symbol"
