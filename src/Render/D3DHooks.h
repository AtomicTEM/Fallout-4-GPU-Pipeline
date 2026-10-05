#pragma once

// Direct3D 11 / DXGI interception.
//
// All hooks are COM vtable patches (no executable addresses), plus one import
// address table patch on Fallout4.exe so the device vtable is patched before
// the renderer creates its input layouts.

namespace GWP::D3DHooks
{
	// Interface method slots (d3d11.h / dxgi.h declaration order).
	namespace Slot
	{
		inline constexpr std::size_t kDeviceCreateInputLayout = 11;

		inline constexpr std::size_t kContextDrawIndexed = 12;
		inline constexpr std::size_t kContextDrawIndexedInstanced = 20;

		inline constexpr std::size_t kSwapChainPresent = 8;
		inline constexpr std::size_t kSwapChain1Present1 = 22;
	}

	using CreateInputLayout_t = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_INPUT_ELEMENT_DESC*, UINT, const void*, SIZE_T, ID3D11InputLayout**);
	using DrawIndexed_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
	using DrawIndexedInstanced_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
	using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
	using Present1_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

	// Patches the D3D11CreateDevice* imports of the game executable. Must run
	// during plugin load, before the renderer initializes.
	bool InstallCreateDeviceHooks();

	// Patches the device/context/swap chain vtables. Safe to call repeatedly;
	// each vtable is only patched once.
	void InstallObjectHooks(ID3D11Device* a_device, ID3D11DeviceContext* a_context, IDXGISwapChain* a_swapChain);

	[[nodiscard]] bool DeviceHooked() noexcept;
	[[nodiscard]] bool HookedBeforeRendererInit() noexcept;

	// Originals, used by the pipeline to issue draws that bypass its own hooks.
	void CallDrawIndexed(ID3D11DeviceContext* a_context, UINT a_indexCount, UINT a_startIndex, INT a_baseVertex);
	void CallDrawIndexedInstanced(ID3D11DeviceContext* a_context, UINT a_indexCount, UINT a_instanceCount, UINT a_startIndex, INT a_baseVertex, UINT a_startInstance);
	HRESULT CallCreateInputLayout(ID3D11Device* a_device, const D3D11_INPUT_ELEMENT_DESC* a_descs, UINT a_count, const void* a_bytecode, SIZE_T a_length, ID3D11InputLayout** a_layout);
}
