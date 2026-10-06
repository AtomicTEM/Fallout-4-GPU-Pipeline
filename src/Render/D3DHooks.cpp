#include "Render/D3DHooks.h"

#include "Core/Guard.h"
#include "Core/Pipeline.h"
#include "Render/InputLayouts.h"

namespace GWP::D3DHooks
{
	namespace
	{
		using D3D11CreateDeviceAndSwapChain_t = HRESULT(WINAPI*)(
			IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
			const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

		using D3D11CreateDevice_t = HRESULT(WINAPI*)(
			IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
			ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

		using CreateDeferredContext_t = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, UINT, ID3D11DeviceContext**);

		inline constexpr std::size_t kDeviceCreateDeferredContext = 27;

		D3D11CreateDeviceAndSwapChain_t g_createDeviceAndSwapChain{ nullptr };
		D3D11CreateDevice_t g_createDevice{ nullptr };

		CreateInputLayout_t g_createInputLayout{ nullptr };
		CreateDeferredContext_t g_createDeferredContext{ nullptr };
		DrawIndexed_t g_drawIndexed{ nullptr };
		DrawIndexedInstanced_t g_drawIndexedInstanced{ nullptr };
		Present_t g_present{ nullptr };
		Present1_t g_present1{ nullptr };
		thread_local bool t_inPresent{ false };

		std::mutex g_patchLock;
		std::unordered_set<std::uintptr_t*> g_patchedVTables;
		std::atomic_bool g_deviceHooked{ false };
		std::atomic_bool g_hookedEarly{ false };

		// Replaces one vtable slot and returns the previous entry. The first
		// caller for a slot wins; later calls (other objects sharing the same
		// vtable) see our function already installed and keep the original.
		template <class T>
		void PatchSlot(void* a_object, std::size_t a_slot, T a_replacement, T& a_original)
		{
			auto* const vtable = *reinterpret_cast<std::uintptr_t**>(a_object);
			auto* const entry = vtable + a_slot;
			const auto replacement = reinterpret_cast<std::uintptr_t>(a_replacement);

			if (*entry == replacement) {
				return;
			}

			DWORD oldProtect{};
			if (!::VirtualProtect(entry, sizeof(std::uintptr_t), PAGE_EXECUTE_READWRITE, &oldProtect)) {
				logger::error("d3d: VirtualProtect failed for vtable slot {}", a_slot);
				return;
			}

			const auto previous = *entry;
			*entry = replacement;
			::VirtualProtect(entry, sizeof(std::uintptr_t), oldProtect, &oldProtect);

			if (!a_original) {
				a_original = reinterpret_cast<T>(previous);
			} else if (reinterpret_cast<std::uintptr_t>(a_original) != previous) {
				// A second, different vtable (e.g. a deferred context class). The
				// pipeline only supports one original per slot; restore it.
				::VirtualProtect(entry, sizeof(std::uintptr_t), PAGE_EXECUTE_READWRITE, &oldProtect);
				*entry = previous;
				::VirtualProtect(entry, sizeof(std::uintptr_t), oldProtect, &oldProtect);
				logger::warn("d3d: object with a distinct vtable for slot {} left unhooked", a_slot);
				Pipeline::Get().ReportUnhookedContextClass();
			}
		}

		[[nodiscard]] bool MarkVTable(void* a_object)
		{
			std::scoped_lock lock{ g_patchLock };
			return g_patchedVTables.insert(*reinterpret_cast<std::uintptr_t**>(a_object)).second;
		}

		HRESULT STDMETHODCALLTYPE HookCreateInputLayout(ID3D11Device* a_this, const D3D11_INPUT_ELEMENT_DESC* a_descs, UINT a_count, const void* a_bytecode, SIZE_T a_length, ID3D11InputLayout** a_layout)
		{
			const auto result = g_createInputLayout(a_this, a_descs, a_count, a_bytecode, a_length, a_layout);
			if (SUCCEEDED(result) && a_layout && *a_layout) {
				Guarded("CreateInputLayout", [&] { InputLayouts::Get().OnCreated(*a_layout, a_descs, a_count, a_bytecode, a_length); });
			}
			return result;
		}

		void HookContext(ID3D11DeviceContext* a_context);

		HRESULT STDMETHODCALLTYPE HookCreateDeferredContext(ID3D11Device* a_this, UINT a_flags, ID3D11DeviceContext** a_context)
		{
			const auto result = g_createDeferredContext(a_this, a_flags, a_context);
			if (SUCCEEDED(result) && a_context && *a_context) {
				Guarded("CreateDeferredContext", [&] { HookContext(*a_context); });
			}
			return result;
		}

		void STDMETHODCALLTYPE HookDrawIndexed(ID3D11DeviceContext* a_this, UINT a_indexCount, UINT a_startIndex, INT a_baseVertex)
		{
			if (Guarded("DrawIndexed", [] { return false; }, [&] { return Pipeline::Get().OnDrawIndexed(a_this, a_indexCount, 1, a_startIndex, a_baseVertex, 0, false); })) {
				return;
			}
			g_drawIndexed(a_this, a_indexCount, a_startIndex, a_baseVertex);
		}

		void STDMETHODCALLTYPE HookDrawIndexedInstanced(ID3D11DeviceContext* a_this, UINT a_indexCount, UINT a_instanceCount, UINT a_startIndex, INT a_baseVertex, UINT a_startInstance)
		{
			if (Guarded("DrawIndexedInstanced", [] { return false; }, [&] { return Pipeline::Get().OnDrawIndexed(a_this, a_indexCount, a_instanceCount, a_startIndex, a_baseVertex, a_startInstance, true); })) {
				return;
			}
			g_drawIndexedInstanced(a_this, a_indexCount, a_instanceCount, a_startIndex, a_baseVertex, a_startInstance);
		}

		HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* a_this, UINT a_syncInterval, UINT a_flags)
		{
			// DXGI_PRESENT_TEST only queries occlusion state; it is not a frame.
			if ((a_flags & DXGI_PRESENT_TEST) == 0 && !t_inPresent) {
				// After a fault only the command buffers detached that frame are put back.
				Guarded("Present", [] { Pipeline::Get().ReattachAfterFault(); }, [&] { Pipeline::Get().OnPresent(a_this); });
			}
			const bool outer = !t_inPresent;
			t_inPresent = true;
			const auto result = g_present(a_this, a_syncInterval, a_flags);
			t_inPresent = !outer;
			return result;
		}

		HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* a_this, UINT a_syncInterval, UINT a_flags, const DXGI_PRESENT_PARAMETERS* a_parameters)
		{
			// Some runtimes implement Present on top of Present1; count the frame once.
			if ((a_flags & DXGI_PRESENT_TEST) == 0 && !t_inPresent) {
				Guarded("Present1", [] { Pipeline::Get().ReattachAfterFault(); }, [&] { Pipeline::Get().OnPresent(a_this); });
			}
			const bool outer = !t_inPresent;
			t_inPresent = true;
			const auto result = g_present1(a_this, a_syncInterval, a_flags, a_parameters);
			t_inPresent = !outer;
			return result;
		}

		void HookDevice(ID3D11Device* a_device)
		{
			if (!a_device || !MarkVTable(a_device)) {
				return;
			}
			PatchSlot(a_device, Slot::kDeviceCreateInputLayout, &HookCreateInputLayout, g_createInputLayout);
			PatchSlot(a_device, kDeviceCreateDeferredContext, &HookCreateDeferredContext, g_createDeferredContext);
			g_deviceHooked = true;
			logger::info("d3d: device vtable hooked");
		}

		void HookContext(ID3D11DeviceContext* a_context)
		{
			if (!a_context || !MarkVTable(a_context)) {
				return;
			}
			PatchSlot(a_context, Slot::kContextDrawIndexed, &HookDrawIndexed, g_drawIndexed);
			PatchSlot(a_context, Slot::kContextDrawIndexedInstanced, &HookDrawIndexedInstanced, g_drawIndexedInstanced);
			logger::info("d3d: context vtable hooked ({})", a_context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE ? "immediate"sv : "deferred"sv);
		}

