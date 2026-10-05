#include "Render/GpuBuffers.h"

namespace GWP
{
	void RangeAllocator::Reset(std::uint64_t a_capacity)
	{
		_free.clear();
		_capacity = a_capacity;
		_used = 0;
		if (a_capacity > 0) {
			_free.emplace(0, a_capacity);
		}
	}

	void RangeAllocator::Grow(std::uint64_t a_newCapacity)
	{
		if (a_newCapacity <= _capacity) {
			return;
		}
		InsertFree(_capacity, a_newCapacity - _capacity);
		_capacity = a_newCapacity;
	}

	std::optional<std::uint64_t> RangeAllocator::Allocate(std::uint64_t a_size, std::uint64_t a_alignment)
	{
		if (a_size == 0) {
			return std::nullopt;
		}
		a_alignment = std::max<std::uint64_t>(a_alignment, 1);

		for (auto it = _free.begin(); it != _free.end(); ++it) {
			const auto [blockOffset, blockSize] = *it;
			const auto aligned = (blockOffset + a_alignment - 1) / a_alignment * a_alignment;
			const auto padding = aligned - blockOffset;
			if (blockSize < padding || blockSize - padding < a_size) {
				continue;
			}

			_free.erase(it);
			if (padding > 0) {
				_free.emplace(blockOffset, padding);
			}
			const auto tail = blockSize - padding - a_size;
			if (tail > 0) {
				_free.emplace(aligned + a_size, tail);
			}
			_used += a_size;
			return aligned;
		}
		return std::nullopt;
	}

	void RangeAllocator::Free(std::uint64_t a_offset, std::uint64_t a_size)
	{
		if (a_size == 0) {
			return;
		}
		_used -= std::min(_used, a_size);
		InsertFree(a_offset, a_size);
	}

	void RangeAllocator::InsertFree(std::uint64_t a_offset, std::uint64_t a_size)
	{
		auto [it, inserted] = _free.emplace(a_offset, a_size);
		if (!inserted) {
			logger::error("arena: double free at offset {}", a_offset);
			return;
		}

		// Coalesce with the following block.
		if (const auto next = std::next(it); next != _free.end() && it->first + it->second == next->first) {
			it->second += next->second;
			_free.erase(next);
		}
		// Coalesce with the preceding block.
		if (it != _free.begin()) {
			const auto previous = std::prev(it);
			if (previous->first + previous->second == it->first) {
				previous->second += it->second;
				_free.erase(it);
			}
		}
	}

	bool ArenaBuffer::Create(ID3D11Device* a_device, const Desc& a_desc)
	{
		_device = a_device;
		_desc = a_desc;
		_desc.initialBytes = std::min(std::max<std::uint64_t>(_desc.initialBytes, 64 * 1024), _desc.maxBytes);
		_desc.initialBytes = (_desc.initialBytes + 255) & ~std::uint64_t{ 255 };

		if (!CreateStorage(_desc.initialBytes, _buffer, _srv, _uav)) {
			return false;
		}
		_size = _desc.initialBytes;
		_allocator.Reset(_size);
		return true;
	}

	void ArenaBuffer::Destroy()
	{
		_uav.Reset();
		_srv.Reset();
		_buffer.Reset();
		_size = 0;
		_allocator.Reset(0);
	}

	bool ArenaBuffer::CreateStorage(std::uint64_t a_bytes, Microsoft::WRL::ComPtr<ID3D11Buffer>& a_buffer, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& a_srv, Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>& a_uav) const
	{
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = static_cast<UINT>(a_bytes);
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = _desc.bindFlags;
		desc.MiscFlags = _desc.miscFlags;
		desc.StructureByteStride = _desc.structureStride;

		if (FAILED(_device->CreateBuffer(&desc, nullptr, a_buffer.ReleaseAndGetAddressOf()))) {
			logger::error("arena: failed to create {} ({} bytes)", _desc.name, a_bytes);
			return false;
		}

		if (_desc.srv) {
			D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
			if (_desc.structureStride != 0) {
				srv.Format = DXGI_FORMAT_UNKNOWN;
				srv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
				srv.Buffer.FirstElement = 0;
				srv.Buffer.NumElements = static_cast<UINT>(a_bytes / _desc.structureStride);
			} else {
				srv.Format = DXGI_FORMAT_R32_TYPELESS;
				srv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
				srv.BufferEx.FirstElement = 0;
				srv.BufferEx.NumElements = static_cast<UINT>(a_bytes / 4);
				srv.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
			}
			if (FAILED(_device->CreateShaderResourceView(a_buffer.Get(), &srv, a_srv.ReleaseAndGetAddressOf()))) {
				logger::error("arena: failed to create SRV for {}", _desc.name);
				return false;
			}
		}

		if (_desc.uav) {
			D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
			uav.Format = DXGI_FORMAT_R32_TYPELESS;
			uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
			uav.Buffer.FirstElement = 0;
			uav.Buffer.NumElements = static_cast<UINT>(a_bytes / 4);
			uav.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
			if (FAILED(_device->CreateUnorderedAccessView(a_buffer.Get(), &uav, a_uav.ReleaseAndGetAddressOf()))) {
				logger::error("arena: failed to create UAV for {}", _desc.name);
				return false;
			}
		}
		return true;
	}

