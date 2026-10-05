#include "Render/NvApi.h"

namespace GWP
{
	namespace
	{
		// From NVIDIA/nvapi nvapi_interface.h
		inline constexpr std::uint32_t kNvAPI_Initialize = 0x0150E828;
		inline constexpr std::uint32_t kNvAPI_D3D11_MultiDrawIndexedInstancedIndirect = 0x59E890F9;

		inline constexpr int kNvApiOk = 0;
	}

	bool NvApi::Initialize()
	{
		if (_initialized) {
			return HasMultiDrawIndexedIndirect();
		}
		_initialized = true;

		_module = ::LoadLibraryW(L"nvapi64.dll");
		if (!_module) {
			logger::info("nvapi: nvapi64.dll not present");
			return false;
		}

		const auto query = reinterpret_cast<QueryInterface_t>(::GetProcAddress(_module, "nvapi_QueryInterface"));
		if (!query) {
			logger::warn("nvapi: nvapi_QueryInterface export missing");
			return false;
		}

		const auto initialize = reinterpret_cast<Initialize_t>(query(kNvAPI_Initialize));
		if (!initialize || initialize() != kNvApiOk) {
			logger::warn("nvapi: NvAPI_Initialize failed");
			return false;
		}

		_multiDrawIndexed = reinterpret_cast<MultiDrawIndexed_t>(query(kNvAPI_D3D11_MultiDrawIndexedInstancedIndirect));
		logger::info("nvapi: MultiDrawIndexedInstancedIndirect {}", _multiDrawIndexed ? "available"sv : "unavailable"sv);
		return HasMultiDrawIndexedIndirect();
	}

	bool NvApi::MultiDrawIndexedInstancedIndirect(ID3D11DeviceContext* a_context, std::uint32_t a_drawCount, ID3D11Buffer* a_args, std::uint32_t a_offset, std::uint32_t a_stride)
	{
		return _multiDrawIndexed && _multiDrawIndexed(a_context, a_drawCount, a_args, a_offset, a_stride) == kNvApiOk;
	}
}
