#pragma once

namespace GWP
{
	class ShaderLibrary;

	// Hierarchical-Z pyramid of the main view's depth, rebuilt every frame once
	// the opaque world has been drawn. The next frame tests batch members
	// against it with the camera matrix it was built with, so static objects
	// hidden last frame are skipped this frame (Nvidium uses the same temporal
	// idea with raster queries against last frame's visibility).
	class HiZ
	{
	public:
		bool Initialize(ID3D11Device* a_device, ShaderLibrary& a_shaders);
		void Release();

		// Copies the depth texture behind a_depth and builds the pyramid for the
		// viewport region. Records the matrix and convention for later tests.
		bool Build(ID3D11DeviceContext* a_context, ID3D11DepthStencilView* a_depth, const D3D11_VIEWPORT& a_viewport, const float (&a_viewProj)[4][4], bool a_reversedZ, std::uint32_t a_frame);

		[[nodiscard]] bool UsableFor(std::uint32_t a_frame) const noexcept { return _valid && a_frame - _builtFrame == 1; }
		[[nodiscard]] ID3D11ShaderResourceView* SRV() const noexcept { return _pyramidSRV.Get(); }
		[[nodiscard]] std::uint32_t Width() const noexcept { return _width; }
		[[nodiscard]] std::uint32_t Height() const noexcept { return _height; }
		[[nodiscard]] std::uint32_t Mips() const noexcept { return _mips; }
		[[nodiscard]] bool ReversedZ() const noexcept { return _reversedZ; }
		[[nodiscard]] const float (&ViewProj() const noexcept)[4][4] { return _viewProj; }

		void Invalidate() noexcept { _valid = false; }

	private:
		bool EnsureCopy(ID3D11Texture2D* a_depth);
		bool EnsurePyramid(std::uint32_t a_width, std::uint32_t a_height);

		ID3D11Device* _device{ nullptr };
		Microsoft::WRL::ComPtr<ID3D11ComputeShader> _init;
		Microsoft::WRL::ComPtr<ID3D11ComputeShader> _reduce;
		Microsoft::WRL::ComPtr<ID3D11Buffer> _constants;

		Microsoft::WRL::ComPtr<ID3D11Texture2D> _copy;
		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _copySRV;
		D3D11_TEXTURE2D_DESC _copyDesc{};

		Microsoft::WRL::ComPtr<ID3D11Texture2D> _pyramid;
		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _pyramidSRV;
		std::vector<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> _mipSRVs;
		std::vector<Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>> _mipUAVs;
		std::uint32_t _width{ 0 };
		std::uint32_t _height{ 0 };
		std::uint32_t _mips{ 0 };

		float _viewProj[4][4]{};
		bool _reversedZ{ false };
		bool _valid{ false };
		std::uint32_t _builtFrame{ 0 };
		bool _warnedFormat{ false };
	};
}
