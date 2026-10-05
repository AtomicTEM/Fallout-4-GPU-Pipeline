#include "Render/HiZ.h"

#include "Render/GpuBuffers.h"
#include "Render/ShaderConstants.h"
#include "Render/ShaderLibrary.h"
#include "Render/StateGuards.h"

namespace GWP
{
	namespace
	{
		using HiZConstants = ShaderConstants::HiZ;

		// Depth formats are copied as their typeless family and read through
		// the matching depth-as-color SRV format.
		struct DepthFormats
		{
			DXGI_FORMAT typeless;
			DXGI_FORMAT read;
		};

		[[nodiscard]] std::optional<DepthFormats> ResolveDepthFormat(DXGI_FORMAT a_format) noexcept
		{
			switch (a_format) {
			case DXGI_FORMAT_R24G8_TYPELESS:
			case DXGI_FORMAT_D24_UNORM_S8_UINT:
			case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
				return DepthFormats{ DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_R24_UNORM_X8_TYPELESS };
			case DXGI_FORMAT_R32_TYPELESS:
			case DXGI_FORMAT_D32_FLOAT:
			case DXGI_FORMAT_R32_FLOAT:
				return DepthFormats{ DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_FLOAT };
			case DXGI_FORMAT_R32G8X24_TYPELESS:
			case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
			case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
				return DepthFormats{ DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS };
			case DXGI_FORMAT_R16_TYPELESS:
			case DXGI_FORMAT_D16_UNORM:
			case DXGI_FORMAT_R16_UNORM:
				return DepthFormats{ DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UNORM };
			default:
				return std::nullopt;
			}
		}
	}

	bool HiZ::Initialize(ID3D11Device* a_device, ShaderLibrary& a_shaders)
	{
		_device = a_device;
		_init = a_shaders.CompileCompute("HiZ.hlsl", "CSInit");
		_reduce = a_shaders.CompileCompute("HiZ.hlsl", "CSReduce");
		_constants = CreateConstantBuffer(a_device, sizeof(HiZConstants));
		return _init && _reduce && _constants;
	}

	void HiZ::Release()
	{
		_copySRV.Reset();
		_copy.Reset();
		_mipSRVs.clear();
		_mipUAVs.clear();
		_pyramidSRV.Reset();
		_pyramid.Reset();
		_valid = false;
	}

	bool HiZ::EnsureCopy(ID3D11Texture2D* a_depth)
	{
		D3D11_TEXTURE2D_DESC desc{};
		a_depth->GetDesc(&desc);

		if (_copy && desc.Width == _copyDesc.Width && desc.Height == _copyDesc.Height && desc.Format == _copyDesc.Format &&
			desc.ArraySize == _copyDesc.ArraySize && desc.MipLevels == _copyDesc.MipLevels) {
			return true;
		}

		if (desc.SampleDesc.Count != 1 || desc.ArraySize != 1) {
			if (!_warnedFormat) {
				logger::warn("hiz: depth target is multisampled or an array, occlusion culling disabled");
				_warnedFormat = true;
			}
			return false;
		}

		const auto formats = ResolveDepthFormat(desc.Format);
		if (!formats) {
			if (!_warnedFormat) {
				logger::warn("hiz: unsupported depth format {}, occlusion culling disabled", static_cast<std::uint32_t>(desc.Format));
				_warnedFormat = true;
			}
			return false;
		}

		D3D11_TEXTURE2D_DESC copy = desc;
		copy.Format = formats->typeless;
		copy.Usage = D3D11_USAGE_DEFAULT;
		copy.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		copy.CPUAccessFlags = 0;
		copy.MiscFlags = 0;
		if (FAILED(_device->CreateTexture2D(&copy, nullptr, _copy.ReleaseAndGetAddressOf()))) {
			logger::error("hiz: failed to create depth copy {}x{}", desc.Width, desc.Height);
			return false;
		}

		D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Format = formats->read;
		srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srv.Texture2D.MostDetailedMip = 0;
		srv.Texture2D.MipLevels = 1;
		if (FAILED(_device->CreateShaderResourceView(_copy.Get(), &srv, _copySRV.ReleaseAndGetAddressOf()))) {
			logger::error("hiz: failed to create depth copy SRV");
			_copy.Reset();
			return false;
		}

		_copyDesc = desc;
		logger::info("hiz: depth copy {}x{} format {}", desc.Width, desc.Height, static_cast<std::uint32_t>(desc.Format));
		return true;
	}

