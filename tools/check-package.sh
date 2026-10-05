#!/usr/bin/env bash
# Checks a packaged build of the plugin: the folder layout a mod manager or a
# manual install into Fallout 4/Data expects, and the DLL itself (64-bit PE,
# the exports F4SE looks for, no imports the game cannot satisfy).
#
# Usage: tools/check-package.sh <package dir>
#   <package dir> contains F4SE/Plugins/..., e.g. an extracted CI artifact.
#
# Needs GNU binutils' objdump (Ubuntu/Debian: binutils), which reads PE files.
set -euo pipefail

PACKAGE=${1:?usage: $0 <package dir>}
PLUGIN_DIR=F4SE/Plugins
DLL=$PLUGIN_DIR/GPUWorldPipeline.dll
INI=$PLUGIN_DIR/GPUWorldPipeline.ini
FAILED=0

fail() {
	echo "error: $*" >&2
	FAILED=1
}

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

# Dependencies are linked statically (x64-windows-static-md) and the game
# ships only the release CRT, so third-party or debug-CRT imports would keep
# the plugin from loading.
imports=$(awk '/DLL Name:/ { print $NF }' <<<"$header")
while read -r import; do
	if grep -Eiq '^(fmt|spdlog|zydis|boost|mmio)|^(ucrtbased|vcruntime140d|vcruntime140_1d|msvcp140d)\.dll$|^d3dcompiler' <<<"$import"; then
		fail "$DLL imports $import"
	fi
done <<<"$imports"

grep -q '^\[General\]' "$INI" 2>/dev/null || fail "$INI has no [General] section"

echo "Package: $PACKAGE"
for file in "$DLL" "$INI"; do
	if [[ -f "$file" ]]; then
		printf '  %-40s %8d bytes  sha256 %s\n' "$file" "$(stat -c %s "$file")" "$(sha256sum "$file" | cut -d' ' -f1)"
	fi
done
echo "Exports: $(paste -sd' ' <<<"$exports")"
echo "Imports: $(paste -sd' ' <<<"$imports")"

if ((FAILED)); then
	exit 1
fi
echo "OK"
