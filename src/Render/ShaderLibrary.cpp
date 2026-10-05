#include "Render/ShaderLibrary.h"

#include "EmbeddedShaders.h"
#include "Settings.h"

#ifdef GWP_PRECOMPILED_SHADERS
#	include "PrecompiledShaders.h"
#endif

namespace GWP
{
	namespace
	{
		class IncludeHandler final : public ID3DInclude
		{
		public:
			explicit IncludeHandler(const ShaderLibrary& a_library) :
				_library(a_library)
			{}

			HRESULT STDMETHODCALLTYPE Open(D3D_INCLUDE_TYPE, LPCSTR a_fileName, LPCVOID, LPCVOID* a_data, UINT* a_bytes) override
			{
				auto source = _library.LoadSource(a_fileName ? a_fileName : "");
				if (!source) {
					return E_FAIL;
				}
				auto& stored = _storage.emplace_back(std::move(*source));
				*a_data = stored.data();
				*a_bytes = static_cast<UINT>(stored.size());
				return S_OK;
			}

			HRESULT STDMETHODCALLTYPE Close(LPCVOID) override
			{
				return S_OK;
			}

		private:
			const ShaderLibrary& _library;
			std::deque<std::string> _storage;
		};
	}

	bool ShaderLibrary::Initialize(ID3D11Device* a_device)
	{
		_device = a_device;
		if (PrecompiledAllowed() && PrecompiledCount() > 0) {
			logger::info("shaders: using {} precompiled compute shaders", PrecompiledCount());
			return true;
		}
		return LoadCompiler();
	}

	std::size_t ShaderLibrary::PrecompiledCount() noexcept
	{
#ifdef GWP_PRECOMPILED_SHADERS
		return std::size(PrecompiledShaders::kAll);
#else
		return 0;
#endif
	}

	std::optional<std::span<const std::uint8_t>> ShaderLibrary::FindPrecompiled([[maybe_unused]] std::string_view a_file, [[maybe_unused]] std::string_view a_entryPoint) noexcept
	{
#ifdef GWP_PRECOMPILED_SHADERS
		for (const auto& entry : PrecompiledShaders::kAll) {
			if (entry.file == a_file && entry.entryPoint == a_entryPoint) {
				return entry.bytecode;
			}
		}
#endif
		return std::nullopt;
	}

	bool ShaderLibrary::PrecompiledAllowed() const
	{
		// Shader overrides on disk are compiled at runtime so they can be
		// edited without rebuilding the DLL.
		return _usePrecompiled && Settings::Get().shaderDirectory.empty();
	}

	bool ShaderLibrary::LoadCompiler()
	{
		if (_compile) {
			return true;
		}
		if (_compilerLoadAttempted) {
			return false;
		}
		_compilerLoadAttempted = true;

		_compilerModule = ::LoadLibraryW(L"d3dcompiler_47.dll");
		if (!_compilerModule) {
			logger::error("shaders: d3dcompiler_47.dll could not be loaded (error {})", ::GetLastError());
			return false;
		}

		_compile = reinterpret_cast<pD3DCompile>(::GetProcAddress(_compilerModule, "D3DCompile"));
		if (!_compile) {
			logger::error("shaders: D3DCompile export missing");
			return false;
		}
		return true;
	}

	std::optional<std::string> ShaderLibrary::LoadSource(std::string_view a_file) const
	{
		const auto& overrideDirectory = Settings::Get().shaderDirectory;
		if (!overrideDirectory.empty()) {
			const auto path = std::filesystem::path{ overrideDirectory } / std::filesystem::path{ std::string{ a_file } };
			std::ifstream stream{ path, std::ios::binary };
			if (stream) {
				return std::string{ std::istreambuf_iterator<char>{ stream }, std::istreambuf_iterator<char>{} };
			}
		}

		for (const auto& entry : EmbeddedShaders::kAll) {
			if (entry.name == a_file) {
				return std::string{ entry.source };
			}
		}
		return std::nullopt;
	}

	Microsoft::WRL::ComPtr<ID3D11ComputeShader> ShaderLibrary::CompileCompute(std::string_view a_file, const char* a_entryPoint, std::initializer_list<std::pair<const char*, const char*>> a_defines)
	{
		if (!_device) {
			return nullptr;
		}

		if (PrecompiledAllowed() && a_defines.size() == 0) {
			if (const auto bytecode = FindPrecompiled(a_file, a_entryPoint)) {
				Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader;
				const auto result = _device->CreateComputeShader(bytecode->data(), bytecode->size(), nullptr, shader.GetAddressOf());
				if (FAILED(result)) {
					logger::error("shaders: CreateComputeShader failed for precompiled {}:{} (0x{:08X})", a_file, a_entryPoint, static_cast<std::uint32_t>(result));
					return nullptr;
				}
				return shader;
			}
		}

		if (!LoadCompiler()) {
			logger::error("shaders: {}:{} needs d3dcompiler_47.dll to compile at runtime", a_file, a_entryPoint);
			return nullptr;
		}

		const auto source = LoadSource(a_file);
		if (!source) {
			logger::error("shaders: source {} not found", a_file);
			return nullptr;
		}

		std::vector<D3D_SHADER_MACRO> macros;
		macros.reserve(a_defines.size() + 1);
		for (const auto& [name, value] : a_defines) {
			macros.push_back({ name, value });
		}
		macros.push_back({ nullptr, nullptr });

		IncludeHandler includes{ *this };
		Microsoft::WRL::ComPtr<ID3DBlob> bytecode;
		Microsoft::WRL::ComPtr<ID3DBlob> errors;
		const std::string fileName{ a_file };

		const auto result = _compile(
			source->data(), source->size(), fileName.c_str(), macros.data(), &includes, a_entryPoint, "cs_5_0",
			D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS, 0, bytecode.GetAddressOf(), errors.GetAddressOf());

		if (errors && errors->GetBufferSize() > 0) {
			const std::string_view text{ static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize() };
			if (FAILED(result)) {
				logger::error("shaders: {}:{} failed to compile:\n{}", a_file, a_entryPoint, text);
			} else {
				logger::debug("shaders: {}:{} warnings:\n{}", a_file, a_entryPoint, text);
			}
		}
		if (FAILED(result) || !bytecode) {
			return nullptr;
		}

		Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader;
		if (FAILED(_device->CreateComputeShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr, shader.GetAddressOf()))) {
			logger::error("shaders: CreateComputeShader failed for {}:{}", a_file, a_entryPoint);
			return nullptr;
		}
		return shader;
	}
}
