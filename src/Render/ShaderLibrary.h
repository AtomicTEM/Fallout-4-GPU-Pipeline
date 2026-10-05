#pragma once

namespace GWP
{
	// Creates the plugin's compute shaders. Release builds carry bytecode that
	// fxc compiled at build time, so no shader compiler is needed at runtime
	// (Wine/Proton's built-in d3dcompiler_47 cannot compile these shaders).
	// d3dcompiler_47.dll is loaded only to compile HLSL at runtime: overrides
	// from [Debug] sShaderDirectory, shaders built with defines, or builds
	// without precompiled bytecode.
	class ShaderLibrary
	{
	public:
		bool Initialize(ID3D11Device* a_device);

		[[nodiscard]] Microsoft::WRL::ComPtr<ID3D11ComputeShader> CompileCompute(std::string_view a_file, const char* a_entryPoint, std::initializer_list<std::pair<const char*, const char*>> a_defines = {});

		[[nodiscard]] std::optional<std::string> LoadSource(std::string_view a_file) const;

		// Tests use this to exercise both paths.
		void SetUsePrecompiled(bool a_use) noexcept { _usePrecompiled = a_use; }

		[[nodiscard]] static std::size_t PrecompiledCount() noexcept;
		[[nodiscard]] static std::optional<std::span<const std::uint8_t>> FindPrecompiled(std::string_view a_file, std::string_view a_entryPoint) noexcept;

	private:
		[[nodiscard]] bool PrecompiledAllowed() const;
		bool LoadCompiler();

		ID3D11Device* _device{ nullptr };
		HMODULE _compilerModule{ nullptr };
		pD3DCompile _compile{ nullptr };
		bool _compilerLoadAttempted{ false };
		bool _usePrecompiled{ true };
	};
}
