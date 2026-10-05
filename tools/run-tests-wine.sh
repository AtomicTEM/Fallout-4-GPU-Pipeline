#!/usr/bin/env bash
# Builds tests/GpuTests.cpp with MinGW-w64 and runs it under Wine, where
# Direct3D 11 is provided by wined3d on top of Mesa's llvmpipe (software GL).
# Lets the GPU pipeline be tested on Linux CI without Fallout 4 or a GPU.
#
# Ubuntu/Debian packages:
#   g++-mingw-w64-x86-64-posix wine64 xvfb xauth libgl1-mesa-dri libfmt-dev cmake curl unzip
#
# Wine's built-in HLSL compiler cannot compile these shaders, so Microsoft's
# d3dcompiler_47 is used instead. Set D3DCOMPILER=/path/to/d3dcompiler_47.dll,
# or let the script take D3DCompiler_47_cor3.dll from the .NET WindowsDesktop
# runtime pack on nuget.org (the copy WPF ships with).
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-"$ROOT/build/wine-tests"}
CXX=${CXX:-x86_64-w64-mingw32-g++-posix}
WINE=${WINE:-$(command -v wine64 || echo /usr/lib/wine/wine64)}
RUNTIME_PACK_VERSION=${RUNTIME_PACK_VERSION:-8.0.11}

mkdir -p "$OUT/include" "$OUT/deps"

# Embedded shader header, exactly as the CMake build produces it.
SHADERS=$(cd "$ROOT/shaders" && ls *.hlsl *.hlsli | paste -sd';')
cmake -DOUTPUT="$OUT/include/EmbeddedShaders.h" -DSHADER_DIR="$ROOT/shaders" -DSHADERS="$SHADERS" \
	-P "$ROOT/cmake/GenerateShaderHeader.cmake"

# Header-only dependencies without the rest of /usr/include, plus a
# Windows.h alias for MinGW's lower-case windows.h.
ln -sfn "${FMT_INCLUDE:-/usr/include/fmt}" "$OUT/deps/fmt"
WINDOWS_H=$(echo '#include <windows.h>' | "$CXX" -x c++ -E -H - 2>&1 >/dev/null | awk '/\/windows\.h$/ && !found { sub(/^\.+ /, ""); print; found = 1 }')
ln -sfn "$WINDOWS_H" "$OUT/deps/Windows.h"

"$CXX" -std=c++20 -O1 -static -DFMT_HEADER_ONLY \
	-I "$OUT/include" -I "$ROOT/src" -I "$ROOT/tests" -isystem "$OUT/deps" \
	-include "$ROOT/tests/TestPCH.h" \
	"$ROOT/tests/GpuTests.cpp" \
	"$ROOT/src/Render/GpuBuffers.cpp" \
	"$ROOT/src/Render/HiZ.cpp" \
	"$ROOT/src/Render/ShaderLibrary.cpp" \
	"$ROOT/src/Scene/MergeMath.cpp" \
	"$ROOT/src/Scene/VertexFormat.cpp" \
	-o "$OUT/GpuTests.exe" -ld3d11 -ldxgi -ldxguid -luuid

if [[ -z "${D3DCOMPILER:-}" ]]; then
	D3DCOMPILER="$OUT/deps/D3DCompiler_47_cor3.dll"
	if [[ ! -f "$D3DCOMPILER" ]]; then
		echo "Fetching d3dcompiler_47 from Microsoft.WindowsDesktop.App.Runtime.win-x64 $RUNTIME_PACK_VERSION"
		PACKAGE="$OUT/deps/windowsdesktop.nupkg"
		curl -sSL -o "$PACKAGE" "https://api.nuget.org/v3-flatcontainer/microsoft.windowsdesktop.app.runtime.win-x64/$RUNTIME_PACK_VERSION/microsoft.windowsdesktop.app.runtime.win-x64.$RUNTIME_PACK_VERSION.nupkg"
		unzip -o -j -q "$PACKAGE" "runtimes/win-x64/native/D3DCompiler_47_cor3.dll" -d "$OUT/deps"
	fi
fi
cp "$D3DCOMPILER" "$OUT/d3dcompiler_47.dll"

export WINEPREFIX=${WINEPREFIX:-"$OUT/wineprefix"}
export WINEDEBUG=${WINEDEBUG:--all}
export WINEDLLOVERRIDES="d3dcompiler_47=n"

if [[ -z "${DISPLAY:-}" ]]; then
	export DISPLAY=:97
	Xvfb "$DISPLAY" -screen 0 1024x768x24 >/dev/null 2>&1 &
	XVFB_PID=$!
	trap 'kill $XVFB_PID 2>/dev/null || true' EXIT
	sleep 2
fi

cd "$OUT"
"$WINE" GpuTests.exe "$@"