	bool HiZ::EnsurePyramid(std::uint32_t a_width, std::uint32_t a_height)
	{
		if (_pyramid && a_width == _width && a_height == _height) {
			return true;
		}

		_mips = static_cast<std::uint32_t>(std::bit_width(std::max(a_width, a_height)));

		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = a_width;
		desc.Height = a_height;
		desc.MipLevels = _mips;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R32_FLOAT;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		if (FAILED(_device->CreateTexture2D(&desc, nullptr, _pyramid.ReleaseAndGetAddressOf()))) {
			logger::error("hiz: failed to create {}x{} pyramid", a_width, a_height);
			return false;
		}

		D3D11_SHADER_RESOURCE_VIEW_DESC all{};
		all.Format = DXGI_FORMAT_R32_FLOAT;
		all.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		all.Texture2D.MostDetailedMip = 0;
		all.Texture2D.MipLevels = _mips;
		if (FAILED(_device->CreateShaderResourceView(_pyramid.Get(), &all, _pyramidSRV.ReleaseAndGetAddressOf()))) {
			return false;
		}

		_mipSRVs.assign(_mips, nullptr);
		_mipUAVs.assign(_mips, nullptr);
		for (std::uint32_t mip = 0; mip < _mips; ++mip) {
			D3D11_SHADER_RESOURCE_VIEW_DESC srv = all;
			srv.Texture2D.MostDetailedMip = mip;
			srv.Texture2D.MipLevels = 1;
			D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
			uav.Format = DXGI_FORMAT_R32_FLOAT;
			uav.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			uav.Texture2D.MipSlice = mip;
			if (FAILED(_device->CreateShaderResourceView(_pyramid.Get(), &srv, _mipSRVs[mip].GetAddressOf())) ||
				FAILED(_device->CreateUnorderedAccessView(_pyramid.Get(), &uav, _mipUAVs[mip].GetAddressOf()))) {
				logger::error("hiz: failed to create views for mip {}", mip);
				_pyramid.Reset();
				return false;
			}
		}

		_width = a_width;
		_height = a_height;
		logger::info("hiz: pyramid {}x{} with {} mips", a_width, a_height, _mips);
		return true;
	}

	bool HiZ::Build(ID3D11DeviceContext* a_context, ID3D11DepthStencilView* a_depth, const D3D11_VIEWPORT& a_viewport, const float (&a_viewProj)[4][4], bool a_reversedZ, std::uint32_t a_frame)
	{
		_valid = false;
		if (!a_depth) {
			return false;
		}

		Microsoft::WRL::ComPtr<ID3D11Resource> resource;
		a_depth->GetResource(resource.GetAddressOf());
		Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
		if (FAILED(resource.As(&texture)) || !EnsureCopy(texture.Get())) {
			return false;
		}

		const auto x = static_cast<std::uint32_t>(std::max(a_viewport.TopLeftX, 0.0F));
		const auto y = static_cast<std::uint32_t>(std::max(a_viewport.TopLeftY, 0.0F));
		const auto width = std::min(static_cast<std::uint32_t>(a_viewport.Width), _copyDesc.Width - std::min(x, _copyDesc.Width));
		const auto height = std::min(static_cast<std::uint32_t>(a_viewport.Height), _copyDesc.Height - std::min(y, _copyDesc.Height));
		if (width < 8 || height < 8 || !EnsurePyramid(width, height)) {
			return false;
		}

		// Copying (instead of reading the depth directly) leaves the engine's
		// output-merger bindings untouched; copies from a bound depth buffer are legal.
		a_context->CopyResource(_copy.Get(), texture.Get());

		ComputeStateGuard guard{ a_context };
		ID3D11Buffer* const constants = _constants.Get();
		a_context->CSSetConstantBuffers(0, 1, &constants);

		HiZConstants params{};
		params.sourceOffset[0] = x;
		params.sourceOffset[1] = y;
		params.sourceSize[0] = width;
		params.sourceSize[1] = height;
		params.destSize[0] = width;
		params.destSize[1] = height;
		params.reversedZ = a_reversedZ ? 1 : 0;
		UploadConstants(a_context, constants, params);

		ID3D11ShaderResourceView* source = _copySRV.Get();
		ID3D11UnorderedAccessView* target = _mipUAVs[0].Get();
		a_context->CSSetShader(_init.Get(), nullptr, 0);
		a_context->CSSetShaderResources(0, 1, &source);
		a_context->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
		a_context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);

		a_context->CSSetShader(_reduce.Get(), nullptr, 0);
		std::uint32_t sourceWidth = width;
		std::uint32_t sourceHeight = height;
		for (std::uint32_t mip = 1; mip < _mips; ++mip) {
			const auto destWidth = std::max(sourceWidth / 2, 1u);
			const auto destHeight = std::max(sourceHeight / 2, 1u);

			params.sourceOffset[0] = 0;
			params.sourceOffset[1] = 0;
			params.sourceSize[0] = sourceWidth;
			params.sourceSize[1] = sourceHeight;
			params.destSize[0] = destWidth;
			params.destSize[1] = destHeight;
			UploadConstants(a_context, constants, params);

			// Unbind the previous target before reading it.
			ID3D11UnorderedAccessView* const none = nullptr;
			a_context->CSSetUnorderedAccessViews(0, 1, &none, nullptr);
			source = _mipSRVs[mip - 1].Get();
			target = _mipUAVs[mip].Get();
			a_context->CSSetShaderResources(0, 1, &source);
			a_context->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
			a_context->Dispatch((destWidth + 7) / 8, (destHeight + 7) / 8, 1);

			sourceWidth = destWidth;
			sourceHeight = destHeight;
		}

		ID3D11UnorderedAccessView* const none = nullptr;
		ID3D11ShaderResourceView* const noSource = nullptr;
		a_context->CSSetUnorderedAccessViews(0, 1, &none, nullptr);
		a_context->CSSetShaderResources(0, 1, &noSource);

		std::memcpy(_viewProj, a_viewProj, sizeof(_viewProj));
		_reversedZ = a_reversedZ;
		_builtFrame = a_frame;
		_valid = true;
		return true;
	}
}
