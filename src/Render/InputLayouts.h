#pragma once

namespace GWP
{
	// Records every input layout the renderer creates so batch draws can bind a
	// variant whose POSITION element reads the full-precision positions of the
	// merged vertex buffers.
	class InputLayouts
	{
	public:
		struct Element
		{
			std::string semanticName;
			UINT semanticIndex{ 0 };
			DXGI_FORMAT format{ DXGI_FORMAT_UNKNOWN };
			UINT inputSlot{ 0 };
			UINT alignedByteOffset{ 0 };  // resolved, never D3D11_APPEND_ALIGNED_ELEMENT
			D3D11_INPUT_CLASSIFICATION inputSlotClass{ D3D11_INPUT_PER_VERTEX_DATA };
			UINT instanceDataStepRate{ 0 };
		};

		struct Layout
		{
			std::vector<Element> elements;
			std::vector<std::uint8_t> bytecode;  // VS signature the layout was validated against
		};

		[[nodiscard]] static InputLayouts& Get() noexcept
		{
			static InputLayouts singleton;
			return singleton;
		}

		void OnCreated(ID3D11InputLayout* a_layout, const D3D11_INPUT_ELEMENT_DESC* a_descs, UINT a_count, const void* a_bytecode, SIZE_T a_length);

		[[nodiscard]] std::shared_ptr<const Layout> Find(ID3D11InputLayout* a_layout) const;

		// Returns a layout identical to a_source except that every slot-0
		// element at or beyond a_positionBytes is shifted by a_shift bytes and
		// a half-precision POSITION is widened to R32G32B32A32_FLOAT. Returns
		// a_source itself when no change is needed, nullptr when the source
		// layout is unknown or incompatible.
		[[nodiscard]] ID3D11InputLayout* GetWidenedVariant(ID3D11Device* a_device, ID3D11InputLayout* a_source, std::uint32_t a_positionBytes, std::uint32_t a_shift);

		[[nodiscard]] std::size_t Count() const;

		void ReleaseVariants();

	private:
		InputLayouts() = default;

		mutable std::shared_mutex _lock;
		std::unordered_map<ID3D11InputLayout*, std::shared_ptr<const Layout>> _layouts;

		struct VariantKey
		{
			ID3D11InputLayout* source;
			std::uint32_t shift;

			bool operator==(const VariantKey&) const = default;
		};

		struct VariantKeyHash
		{
			std::size_t operator()(const VariantKey& a_key) const noexcept
			{
				return std::hash<const void*>{}(a_key.source) ^ (static_cast<std::size_t>(a_key.shift) << 1);
			}
		};

		std::mutex _variantLock;
		std::unordered_map<VariantKey, Microsoft::WRL::ComPtr<ID3D11InputLayout>, VariantKeyHash> _variants;
	};

	[[nodiscard]] std::uint32_t FormatByteSize(DXGI_FORMAT a_format) noexcept;
}
