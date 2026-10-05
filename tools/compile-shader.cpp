// Minimal stand-in for fxc, used by tools/run-tests-wine.sh to precompile the
// compute shaders with Microsoft's d3dcompiler_47 under Wine, the same way
// the MSVC build does with fxc (cs_5_0, /O3, /Ges).
//
// Usage: compile-shader.exe <source.hlsl> <entry point> <output.cso>

#include <d3dcompiler.h>
#include <windows.h>

#include <cstdio>

int main(int a_argc, char** a_argv)
{
	if (a_argc != 4) {
		std::fprintf(stderr, "usage: compile-shader <source.hlsl> <entry point> <output.cso>\n");
		return 2;
	}

	const auto compiler = ::LoadLibraryW(L"d3dcompiler_47.dll");
	const auto compileFromFile = compiler ? reinterpret_cast<decltype(&D3DCompileFromFile)>(::GetProcAddress(compiler, "D3DCompileFromFile")) : nullptr;
	if (!compileFromFile) {
		std::fprintf(stderr, "d3dcompiler_47.dll with D3DCompileFromFile is required\n");
		return 1;
	}

	wchar_t source[MAX_PATH]{};
	::MultiByteToWideChar(CP_UTF8, 0, a_argv[1], -1, source, MAX_PATH);

	ID3DBlob* bytecode = nullptr;
	ID3DBlob* errors = nullptr;
	const auto result = compileFromFile(source, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, a_argv[2], "cs_5_0",
		D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS, 0, &bytecode, &errors);
	if (errors) {
		std::fprintf(stderr, "%.*s", static_cast<int>(errors->GetBufferSize()), static_cast<const char*>(errors->GetBufferPointer()));
	}
	if (FAILED(result) || !bytecode) {
		std::fprintf(stderr, "%s:%s failed to compile (0x%08lX)\n", a_argv[1], a_argv[2], static_cast<unsigned long>(result));
		return 1;
	}

	const auto file = std::fopen(a_argv[3], "wb");
	if (!file || std::fwrite(bytecode->GetBufferPointer(), 1, bytecode->GetBufferSize(), file) != bytecode->GetBufferSize()) {
		std::fprintf(stderr, "cannot write %s\n", a_argv[3]);
		return 1;
	}
	std::fclose(file);
	return 0;
}
