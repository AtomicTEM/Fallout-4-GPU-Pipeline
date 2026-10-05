#!/usr/bin/env bash
# Checks a packaged build of the plugin: the folder layout a mod manager or a
# manual install into Fallout 4/Data expects, and the DLL itself (64-bit PE,
# the exports F4SE looks for, imports limited to Windows and the Visual C++
# runtime, and the precompiled shaders that let it run without a shader
# compiler, which Proton lacks).
#
# Usage: tools/check-package.sh <package dir>
#   <package dir> contains F4SE/Plugins/..., e.g. an extracted CI artifact.
#
# Needs GNU binutils' objdump with PE support (Ubuntu/Debian/Arch: binutils;
# macOS: brew install binutils and put its objdump first in PATH).
set -euo pipefail

PACKAGE=${1:?usage: $0 <package dir>}
SOURCELIST="$(cd "$(dirname "$0")/.." && pwd)/cmake/sourcelist.cmake"
PLUGIN_DIR=F4SE/Plugins
DLL=$PLUGIN_DIR/GPUWorldPipeline.dll
INI=$PLUGIN_DIR/GPUWorldPipeline.ini
FAILED=0

fail() {
	echo "error: $*" >&2
	FAILED=1
}

if ! objdump --version 2>/dev/null | grep -q '^GNU objdump' || ! objdump -i 2>/dev/null | grep -qx 'pei-x86-64'; then
	echo "error: needs GNU binutils' objdump with PE (pei-x86-64) support" >&2
	exit 2
fi

cd "$PACKAGE"

# Exactly the DLL and INI, nothing else (no PDB, .trace or .mapping files).
expected=$(printf '%s\n' "$DLL" "$INI" | LC_ALL=C sort)
actual=$(find . -type f | sed 's|^\./||' | LC_ALL=C sort)
if [[ "$actual" != "$expected" ]]; then
	fail "unexpected package contents:"$'\n'"$actual"$'\n'"expected:"$'\n'"$expected"
fi
[[ -f "$DLL" ]] || { echo "error: $DLL is missing" >&2; exit 1; }

header=$(objdump -p "$DLL")

format=$(objdump -f "$DLL" | awk '/file format/ { print $NF }')
[[ "$format" == pei-x86-64 ]] || fail "$DLL is '$format', expected a 64-bit PE (pei-x86-64)"

characteristics=$(awk '$1 == "Characteristics" { print $2; exit }' <<<"$header")
if [[ -z "$characteristics" ]] || (((characteristics & 0x2000) == 0)); then
	fail "$DLL is not a DLL (characteristics ${characteristics:-unknown})"
fi

# F4SE (NG/AE) reads F4SEPlugin_Version, F4SE for 1.10.163 calls
# F4SEPlugin_Query (supplied by CommonLibF4RD), and both call F4SEPlugin_Load.
exports=$(awk '/^\[Ordinal\/Name Pointer\] Table/ { table = 1; next } table && NF == 0 { exit } table { print $NF }' <<<"$header")
for symbol in F4SEPlugin_Version F4SEPlugin_Query F4SEPlugin_Load; do
	grep -qx "$symbol" <<<"$exports" || fail "$DLL does not export $symbol"
done

# Dependencies are linked statically (x64-windows-static-md), so the DLL may
# import only Windows system DLLs and the release Visual C++ runtime. Anything
# else (a vcpkg dependency's DLL, the debug CRT, d3dcompiler_47) would keep
# the plugin from loading on some systems.
imports=$(awk '/DLL Name:/ { print $NF }' <<<"$header")
while read -r import; do
	if ! grep -Eiq '^(kernel32|user32|gdi32|shell32|ole32|oleaut32|advapi32|version|d3d11|dxgi|dbghelp|bcrypt|ntdll|shlwapi|ws2_32|winmm|psapi)\.dll$|^(msvcp140(_1|_2|_atomic_wait|_codecvt_ids)?|vcruntime140(_1)?|concrt140)\.dll$|^api-ms-win-[a-z0-9-]+\.dll$' <<<"$import"; then
		fail "$DLL imports $import, which is not a Windows system DLL or the release Visual C++ runtime"
	fi
done <<<"$imports"

# Every compute shader is embedded as fxc bytecode (each blob starts with
# "DXBC"); without them the plugin needs d3dcompiler_47 at runtime.
expectedShaders=1
if [[ -f "$SOURCELIST" ]]; then
	expectedShaders=$(awk '/^set\(COMPUTE_SHADERS/ { list = 1; next } list && /^\)/ { exit } list && NF { count++ } END { print count + 0 }' "$SOURCELIST")
fi
shaderBlobs=$(grep -aob 'DXBC' "$DLL" | wc -l)
if ((shaderBlobs < expectedShaders)); then
	fail "$DLL embeds $shaderBlobs precompiled shaders, expected $expectedShaders"
fi

grep -q '^\[General\]' "$INI" 2>/dev/null || fail "$INI has no [General] section"

echo "Package: $PACKAGE"
for file in "$DLL" "$INI"; do
	if [[ -f "$file" ]]; then
		printf '  %-40s %8d bytes  sha256 %s\n' "$file" "$(wc -c <"$file")" "$(sha256sum "$file" | cut -d' ' -f1)"
	fi
done
echo "Exports: $(paste -sd' ' <<<"$exports")"
echo "Imports: $(paste -sd' ' <<<"$imports")"
echo "Precompiled shaders: $shaderBlobs"

if ((FAILED)); then
	exit 1
fi
echo "OK"