	std::optional<std::uint64_t> ArenaBuffer::Allocate(ID3D11DeviceContext* a_context, std::uint64_t a_size, std::uint64_t a_alignment)
	{
		if (auto offset = _allocator.Allocate(a_size, a_alignment)) {
			return offset;
		}

		// Grow geometrically until the request fits or the budget is exhausted.
		auto newSize = _size;
		while (newSize < _desc.maxBytes) {
			newSize = std::min(newSize * 2, _desc.maxBytes);
			if (newSize - _allocator.Used() >= a_size + a_alignment) {
				break;
			}
		}
		if (newSize <= _size) {
			return std::nullopt;
		}

		Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
		Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;
		if (!CreateStorage(newSize, buffer, srv, uav)) {
			return std::nullopt;
		}

		D3D11_BOX box{};
		box.left = 0;
		box.right = static_cast<UINT>(_size);
		box.top = 0;
		box.bottom = 1;
		box.front = 0;
		box.back = 1;
		a_context->CopySubresourceRegion(buffer.Get(), 0, 0, 0, 0, _buffer.Get(), 0, &box);

		logger::info("arena: {} grew {} -> {} MB", _desc.name, _size >> 20, newSize >> 20);

		_buffer = std::move(buffer);
		_srv = std::move(srv);
		_uav = std::move(uav);
		_allocator.Grow(newSize);
		_size = newSize;

		return _allocator.Allocate(a_size, a_alignment);
	}

	bool DynamicBuffer::Create(ID3D11Device* a_device, UINT a_bindFlags, UINT a_structureStride, std::uint32_t a_bytes, const char* a_name)
	{
		_device = a_device;
		_bindFlags = a_bindFlags;
		_stride = a_structureStride;
		_name = a_name;
		return Recreate(a_bytes);
	}

	bool DynamicBuffer::Recreate(std::uint32_t a_bytes)
	{
		a_bytes = std::max<std::uint32_t>(a_bytes, 256);
		if (_stride) {
			a_bytes = (a_bytes + _stride - 1) / _stride * _stride;
		}

		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = a_bytes;
		desc.Usage = D3D11_USAGE_DYNAMIC;
		desc.BindFlags = _bindFlags;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		desc.MiscFlags = _stride ? D3D11_RESOURCE_MISC_BUFFER_STRUCTURED : 0;
		desc.StructureByteStride = _stride;

		if (FAILED(_device->CreateBuffer(&desc, nullptr, _buffer.ReleaseAndGetAddressOf()))) {
			logger::error("dynamic: failed to create {} ({} bytes)", _name, a_bytes);
			return false;
		}

		_srv.Reset();
		if (_bindFlags & D3D11_BIND_SHADER_RESOURCE) {
			D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = DXGI_FORMAT_UNKNOWN;
			srv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
			srv.Buffer.FirstElement = 0;
			srv.Buffer.NumElements = a_bytes / std::max<UINT>(_stride, 1);
			if (FAILED(_device->CreateShaderResourceView(_buffer.Get(), &srv, _srv.GetAddressOf()))) {
				logger::error("dynamic: failed to create SRV for {}", _name);
				return false;
			}
		}

		_size = a_bytes;
		return true;
	}

	bool DynamicBuffer::Upload(ID3D11DeviceContext* a_context, const void* a_data, std::uint32_t a_bytes)
	{
		if (a_bytes > _size && !Recreate(std::bit_ceil(a_bytes))) {
			return false;
		}

		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(a_context->Map(_buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			return false;
		}
		if (a_bytes) {
			std::memcpy(mapped.pData, a_data, a_bytes);
		}
		a_context->Unmap(_buffer.Get(), 0);
		return true;
	}

	Microsoft::WRL::ComPtr<ID3D11Buffer> CreateConstantBuffer(ID3D11Device* a_device, UINT a_bytes)
	{
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = (a_bytes + 15) & ~15u;
		desc.Usage = D3D11_USAGE_DYNAMIC;
		desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

		Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
		if (FAILED(a_device->CreateBuffer(&desc, nullptr, buffer.GetAddressOf()))) {
			logger::error("dynamic: failed to create constant buffer ({} bytes)", a_bytes);
		}
		return buffer;
	}
}
