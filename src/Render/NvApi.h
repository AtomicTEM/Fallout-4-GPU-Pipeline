#pragma once

namespace GWP
{
	// Minimal dynamic binding of NVAPI's D3D11 multi-draw-indirect extension.
	// nvapi64.dll only exports nvapi_QueryInterface; functions are looked up
	// by the interface IDs published in NVIDIA's nvapi_interface.h.
	class NvApi
	{
	public:
		[[nodiscard]] static NvApi& Get() noexcept
		{
			static NvApi singleton;
			return singleton;
		}

		bool Initialize();

		[[nodiscard]] bool HasMultiDrawIndexedIndirect() const noexcept { return _multiDrawIndexed != nullptr; }

		// Executes a_drawCount DrawIndexedInstancedIndirect calls stored at a
		// a_stride byte intervals starting at a_offset.
		bool MultiDrawIndexedInstancedIndirect(ID3D11DeviceContext* a_context, std::uint32_t a_drawCount, ID3D11Buffer* a_args, std::uint32_t a_offset, std::uint32_t a_stride);

	private:
		using QueryInterface_t = void*(__cdecl*)(std::uint32_t);
		using Initialize_t = int(__cdecl*)();
		using MultiDrawIndexed_t = int(__cdecl*)(ID3D11DeviceContext*, std::uint32_t, ID3D11Buffer*, std::uint32_t, std::uint32_t);

		HMODULE _module{ nullptr };
		MultiDrawIndexed_t _multiDrawIndexed{ nullptr };
		bool _initialized{ false };
	};
}