		void HookSwapChain(IDXGISwapChain* a_swapChain)
		{
			if (!a_swapChain || !MarkVTable(a_swapChain)) {
				return;
			}
			PatchSlot(a_swapChain, Slot::kSwapChainPresent, &HookPresent, g_present);

			Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain1;
			if (SUCCEEDED(a_swapChain->QueryInterface(IID_PPV_ARGS(swapChain1.GetAddressOf())))) {
				PatchSlot(swapChain1.Get(), Slot::kSwapChain1Present1, &HookPresent1, g_present1);
			}
			logger::info("d3d: swap chain vtable hooked (Present1: {})", swapChain1 ? "yes"sv : "no"sv);
		}

		void OnDeviceCreated(ID3D11Device* a_device, ID3D11DeviceContext* a_context, IDXGISwapChain* a_swapChain)
		{
			Microsoft::WRL::ComPtr<ID3D11DeviceContext> immediate;
			if (!a_context && a_device) {
				a_device->GetImmediateContext(immediate.GetAddressOf());
				a_context = immediate.Get();
			}
			g_hookedEarly = true;
			InstallObjectHooks(a_device, a_context, a_swapChain);
		}

		HRESULT WINAPI HookD3D11CreateDeviceAndSwapChain(
			IDXGIAdapter* a_adapter, D3D_DRIVER_TYPE a_driverType, HMODULE a_software, UINT a_flags,
			const D3D_FEATURE_LEVEL* a_featureLevels, UINT a_numFeatureLevels, UINT a_sdkVersion,
			const DXGI_SWAP_CHAIN_DESC* a_swapChainDesc, IDXGISwapChain** a_swapChain, ID3D11Device** a_device,
			D3D_FEATURE_LEVEL* a_featureLevel, ID3D11DeviceContext** a_context)
		{
			const auto result = g_createDeviceAndSwapChain(a_adapter, a_driverType, a_software, a_flags, a_featureLevels, a_numFeatureLevels,
				a_sdkVersion, a_swapChainDesc, a_swapChain, a_device, a_featureLevel, a_context);
			if (SUCCEEDED(result) && a_device && *a_device) {
				Guarded("D3D11CreateDeviceAndSwapChain", [&] { OnDeviceCreated(*a_device, a_context ? *a_context : nullptr, a_swapChain ? *a_swapChain : nullptr); });
			}
			return result;
		}

		HRESULT WINAPI HookD3D11CreateDevice(
			IDXGIAdapter* a_adapter, D3D_DRIVER_TYPE a_driverType, HMODULE a_software, UINT a_flags,
			const D3D_FEATURE_LEVEL* a_featureLevels, UINT a_numFeatureLevels, UINT a_sdkVersion,
			ID3D11Device** a_device, D3D_FEATURE_LEVEL* a_featureLevel, ID3D11DeviceContext** a_context)
		{
			const auto result = g_createDevice(a_adapter, a_driverType, a_software, a_flags, a_featureLevels, a_numFeatureLevels,
				a_sdkVersion, a_device, a_featureLevel, a_context);
			if (SUCCEEDED(result) && a_device && *a_device) {
				Guarded("D3D11CreateDevice", [&] { OnDeviceCreated(*a_device, a_context ? *a_context : nullptr, nullptr); });
			}
			return result;
		}

		// Returns the IAT slot for an imported function of the main module.
		[[nodiscard]] void** FindImportSlot(const char* a_module, const char* a_function)
		{
			auto* const base = reinterpret_cast<std::uint8_t*>(::GetModuleHandleW(nullptr));
			if (!base) {
				return nullptr;
			}

			const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
				return nullptr;
			}

