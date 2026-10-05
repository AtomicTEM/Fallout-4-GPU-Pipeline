#include "Render/InputLayouts.h"

#include "Render/D3DHooks.h"

namespace GWP
{
	std::uint32_t FormatByteSize(DXGI_FORMAT a_format) noexcept
	{
		switch (a_format) {
		case DXGI_FORMAT_R32G32B32A32_FLOAT:
		case DXGI_FORMAT_R32G32B32A32_UINT:
		case DXGI_FORMAT_R32G32B32A32_SINT:
			return 16;
		case DXGI_FORMAT_R32G32B32_FLOAT:
		case DXGI_FORMAT_R32G32B32_UINT:
		case DXGI_FORMAT_R32G32B32_SINT:
			return 12;
		case DXGI_FORMAT_R16G16B16A16_FLOAT:
		case DXGI_FORMAT_R16G16B16A16_UNORM:
		case DXGI_FORMAT_R16G16B16A16_UINT:
		case DXGI_FORMAT_R16G16B16A16_SNORM:
		case DXGI_FORMAT_R16G16B16A16_SINT:
		case DXGI_FORMAT_R32G32_FLOAT:
		case DXGI_FORMAT_R32G32_UINT:
		case DXGI_FORMAT_R32G32_SINT:
			return 8;
		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_R8G8B8A8_UINT:
		case DXGI_FORMAT_R8G8B8A8_SNORM:
		case DXGI_FORMAT_R8G8B8A8_SINT:
		case DXGI_FORMAT_B8G8R8A8_UNORM:
		case DXGI_FORMAT_R10G10B10A2_UNORM:
		case DXGI_FORMAT_R10G10B10A2_UINT:
		case DXGI_FORMAT_R11G11B10_FLOAT:
		case DXGI_FORMAT_R16G16_FLOAT:
		case DXGI_FORMAT_R16G16_UNORM:
		case DXGI_FORMAT_R16G16_UINT:
		case DXGI_FORMAT_R16G16_SNORM:
		case DXGI_FORMAT_R16G16_SINT:
		case DXGI_FORMAT_R32_FLOAT:
		case DXGI_FORMAT_R32_UINT:
		case DXGI_FORMAT_R32_SINT:
			return 4;
		case DXGI_FORMAT_R8G8_UNORM:
		case DXGI_FORMAT_R8G8_UINT:
		case DXGI_FORMAT_R8G8_SNORM:
		case DXGI_FORMAT_R8G8_SINT:
		case DXGI_FORMAT_R16_FLOAT:
		case DXGI_FORMAT_R16_UNORM:
		case DXGI_FORMAT_R16_UINT:
		case DXGI_FORMAT_R16_SNORM:
		case DXGI_FORMAT_R16_SINT:
			return 2;
		case DXGI_FORMAT_R8_UNORM:
		case DXGI_FORMAT_R8_UINT:
		case DXGI_FORMAT_R8_SNORM:
		case DXGI_FORMAT_R8_SINT:
			return 1;
		default:
			return 0;
		}
	}

	void InputLayouts::OnCreated(ID3D11InputLayout* a_layout, const D3D11_INPUT_ELEMENT_DESC* a_descs, UINT a_count, const void* a_bytecode, SIZE_T a_length)
	{
		auto layout = std::make_shared<Layout>();
		layout->elements.reserve(a_count);

		// Resolve D3D11_APPEND_ALIGNED_ELEMENT per input slot.
		std::array<UINT, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> cursor{};
		for (UINT i = 0; i < a_count; ++i) {
			const auto& desc = a_descs[i];
			if (desc.InputSlot >= cursor.size()) {
				return;
			}

			Element element;
			element.semanticName = desc.SemanticName ? desc.SemanticName : "";
			element.semanticIndex = desc.SemanticIndex;
			element.format = desc.Format;
			element.inputSlot = desc.InputSlot;
			element.alignedByteOffset = desc.AlignedByteOffset == D3D11_APPEND_ALIGNED_ELEMENT ? cursor[desc.InputSlot] : desc.AlignedByteOffset;
			element.inputSlotClass = desc.InputSlotClass;
			element.instanceDataStepRate = desc.InstanceDataStepRate;

			cursor[desc.InputSlot] = element.alignedByteOffset + FormatByteSize(desc.Format);
			layout->elements.push_back(std::move(element));
		}

		if (a_bytecode && a_length) {
			const auto* const bytes = static_cast<const std::uint8_t*>(a_bytecode);
			layout->bytecode.assign(bytes, bytes + a_length);
		}

		std::unique_lock lock{ _lock };
		// A recycled pointer means the old layout was released; replace it.
		_layouts[a_layout] = std::move(layout);
	}

