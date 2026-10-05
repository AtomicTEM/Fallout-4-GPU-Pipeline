#pragma once

namespace GWP
{
	// Compiles the embedded HLSL (or overrides from [Debug] sShaderDirectory)
	// with d3dcompiler_47.dll, which ships with Windows 10 and 11.
	class ShaderLibrary
	{
	public:
		bool Initialize(ID3D11Device* a_device);

		[[nodiscard]] Microsoft::WRL::ComPtr<ID3D11ComputeShader> CompileCompute(std::string_view a_file, const char* a_entryPoint, std::initializer_list<std::pair<const char*, const char*>> a_defines = {});

		[[nodiscard]] std::optional<std::string> LoadSource(std::string_view a_file) const;

	private:
		ID3D11Device* _device{ nullptr };
		HMODULE _compilerModule{ nullptr };
		pD3DCompile _compile{ nullptr };
	};
}