			const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
			const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
			if (directory.VirtualAddress == 0) {
				return nullptr;
			}

			for (auto* descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress); descriptor->Name != 0; ++descriptor) {
				const auto* const name = reinterpret_cast<const char*>(base + descriptor->Name);
				if (::_stricmp(name, a_module) != 0) {
					continue;
				}

				auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
				const auto* lookup = descriptor->OriginalFirstThunk != 0 ?
				                         reinterpret_cast<const IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk) :
				                         thunk;

				for (; lookup->u1.AddressOfData != 0; ++lookup, ++thunk) {
					if (IMAGE_SNAP_BY_ORDINAL(lookup->u1.Ordinal)) {
						continue;
					}
					const auto* const byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + lookup->u1.AddressOfData);
					if (std::strcmp(reinterpret_cast<const char*>(byName->Name), a_function) == 0) {
						return reinterpret_cast<void**>(&thunk->u1.Function);
					}
				}
			}
			return nullptr;
		}

		template <class T>
		bool PatchImport(const char* a_function, T a_replacement, T& a_original)
		{
			auto** const slot = FindImportSlot("d3d11.dll", a_function);
			if (!slot) {
				return false;
			}

			DWORD oldProtect{};
			if (!::VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
				return false;
			}
			a_original = reinterpret_cast<T>(*slot);
			*slot = reinterpret_cast<void*>(a_replacement);
			::VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
			return true;
		}
	}

	bool InstallCreateDeviceHooks()
	{
		const bool swapChain = PatchImport("D3D11CreateDeviceAndSwapChain", &HookD3D11CreateDeviceAndSwapChain, g_createDeviceAndSwapChain);
		const bool device = PatchImport("D3D11CreateDevice", &HookD3D11CreateDevice, g_createDevice);
		logger::info("d3d: import hooks D3D11CreateDeviceAndSwapChain={} D3D11CreateDevice={}", swapChain, device);
		return swapChain || device;
	}

	void InstallObjectHooks(ID3D11Device* a_device, ID3D11DeviceContext* a_context, IDXGISwapChain* a_swapChain)
	{
		HookDevice(a_device);
		HookContext(a_context);
		HookSwapChain(a_swapChain);
	}

	bool DeviceHooked() noexcept
	{
		return g_deviceHooked;
	}

	bool HookedBeforeRendererInit() noexcept
	{
		return g_hookedEarly;
	}

	void CallDrawIndexed(ID3D11DeviceContext* a_context, UINT a_indexCount, UINT a_startIndex, INT a_baseVertex)
	{
		if (g_drawIndexed) {
			g_drawIndexed(a_context, a_indexCount, a_startIndex, a_baseVertex);
		} else {
			a_context->DrawIndexed(a_indexCount, a_startIndex, a_baseVertex);
		}
	}

	void CallDrawIndexedInstanced(ID3D11DeviceContext* a_context, UINT a_indexCount, UINT a_instanceCount, UINT a_startIndex, INT a_baseVertex, UINT a_startInstance)
	{
		if (g_drawIndexedInstanced) {
			g_drawIndexedInstanced(a_context, a_indexCount, a_instanceCount, a_startIndex, a_baseVertex, a_startInstance);
		} else {
			a_context->DrawIndexedInstanced(a_indexCount, a_instanceCount, a_startIndex, a_baseVertex, a_startInstance);
		}
	}

	HRESULT CallCreateInputLayout(ID3D11Device* a_device, const D3D11_INPUT_ELEMENT_DESC* a_descs, UINT a_count, const void* a_bytecode, SIZE_T a_length, ID3D11InputLayout** a_layout)
	{
		return g_createInputLayout ?
		           g_createInputLayout(a_device, a_descs, a_count, a_bytecode, a_length, a_layout) :
		           a_device->CreateInputLayout(a_descs, a_count, a_bytecode, a_length, a_layout);
	}
}
