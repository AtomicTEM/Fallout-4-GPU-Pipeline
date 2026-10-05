#pragma once

namespace GWP
{
	// First-fit free list over a linear byte range.
	class RangeAllocator
	{
	public:
		void Reset(std::uint64_t a_capacity);
		void Grow(std::uint64_t a_newCapacity);

		[[nodiscard]] std::optional<std::uint64_t> Allocate(std::uint64_t a_size, std::uint64_t a_alignment);
		void Free(std::uint64_t a_offset, std::uint64_t a_size);

		[[nodiscard]] std::uint64_t Capacity() const noexcept { return _capacity; }
		[[nodiscard]] std::uint64_t Used() const noexcept { return _used; }

	private:
		void InsertFree(std::uint64_t a_offset, std::uint64_t a_size);

		std::map<std::uint64_t, std::uint64_t> _free;  // offset -> size
		std::uint64_t _capacity{ 0 };
		std::uint64_t _used{ 0 };
	};

	// A DEFAULT-usage buffer that can grow (by copying on the GPU) up to a
	// maximum size, with optional raw/structured SRV and raw UAV views.
	class ArenaBuffer
	{
	public:
		struct Desc
		{
			const char* name{ "arena" };
			UINT bindFlags{ 0 };
			UINT miscFlags{ 0 };
			UINT structureStride{ 0 };  // non-zero: structured SRV instead of raw
			std::uint64_t initialBytes{ 0 };
			std::uint64_t maxBytes{ 0 };
			bool srv{ false };
			bool uav{ false };
		};

		bool Create(ID3D11Device* a_device, const Desc& a_desc);
		void Destroy();

		// Allocates a range, growing the buffer when needed. Growth records a
		// GPU copy on a_context, so it must be called on the render thread.
		[[nodiscard]] std::optional<std::uint64_t> Allocate(ID3D11DeviceContext* a_context, std::uint64_t a_size, std::uint64_t a_alignment);
		void Free(std::uint64_t a_offset, std::uint64_t a_size) { _allocator.Free(a_offset, a_size); }

		[[nodiscard]] ID3D11Buffer* Buffer() const noexcept { return _buffer.Get(); }
		[[nodiscard]] ID3D11ShaderResourceView* SRV() const noexcept { return _srv.Get(); }
		[[nodiscard]] ID3D11UnorderedAccessView* UAV() const noexcept { return _uav.Get(); }
		[[nodiscard]] std::uint64_t Size() const noexcept { return _size; }
		[[nodiscard]] std::uint64_t Used() const noexcept { return _allocator.Used(); }
		[[nodiscard]] std::uint64_t MaxSize() const noexcept { return _desc.maxBytes; }

	private:
		bool CreateStorage(std::uint64_t a_bytes, Microsoft::WRL::ComPtr<ID3D11Buffer>& a_buffer, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& a_srv, Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>& a_uav) const;

		ID3D11Device* _device{ nullptr };
		Desc _desc;
		std::uint64_t _size{ 0 };
		RangeAllocator _allocator;
		Microsoft::WRL::ComPtr<ID3D11Buffer> _buffer;
		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _srv;
		Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> _uav;
	};

	// CPU-writable buffer (D3D11_USAGE_DYNAMIC), recreated larger on demand.
	// Its SRV is structured with a stride, otherwise a typed R32_UINT view.
	class DynamicBuffer
	{
	public:
		bool Create(ID3D11Device* a_device, UINT a_bindFlags, UINT a_structureStride, std::uint32_t a_bytes, const char* a_name);

		void Destroy();

		// Uploads a_bytes from a_data with WRITE_DISCARD, growing first if needed.
		bool Upload(ID3D11DeviceContext* a_context, const void* a_data, std::uint32_t a_bytes);

		[[nodiscard]] ID3D11Buffer* Buffer() const noexcept { return _buffer.Get(); }
		[[nodiscard]] ID3D11ShaderResourceView* SRV() const noexcept { return _srv.Get(); }

	private:
		bool Recreate(std::uint32_t a_bytes);

		ID3D11Device* _device{ nullptr };
		UINT _bindFlags{ 0 };
		UINT _stride{ 0 };
		std::uint32_t _size{ 0 };
		const char* _name{ "dynamic" };
		Microsoft::WRL::ComPtr<ID3D11Buffer> _buffer;
		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _srv;
	};

	template <class T>
	bool UploadConstants(ID3D11DeviceContext* a_context, ID3D11Buffer* a_buffer, const T& a_data)
	{
		static_assert(sizeof(T) % 16 == 0, "constant buffer structs must be 16-byte multiples");
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(a_context->Map(a_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			return false;
		}
		std::memcpy(mapped.pData, &a_data, sizeof(T));
		a_context->Unmap(a_buffer, 0);
		return true;
	}

	[[nodiscard]] Microsoft::WRL::ComPtr<ID3D11Buffer> CreateConstantBuffer(ID3D11Device* a_device, UINT a_bytes);
}