	std::shared_ptr<const InputLayouts::Layout> InputLayouts::Find(ID3D11InputLayout* a_layout) const
	{
		std::shared_lock lock{ _lock };
		const auto it = _layouts.find(a_layout);
		return it != _layouts.end() ? it->second : nullptr;
	}

	ID3D11InputLayout* InputLayouts::GetWidenedVariant(ID3D11Device* a_device, ID3D11InputLayout* a_source, std::uint32_t a_positionBytes, std::uint32_t a_shift)
	{
		if (!a_source) {
			return nullptr;
		}
		if (a_shift == 0) {
			return a_source;
		}

		const auto source = Find(a_source);
		if (!source || source->bytecode.empty()) {
			return nullptr;
		}

		{
			std::scoped_lock lock{ _variantLock };
			const auto it = _variants.find({ a_source, a_shift });
			if (it != _variants.end() && it->second.source == source) {
				return it->second.layout.Get();
			}
		}

		std::vector<D3D11_INPUT_ELEMENT_DESC> descs;
		descs.reserve(source->elements.size());
		for (const auto& element : source->elements) {
			D3D11_INPUT_ELEMENT_DESC desc{};
			desc.SemanticName = element.semanticName.c_str();
			desc.SemanticIndex = element.semanticIndex;
			desc.Format = element.format;
			desc.InputSlot = element.inputSlot;
			desc.AlignedByteOffset = element.alignedByteOffset;
			desc.InputSlotClass = element.inputSlotClass;
			desc.InstanceDataStepRate = element.instanceDataStepRate;

			if (element.inputSlot == 0) {
				if (element.alignedByteOffset == 0) {
					// POSITION lives at offset 0 of every Creation Engine vertex.
					if (element.format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
						desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
					} else if (element.format != DXGI_FORMAT_R32G32B32A32_FLOAT && element.format != DXGI_FORMAT_R32G32B32_FLOAT) {
						logger::warn("layouts: unexpected POSITION format {} in layout {}", static_cast<std::uint32_t>(element.format), fmt::ptr(a_source));
						return nullptr;
					}
				} else if (element.alignedByteOffset >= a_positionBytes) {
					desc.AlignedByteOffset = element.alignedByteOffset + a_shift;
				} else {
					// An element inside the position block other than POSITION itself.
					return nullptr;
				}
			}
			descs.push_back(desc);
		}

		Microsoft::WRL::ComPtr<ID3D11InputLayout> variant;
		const auto result = D3DHooks::CallCreateInputLayout(a_device, descs.data(), static_cast<UINT>(descs.size()), source->bytecode.data(), source->bytecode.size(), variant.GetAddressOf());
		if (FAILED(result)) {
			logger::error("layouts: failed to create widened variant of {} (0x{:08X})", fmt::ptr(a_source), static_cast<std::uint32_t>(result));
			return nullptr;
		}

		std::scoped_lock lock{ _variantLock };
		auto& slot = _variants[{ a_source, a_shift }];
		if (!slot.layout || slot.source != source) {
			slot = { std::move(variant), source };
		}
		return slot.layout.Get();
	}

	std::size_t InputLayouts::Count() const
	{
		std::shared_lock lock{ _lock };
		return _layouts.size();
	}

	void InputLayouts::ReleaseVariants()
	{
		std::scoped_lock lock{ _variantLock };
		_variants.clear();
	}
}
