#pragma once

namespace GWP
{
	// The renderer caches its own D3D11 bindings (BSGraphics shadow state), so
	// every binding the pipeline changes mid-frame must be put back exactly.

	class ComputeStateGuard
	{
	public:
		static constexpr UINT kCBs = 1;
		static constexpr UINT kSRVs = 4;
		static constexpr UINT kUAVs = 2;

		explicit ComputeStateGuard(ID3D11DeviceContext* a_context) :
			_context(a_context)
		{
			_context->CSGetShader(&_shader, nullptr, nullptr);
			_context->CSGetConstantBuffers(0, kCBs, _cbs);
			_context->CSGetShaderResources(0, kSRVs, _srvs);
			_context->CSGetUnorderedAccessViews(0, kUAVs, _uavs);
		}

		ComputeStateGuard(const ComputeStateGuard&) = delete;
		ComputeStateGuard& operator=(const ComputeStateGuard&) = delete;

		~ComputeStateGuard()
		{
			// Unbind our UAVs before restoring SRVs so a resource never sits
			// in both tables at once.
			constexpr UINT keepCounters[kUAVs]{ static_cast<UINT>(-1), static_cast<UINT>(-1) };
			_context->CSSetUnorderedAccessViews(0, kUAVs, _uavs, keepCounters);
			_context->CSSetShaderResources(0, kSRVs, _srvs);
			_context->CSSetConstantBuffers(0, kCBs, _cbs);
			_context->CSSetShader(_shader, nullptr, 0);

			Release(_shader);
			for (auto* cb : _cbs) {
				Release(cb);
			}
			for (auto* srv : _srvs) {
				Release(srv);
			}
			for (auto* uav : _uavs) {
				Release(uav);
			}
		}

	private:
		template <class T>
		static void Release(T* a_object)
		{
			if (a_object) {
				a_object->Release();
			}
		}

		ID3D11DeviceContext* _context;
		ID3D11ComputeShader* _shader{ nullptr };
		ID3D11Buffer* _cbs[kCBs]{};
		ID3D11ShaderResourceView* _srvs[kSRVs]{};
		ID3D11UnorderedAccessView* _uavs[kUAVs]{};
	};

	class InputAssemblerGuard
	{
	public:
		explicit InputAssemblerGuard(ID3D11DeviceContext* a_context) :
			_context(a_context)
		{
			_context->IAGetIndexBuffer(&_indexBuffer, &_indexFormat, &_indexOffset);
			_context->IAGetVertexBuffers(0, 1, &_vertexBuffer, &_stride, &_vertexOffset);
			_context->IAGetInputLayout(&_layout);
		}

		InputAssemblerGuard(const InputAssemblerGuard&) = delete;
		InputAssemblerGuard& operator=(const InputAssemblerGuard&) = delete;

		~InputAssemblerGuard()
		{
			_context->IASetIndexBuffer(_indexBuffer, _indexFormat, _indexOffset);
			_context->IASetVertexBuffers(0, 1, &_vertexBuffer, &_stride, &_vertexOffset);
			_context->IASetInputLayout(_layout);

			if (_indexBuffer) {
				_indexBuffer->Release();
			}
			if (_vertexBuffer) {
				_vertexBuffer->Release();
			}
			if (_layout) {
				_layout->Release();
			}
		}

		[[nodiscard]] ID3D11Buffer* IndexBuffer() const noexcept { return _indexBuffer; }
		[[nodiscard]] DXGI_FORMAT IndexFormat() const noexcept { return _indexFormat; }
		[[nodiscard]] UINT IndexOffset() const noexcept { return _indexOffset; }
		[[nodiscard]] ID3D11Buffer* VertexBuffer() const noexcept { return _vertexBuffer; }
		[[nodiscard]] UINT Stride() const noexcept { return _stride; }
		[[nodiscard]] UINT VertexOffset() const noexcept { return _vertexOffset; }
		[[nodiscard]] ID3D11InputLayout* Layout() const noexcept { return _layout; }

	private:
		ID3D11DeviceContext* _context;
		ID3D11Buffer* _indexBuffer{ nullptr };
		DXGI_FORMAT _indexFormat{ DXGI_FORMAT_UNKNOWN };
		UINT _indexOffset{ 0 };
		ID3D11Buffer* _vertexBuffer{ nullptr };
		UINT _stride{ 0 };
		UINT _vertexOffset{ 0 };
		ID3D11InputLayout* _layout{ nullptr };
	};
}
