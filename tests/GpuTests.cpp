// Functional tests for the GPU side of the plugin. They run the real HLSL
// (compiled by d3dcompiler_47 exactly as the plugin does) and the real C++
// modules on a Direct3D 11 device, without Fallout 4.
//
//   Windows:  GpuTests.exe
//   Linux:    tools/run-tests-wine.sh (Wine + Mesa llvmpipe)

#include "Render/GpuBuffers.h"
#include "Render/HiZ.h"
#include "Render/ShaderConstants.h"
#include "Render/ShaderLibrary.h"
#include "Scene/MergeMath.h"
#include "Scene/VertexFormat.h"
#include "Settings.h"
#include "Util/Math.h"

using Microsoft::WRL::ComPtr;
using namespace GWP;

namespace
{
	int g_checks = 0;
	int g_failures = 0;

	void Check(bool a_ok, std::string_view a_what, const char* a_file, int a_line)
	{
		++g_checks;
		if (!a_ok) {
			++g_failures;
			std::printf("  FAILED %s:%d: %.*s\n", a_file, a_line, static_cast<int>(a_what.size()), a_what.data());
		}
	}

#define CHECK(expr) Check((expr), #expr, __FILE__, __LINE__)
#define CHECK_NEAR(a, b, tolerance)                                                                                     \
	do {                                                                                                                \
		const double checkA = static_cast<double>(a);                                                                   \
		const double checkB = static_cast<double>(b);                                                                   \
		Check(std::abs(checkA - checkB) <= (tolerance), fmt::format("{} ~ {} ({} vs {})", #a, #b, checkA, checkB), __FILE__, __LINE__); \
	} while (false)

	// ---------------------------------------------------------------------
	// helpers

	std::uint16_t ToHalf(float a_value)
	{
		std::uint32_t bits;
		std::memcpy(&bits, &a_value, 4);
		const std::uint32_t sign = (bits >> 16) & 0x8000;
		const std::int32_t exponent = static_cast<std::int32_t>((bits >> 23) & 0xFF) - 127 + 15;
		std::uint32_t mantissa = bits & 0x7FFFFF;
		if (exponent <= 0) {
			if (exponent < -10) {
				return static_cast<std::uint16_t>(sign);
			}
			mantissa |= 0x800000;
			const auto shift = static_cast<std::uint32_t>(14 - exponent);
			auto half = mantissa >> shift;
			if ((mantissa >> (shift - 1)) & 1) {
				++half;
			}
			return static_cast<std::uint16_t>(sign | half);
		}
		if (exponent >= 31) {
			return static_cast<std::uint16_t>(sign | 0x7C00);
		}
		auto half = sign | (static_cast<std::uint32_t>(exponent) << 10) | (mantissa >> 13);
		if (mantissa & 0x1000) {
			++half;
		}
		return static_cast<std::uint16_t>(half);
	}

	float FromHalf(std::uint16_t a_half)
	{
		const std::uint32_t sign = (a_half & 0x8000u) << 16;
		std::int32_t exponent = (a_half >> 10) & 0x1F;
		std::uint32_t mantissa = a_half & 0x3FFu;
		std::uint32_t bits;
		if (exponent == 0) {
			if (mantissa == 0) {
				bits = sign;
			} else {
				exponent = 1;
				while ((mantissa & 0x400) == 0) {
					mantissa <<= 1;
					--exponent;
				}
				mantissa &= 0x3FF;
				bits = sign | (static_cast<std::uint32_t>(exponent + 127 - 15) << 23) | (mantissa << 13);
			}
		} else if (exponent == 31) {
			bits = sign | 0x7F800000 | (mantissa << 13);
		} else {
			bits = sign | (static_cast<std::uint32_t>(exponent + 127 - 15) << 23) | (mantissa << 13);
		}
		float value;
		std::memcpy(&value, &bits, 4);
		return value;
	}

	std::uint8_t EncodeUnorm(float a_value)
	{
		return static_cast<std::uint8_t>(std::lround(std::clamp(a_value * 0.5F + 0.5F, 0.0F, 1.0F) * 255.0F));
	}

	float DecodeUnorm(std::uint8_t a_value)
	{
		return static_cast<float>(a_value) * (2.0F / 255.0F) - 1.0F;
	}

	RE::NiTransform MakeTransform(double a_yaw, double a_pitch, double a_roll, float a_scale, float a_x, float a_y, float a_z)
	{
		const double cy = std::cos(a_yaw), sy = std::sin(a_yaw);
		const double cp = std::cos(a_pitch), sp = std::sin(a_pitch);
		const double cr = std::cos(a_roll), sr = std::sin(a_roll);
		// Rz(yaw) * Ry(pitch) * Rx(roll)
		const double m[3][3]{
			{ cy * cp, cy * sp * sr - sy * cr, cy * sp * cr + sy * sr },
			{ sy * cp, sy * sp * sr + cy * cr, sy * sp * cr - cy * sr },
			{ -sp, cp * sr, cp * cr }
		};
		RE::NiTransform transform;
		for (int row = 0; row < 3; ++row) {
			for (int col = 0; col < 3; ++col) {
				transform.rotate.entry[row].pt[col] = static_cast<float>(m[row][col]);
			}
		}
		transform.translate = { a_x, a_y, a_z };
		transform.scale = a_scale;
		return transform;
	}

	std::array<double, 3> Rotate(const Math::Affine& a_matrix, double a_x, double a_y, double a_z)
	{
		return {
			a_matrix.m[0][0] * a_x + a_matrix.m[0][1] * a_y + a_matrix.m[0][2] * a_z,
			a_matrix.m[1][0] * a_x + a_matrix.m[1][1] * a_y + a_matrix.m[1][2] * a_z,
			a_matrix.m[2][0] * a_x + a_matrix.m[2][1] * a_y + a_matrix.m[2][2] * a_z
		};
	}

	struct Gpu
	{
		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11DeviceContext> context;
		ShaderLibrary shaders;

		bool Create()
		{
			const D3D_FEATURE_LEVEL levels[]{ D3D_FEATURE_LEVEL_11_0 };
			for (const auto type : { D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP }) {
				if (SUCCEEDED(D3D11CreateDevice(nullptr, type, nullptr, 0, levels, 1, D3D11_SDK_VERSION, device.GetAddressOf(), nullptr, context.GetAddressOf()))) {
					return shaders.Initialize(device.Get());
				}
			}
			return false;
		}

		ComPtr<ID3D11Buffer> MakeBuffer(UINT a_bytes, UINT a_bind, UINT a_misc, const void* a_data = nullptr, UINT a_stride = 0, D3D11_USAGE a_usage = D3D11_USAGE_DEFAULT)
		{
			D3D11_BUFFER_DESC desc{};
			desc.ByteWidth = a_bytes;
			desc.Usage = a_usage;
			desc.BindFlags = a_bind;
			desc.MiscFlags = a_misc;
			desc.StructureByteStride = a_stride;
			desc.CPUAccessFlags = a_usage == D3D11_USAGE_DYNAMIC ? D3D11_CPU_ACCESS_WRITE : 0;
			D3D11_SUBRESOURCE_DATA initial{ a_data, 0, 0 };
			ComPtr<ID3D11Buffer> buffer;
			const auto result = device->CreateBuffer(&desc, a_data ? &initial : nullptr, buffer.GetAddressOf());
			CHECK(SUCCEEDED(result));
			return buffer;
		}

		ComPtr<ID3D11ShaderResourceView> RawSRV(ID3D11Buffer* a_buffer, UINT a_bytes)
		{
			D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
			desc.Format = DXGI_FORMAT_R32_TYPELESS;
			desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
			desc.BufferEx.NumElements = a_bytes / 4;
			desc.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
			ComPtr<ID3D11ShaderResourceView> view;
			CHECK(SUCCEEDED(device->CreateShaderResourceView(a_buffer, &desc, view.GetAddressOf())));
			return view;
		}

		ComPtr<ID3D11ShaderResourceView> StructuredSRV(ID3D11Buffer* a_buffer, UINT a_count)
		{
			D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
			desc.Format = DXGI_FORMAT_UNKNOWN;
			desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
			desc.Buffer.NumElements = a_count;
			ComPtr<ID3D11ShaderResourceView> view;
			CHECK(SUCCEEDED(device->CreateShaderResourceView(a_buffer, &desc, view.GetAddressOf())));
			return view;
		}

		ComPtr<ID3D11UnorderedAccessView> RawUAV(ID3D11Buffer* a_buffer, UINT a_bytes)
		{
			D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};
			desc.Format = DXGI_FORMAT_R32_TYPELESS;
			desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
			desc.Buffer.NumElements = a_bytes / 4;
			desc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
			ComPtr<ID3D11UnorderedAccessView> view;
			CHECK(SUCCEEDED(device->CreateUnorderedAccessView(a_buffer, &desc, view.GetAddressOf())));
			return view;
		}

		template <class T = std::uint8_t>
		std::vector<T> Read(ID3D11Buffer* a_buffer)
		{
			D3D11_BUFFER_DESC desc{};
			a_buffer->GetDesc(&desc);
			desc.Usage = D3D11_USAGE_STAGING;
			desc.BindFlags = 0;
			desc.MiscFlags = 0;
			desc.StructureByteStride = 0;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			ComPtr<ID3D11Buffer> staging;
			CHECK(SUCCEEDED(device->CreateBuffer(&desc, nullptr, staging.GetAddressOf())));
			context->CopyResource(staging.Get(), a_buffer);

			std::vector<T> data(desc.ByteWidth / sizeof(T));
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
				std::memcpy(data.data(), mapped.pData, data.size() * sizeof(T));
				context->Unmap(staging.Get(), 0);
			}
			return data;
		}

		std::vector<float> ReadMip(ID3D11Texture2D* a_texture, UINT a_mip, UINT a_width, UINT a_height)
		{
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = a_width;
			desc.Height = a_height;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_R32_FLOAT;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_STAGING;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			ComPtr<ID3D11Texture2D> staging;
			CHECK(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, staging.GetAddressOf())));
			context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, a_texture, a_mip, nullptr);

			std::vector<float> data(static_cast<std::size_t>(a_width) * a_height);
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
				for (UINT y = 0; y < a_height; ++y) {
					std::memcpy(&data[static_cast<std::size_t>(y) * a_width], static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch, a_width * sizeof(float));
				}
				context->Unmap(staging.Get(), 0);
			}
			return data;
		}

		template <class T>
		ComPtr<ID3D11Buffer> Constants(const T& a_value)
		{
			auto buffer = CreateConstantBuffer(device.Get(), sizeof(T));
			UploadConstants(context.Get(), buffer.Get(), a_value);
			return buffer;
		}

		void Run(ID3D11ComputeShader* a_shader, ID3D11Buffer* a_constants, std::initializer_list<ID3D11ShaderResourceView*> a_srvs, std::initializer_list<ID3D11UnorderedAccessView*> a_uavs, UINT a_x, UINT a_y = 1)
		{
			std::vector<ID3D11ShaderResourceView*> srvs{ a_srvs };
			std::vector<ID3D11UnorderedAccessView*> uavs{ a_uavs };
			context->CSSetShader(a_shader, nullptr, 0);
			context->CSSetConstantBuffers(0, 1, &a_constants);
			context->CSSetShaderResources(0, static_cast<UINT>(srvs.size()), srvs.data());
			context->CSSetUnorderedAccessViews(0, static_cast<UINT>(uavs.size()), uavs.data(), nullptr);
			context->Dispatch(a_x, a_y, 1);

			std::vector<ID3D11ShaderResourceView*> noSrvs(srvs.size(), nullptr);
			std::vector<ID3D11UnorderedAccessView*> noUavs(uavs.size(), nullptr);
			context->CSSetShaderResources(0, static_cast<UINT>(noSrvs.size()), noSrvs.data());
			context->CSSetUnorderedAccessViews(0, static_cast<UINT>(noUavs.size()), noUavs.data(), nullptr);
		}
	};

	// Creation Engine vertex: half4 position (w = bitangent.x), half2 uv,
	// unorm8x4 normal (w = bitangent.y), unorm8x4 tangent (w = bitangent.z),
	// unorm8x4 color. 24 bytes.
	constexpr std::uint64_t kHalfDesc =
		6ull |                 // stride / 4
		(2ull << 8) |          // uv offset / 4
		(3ull << 16) |         // normal offset / 4
		(4ull << 20) |         // tangent offset / 4
		(5ull << 24) |         // color offset / 4
		(59ull << 44);         // VF_VERTEX | VF_UV | VF_NORMAL | VF_TANGENT | VF_COLORS

	InputLayouts::Layout MakeLayout(std::initializer_list<std::tuple<const char*, DXGI_FORMAT, UINT>> a_elements)
	{
		InputLayouts::Layout layout;
		for (const auto& [name, format, offset] : a_elements) {
			InputLayouts::Element element;
			element.semanticName = name;
			element.format = format;
			element.alignedByteOffset = offset;
			layout.elements.push_back(element);
		}
		return layout;
	}

	InputLayouts::Layout HalfLayout()
	{
		return MakeLayout({
			{ "POSITION", DXGI_FORMAT_R16G16B16A16_FLOAT, 0 },
			{ "TEXCOORD", DXGI_FORMAT_R16G16_FLOAT, 8 },
			{ "NORMAL", DXGI_FORMAT_R8G8B8A8_UNORM, 12 },
			{ "BINORMAL", DXGI_FORMAT_R8G8B8A8_UNORM, 16 },
			{ "COLOR", DXGI_FORMAT_R8G8B8A8_UNORM, 20 },
		});
	}

	// ---------------------------------------------------------------------
	// CPU tests

	void TestRangeAllocator()
	{
		std::puts("RangeAllocator");
		RangeAllocator allocator;
		allocator.Reset(1000);

		const auto a = allocator.Allocate(100, 1);
		const auto b = allocator.Allocate(200, 64);
		const auto c = allocator.Allocate(28, 1);
		CHECK(a && *a == 0);
		CHECK(b && *b == 128);
		CHECK(c && *c == 100);  // fits the alignment padding before b
		CHECK(allocator.Used() == 328);
		CHECK(!allocator.Allocate(700, 1));

		allocator.Free(*a, 100);
		const auto d = allocator.Allocate(100, 1);
		CHECK(d && *d == 0);

		allocator.Grow(2000);
		const auto e = allocator.Allocate(1500, 1);
		CHECK(e && *e == 328);

		allocator.Free(*b, 200);
		allocator.Free(*c, 28);
		allocator.Free(*d, 100);
		allocator.Free(*e, 1500);
		CHECK(allocator.Used() == 0);
		const auto whole = allocator.Allocate(2000, 1);
		CHECK(whole && *whole == 0);  // everything coalesced back into one block
	}

	void TestMergeTransform()
	{
		std::puts("MergeTransform");
		const auto anchorWorld = MakeTransform(0.7, -0.3, 1.1, 0.8F, 1500.0F, 1800.0F, -40.0F);
		const auto memberWorld = MakeTransform(1.5708, 0.2, 0.0, 1.5F, 1000.0F, 2000.0F, 50.0F);
		const auto anchor = Math::Affine::FromNiTransform(anchorWorld);
		const auto member = Math::Affine::FromNiTransform(memberWorld);

		const auto transform = ComputeMergeTransform(anchorWorld, memberWorld, false, true);
		CHECK(transform.has_value());
		CHECK(!transform->mirrored);

		std::mt19937 rng{ 7 };
		std::uniform_real_distribution<double> dist{ -300.0, 300.0 };
		for (int i = 0; i < 32; ++i) {
			const double p[3]{ dist(rng), dist(rng), dist(rng) };
			double local[3]{};
			double viaAnchor[3]{};
			double direct[3]{};
			transform->position.Apply(p, local);
			anchor.Apply(local, viaAnchor);
			member.Apply(p, direct);
			for (int k = 0; k < 3; ++k) {
				CHECK_NEAR(viaAnchor[k], direct[k], 1e-6);
			}
		}

		// Directions: anchor rotation applied to the merged direction equals the member's rotation.
		const auto anchorRotation = *Math::Affine::FromNiTransform(anchorWorld).NormalMatrix();
		const auto memberRotation = *Math::Affine::FromNiTransform(memberWorld).NormalMatrix();
		const auto merged = Rotate(transform->direction, 0.0, 0.6, 0.8);
		const auto world1 = Rotate(anchorRotation, merged[0], merged[1], merged[2]);
		const auto world2 = Rotate(memberRotation, 0.0, 0.6, 0.8);
		const auto len1 = std::sqrt(world1[0] * world1[0] + world1[1] * world1[1] + world1[2] * world1[2]);
		const auto len2 = std::sqrt(world2[0] * world2[0] + world2[1] * world2[1] + world2[2] * world2[2]);
		for (int k = 0; k < 3; ++k) {
			CHECK_NEAR(world1[k] / len1, world2[k] / len2, 1e-6);
		}

		// Same orientation: directions must be passed through exactly.
		auto sameRotation = memberWorld;
		sameRotation.rotate = anchorWorld.rotate;
		sameRotation.scale = 2.0F;
		const auto identity = ComputeMergeTransform(anchorWorld, sameRotation, false, true);
		CHECK(identity.has_value());
		for (int row = 0; row < 3; ++row) {
			for (int col = 0; col < 3; ++col) {
				CHECK(identity->direction.m[row][col] == (row == col ? 1.0 : 0.0));
			}
		}

		// Mirrored members flip winding.
		auto mirrored = memberWorld;
		for (auto& value : mirrored.rotate.entry[0].pt) {
			value = -value;
		}
		const auto flipped = ComputeMergeTransform(anchorWorld, mirrored, false, true);
		CHECK(flipped && flipped->mirrored);

		// Zero scale is rejected.
		auto degenerate = memberWorld;
		degenerate.scale = 0.0F;
		CHECK(!ComputeMergeTransform(anchorWorld, degenerate, false, true));
	}

	void TestVertexPlan()
	{
		std::puts("VertexPlan");
		const auto layout = HalfLayout();
		std::string reason;

		const auto plan = BuildVertexPlan(kHalfDesc, 24, &layout, true, &reason);
		CHECK(plan.has_value());
		if (plan) {
			CHECK(plan->srcStride == 24);
			CHECK(plan->dstStride == 32);
			CHECK(plan->positionBytes == 8);
			CHECK(plan->shift == 8);
			CHECK(plan->elements.size() == 5);
			CHECK(plan->bitangentInW);
			CHECK(plan->canRotateDirections);
			CHECK(plan->elements[0].kind == ElementKind::kPosition && plan->elements[0].srcFormat == ElementFormat::kF16x4 && plan->elements[0].dstFormat == ElementFormat::kF32x4);
			CHECK(plan->elements[1].kind == ElementKind::kCopy && plan->elements[1].srcOffset == 8 && plan->elements[1].dstOffset == 16 && plan->elements[1].dwords == 1);
			CHECK(plan->elements[2].kind == ElementKind::kDirection && plan->elements[2].srcFormat == ElementFormat::kUN8x4 && plan->elements[2].dstOffset == 20);
			CHECK(plan->elements[3].kind == ElementKind::kDirection && plan->elements[3].dstOffset == 24);
			CHECK(plan->elements[4].kind == ElementKind::kCopy && plan->elements[4].dstOffset == 28);
			CHECK(plan->ShaderFlags() == (1u | (0u << 4) | (2u << 8) | (3u << 12)));
		}

		// Full precision: positions already 32-bit, nothing moves.
		constexpr std::uint64_t fullDesc = 8ull | (4ull << 8) | (5ull << 16) | (6ull << 20) | (7ull << 24) | ((59ull | 0x400ull) << 44);
		const auto fullLayout = MakeLayout({
			{ "POSITION", DXGI_FORMAT_R32G32B32A32_FLOAT, 0 },
			{ "TEXCOORD", DXGI_FORMAT_R16G16_FLOAT, 16 },
			{ "NORMAL", DXGI_FORMAT_R8G8B8A8_UNORM, 20 },
			{ "BINORMAL", DXGI_FORMAT_R8G8B8A8_UNORM, 24 },
			{ "COLOR", DXGI_FORMAT_R8G8B8A8_UNORM, 28 },
		});
		const auto full = BuildVertexPlan(fullDesc, 32, &fullLayout, true, &reason);
		CHECK(full && full->shift == 0 && full->dstStride == 32 && full->positionBytes == 16);

		// Rejections.
		CHECK(!BuildVertexPlan(kHalfDesc | (64ull << 44), 24, &layout, true, &reason));  // skinned
		CHECK(!BuildVertexPlan(kHalfDesc, 28, &layout, true, &reason));                  // stride mismatch
		auto inside = layout;
		inside.elements.push_back({ "TEXCOORD", 1, DXGI_FORMAT_R16_FLOAT, 0, 10, D3D11_INPUT_PER_VERTEX_DATA, 0 });
		CHECK(!BuildVertexPlan(kHalfDesc, 24, &inside, true, &reason));                  // element inside an attribute

		// Signed normals are recognised; unconfirmed tangents disable rotation.
		const auto signedLayout = MakeLayout({
			{ "POSITION", DXGI_FORMAT_R16G16B16A16_FLOAT, 0 },
			{ "TEXCOORD", DXGI_FORMAT_R16G16_FLOAT, 8 },
			{ "NORMAL", DXGI_FORMAT_R8G8B8A8_SNORM, 12 },
		});
		const auto signedPlan = BuildVertexPlan(kHalfDesc, 24, &signedLayout, true, &reason);
		CHECK(signedPlan && signedPlan->elements[2].srcFormat == ElementFormat::kSN8x4);
		CHECK(signedPlan && !signedPlan->canRotateDirections && !signedPlan->bitangentInW);
	}

	// ---------------------------------------------------------------------
	// GPU tests

	struct SourceVertex
	{
		float position[3];
		float bitangent[3];
		float normal[3];
		float tangent[3];
		std::uint32_t uv;
		std::uint32_t color;
	};

	std::vector<std::uint8_t> EncodeVertices(const std::vector<SourceVertex>& a_vertices)
	{
		std::vector<std::uint8_t> bytes(a_vertices.size() * 24);
		for (std::size_t i = 0; i < a_vertices.size(); ++i) {
			const auto& v = a_vertices[i];
			auto* const out = bytes.data() + i * 24;
			const std::uint16_t position[4]{ ToHalf(v.position[0]), ToHalf(v.position[1]), ToHalf(v.position[2]), ToHalf(v.bitangent[0]) };
			std::memcpy(out, position, 8);
			std::memcpy(out + 8, &v.uv, 4);
			const std::uint8_t normal[4]{ EncodeUnorm(v.normal[0]), EncodeUnorm(v.normal[1]), EncodeUnorm(v.normal[2]), EncodeUnorm(v.bitangent[1]) };
			const std::uint8_t tangent[4]{ EncodeUnorm(v.tangent[0]), EncodeUnorm(v.tangent[1]), EncodeUnorm(v.tangent[2]), EncodeUnorm(v.bitangent[2]) };
			std::memcpy(out + 12, normal, 4);
			std::memcpy(out + 16, tangent, 4);
			std::memcpy(out + 20, &v.color, 4);
		}
		return bytes;
	}

	std::vector<SourceVertex> RandomVertices(std::size_t a_count, std::uint32_t a_seed)
	{
		std::mt19937 rng{ a_seed };
		std::uniform_real_distribution<float> position{ -200.0F, 200.0F };
		std::uniform_real_distribution<float> angle{ 0.0F, 6.2831853F };
		std::uniform_int_distribution<std::uint32_t> bits;

		std::vector<SourceVertex> vertices(a_count);
		for (auto& v : vertices) {
			for (auto& p : v.position) {
				p = position(rng);
			}
			// orthonormal frame from two angles
			const float a = angle(rng);
			const float b = angle(rng) * 0.5F;
			const float n[3]{ std::cos(a) * std::sin(b), std::sin(a) * std::sin(b), std::cos(b) };
			float t[3]{ -std::sin(a), std::cos(a), 0.0F };
			const float bt[3]{ n[1] * t[2] - n[2] * t[1], n[2] * t[0] - n[0] * t[2], n[0] * t[1] - n[1] * t[0] };
			std::memcpy(v.normal, n, sizeof(n));
			std::memcpy(v.tangent, t, sizeof(t));
			std::memcpy(v.bitangent, bt, sizeof(bt));
			v.uv = bits(rng);
			v.color = bits(rng);
		}
		return vertices;
	}

	void TestMergeVertices(Gpu& a_gpu)
	{
		std::puts("MergeVertices.hlsl");
		auto shader = a_gpu.shaders.CompileCompute("MergeVertices.hlsl", "CSMain");
		CHECK(shader != nullptr);
		if (!shader) {
			return;
		}

		const auto layout = HalfLayout();
		const auto plan = *BuildVertexPlan(kHalfDesc, 24, &layout, true, nullptr);

		for (const bool rotated : { true, false }) {
			const auto anchorWorld = MakeTransform(0.7, -0.3, 1.1, 0.8F, 1500.0F, 1800.0F, -40.0F);
			auto memberWorld = MakeTransform(1.5708, 0.2, 0.0, 1.5F, 1000.0F, 2000.0F, 50.0F);
			if (!rotated) {
				memberWorld.rotate = anchorWorld.rotate;
			}
			const auto transform = *ComputeMergeTransform(anchorWorld, memberWorld, false, true);

			constexpr std::uint32_t count = 97;  // not a multiple of the group size
			constexpr std::uint32_t srcBase = 36;
			constexpr std::uint32_t dstBase = 256;
			const auto vertices = RandomVertices(count, rotated ? 11u : 12u);
			const auto encoded = EncodeVertices(vertices);

			std::vector<std::uint8_t> source(srcBase + encoded.size() + 64, 0xCD);
			std::memcpy(source.data() + srcBase, encoded.data(), encoded.size());
			auto src = a_gpu.MakeBuffer(static_cast<UINT>(source.size()), D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, source.data());
			const auto dstBytes = dstBase + count * plan.dstStride + 64;
			auto dst = a_gpu.MakeBuffer(dstBytes, D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_UNORDERED_ACCESS, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS);
			auto srcView = a_gpu.RawSRV(src.Get(), static_cast<UINT>(source.size()));
			auto dstView = a_gpu.RawUAV(dst.Get(), dstBytes);

			const auto constants = a_gpu.Constants(MakeMergeConstants(plan, transform, srcBase, dstBase, count));
			a_gpu.Run(shader.Get(), constants.Get(), { srcView.Get() }, { dstView.Get() }, (count + 63) / 64);
			const auto output = a_gpu.Read(dst.Get());

			const auto anchor = Math::Affine::FromNiTransform(anchorWorld);
			const auto member = Math::Affine::FromNiTransform(memberWorld);
			double worstPosition = 0.0;
			double worstDirection = 0.0;
			for (std::uint32_t i = 0; i < count; ++i) {
				const auto* const in = encoded.data() + i * 24;
				const auto* const out = output.data() + dstBase + i * plan.dstStride;

				std::uint16_t halves[4];
				std::memcpy(halves, in, 8);
				const double p[3]{ FromHalf(halves[0]), FromHalf(halves[1]), FromHalf(halves[2]) };
				float merged[4];
				std::memcpy(merged, out, 16);

				// The anchor's world transform applied to the merged vertex must
				// land where the member's transform puts the original vertex.
				const double q[3]{ merged[0], merged[1], merged[2] };
				double expectedWorld[3]{};
				double actualWorld[3]{};
				member.Apply(p, expectedWorld);
				anchor.Apply(q, actualWorld);
				for (int k = 0; k < 3; ++k) {
					worstPosition = std::max(worstPosition, std::abs(expectedWorld[k] - actualWorld[k]));
				}

				// uv and color are copied verbatim
				CHECK(std::memcmp(out + 16, in + 8, 4) == 0);
				CHECK(std::memcmp(out + 28, in + 20, 4) == 0);

				const std::uint8_t* const normalIn = in + 12;
				const std::uint8_t* const tangentIn = in + 16;
				const std::uint8_t* const normalOut = out + 20;
				const std::uint8_t* const tangentOut = out + 24;
				if (!rotated) {
					// Same orientation: bytes and the bitangent half must survive bit-exactly.
					CHECK(std::memcmp(normalOut, normalIn, 4) == 0);
					CHECK(std::memcmp(tangentOut, tangentIn, 4) == 0);
					CHECK(merged[3] == FromHalf(halves[3]));
					continue;
				}

				const double n[3]{ DecodeUnorm(normalIn[0]), DecodeUnorm(normalIn[1]), DecodeUnorm(normalIn[2]) };
				const double t[3]{ DecodeUnorm(tangentIn[0]), DecodeUnorm(tangentIn[1]), DecodeUnorm(tangentIn[2]) };
				const double b[3]{ FromHalf(halves[3]), DecodeUnorm(normalIn[3]), DecodeUnorm(tangentIn[3]) };
				const auto en = Rotate(transform.direction, n[0], n[1], n[2]);
				const auto et = Rotate(transform.direction, t[0], t[1], t[2]);
				const auto eb = Rotate(transform.direction, b[0], b[1], b[2]);
				for (int k = 0; k < 3; ++k) {
					worstDirection = std::max(worstDirection, std::abs(DecodeUnorm(normalOut[k]) - en[static_cast<std::size_t>(k)]));
					worstDirection = std::max(worstDirection, std::abs(DecodeUnorm(tangentOut[k]) - et[static_cast<std::size_t>(k)]));
				}
				worstDirection = std::max(worstDirection, std::abs(merged[3] - eb[0]));
				worstDirection = std::max(worstDirection, std::abs(DecodeUnorm(normalOut[3]) - eb[1]));
				worstDirection = std::max(worstDirection, std::abs(DecodeUnorm(tangentOut[3]) - eb[2]));
			}
			std::printf("  %s member: worst position error %.6f units, worst direction error %.4f\n", rotated ? "rotated" : "aligned", worstPosition, worstDirection);
			CHECK(worstPosition < 0.01);
			CHECK(worstDirection < 0.01);

			// Bytes before the destination range are untouched.
			bool untouched = true;
			for (std::uint32_t i = 0; i < dstBase; ++i) {
				untouched = untouched && output[i] == 0;
			}
			CHECK(untouched);
		}
	}

	void TestMergeIndices(Gpu& a_gpu)
	{
		std::puts("MergeIndices.hlsl");
		auto shader = a_gpu.shaders.CompileCompute("MergeIndices.hlsl", "CSMain");
		CHECK(shader != nullptr);
		if (!shader) {
			return;
		}

		constexpr std::uint32_t triangles = 40;
		constexpr std::uint32_t count = triangles * 3;
		constexpr std::uint32_t vertexCount = 97;
		constexpr std::uint32_t dstIndex = 1000;
		constexpr std::uint32_t vertexBase = 500;

		std::mt19937 rng{ 3 };
		std::uniform_int_distribution<std::uint32_t> index{ 0, vertexCount - 1 };
		std::vector<std::uint32_t> indices(count);
		for (auto& value : indices) {
			value = index(rng);
		}
		indices[7] = 150;  // out of range: must be reported and clamped to 0

		auto errors = a_gpu.MakeBuffer(256 * 4, D3D11_BIND_UNORDERED_ACCESS, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, std::vector<std::uint32_t>(256, 0).data());
		auto errorsView = a_gpu.RawUAV(errors.Get(), 256 * 4);
		const auto dstBytes = (dstIndex + count + 16) * 4;
		auto dst = a_gpu.MakeBuffer(dstBytes, D3D11_BIND_INDEX_BUFFER | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS);
		auto dstView = a_gpu.RawUAV(dst.Get(), dstBytes);

		struct Case
		{
			bool index32;
			bool flip;
			std::uint32_t srcOffset;
			std::uint32_t errorSlot;
		};
		for (const auto& testCase : { Case{ false, false, 6, 3 }, Case{ false, true, 6, 4 }, Case{ true, false, 8, 5 } }) {
			std::vector<std::uint8_t> source(testCase.srcOffset + count * 4 + 16, 0xEE);
			for (std::uint32_t i = 0; i < count; ++i) {
				if (testCase.index32) {
					std::memcpy(source.data() + testCase.srcOffset + i * 4, &indices[i], 4);
				} else {
					const auto value = static_cast<std::uint16_t>(indices[i]);
					std::memcpy(source.data() + testCase.srcOffset + i * 2, &value, 2);
				}
			}
			auto src = a_gpu.MakeBuffer(static_cast<UINT>(source.size()), D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, source.data());
			auto srcView = a_gpu.RawSRV(src.Get(), static_cast<UINT>(source.size()));

			ShaderConstants::Index params{};
			params.srcByteOffset = testCase.srcOffset;
			params.indexCount = count;
			params.dstIndex = dstIndex;
			params.vertexBase = vertexBase;
			params.vertexCount = vertexCount;
			params.flipWinding = testCase.flip ? 1 : 0;
			params.index32 = testCase.index32 ? 1 : 0;
			params.errorSlot = testCase.errorSlot;
			const auto constants = a_gpu.Constants(params);
			a_gpu.Run(shader.Get(), constants.Get(), { srcView.Get() }, { dstView.Get(), errorsView.Get() }, (count + 63) / 64);

			const auto output = a_gpu.Read<std::uint32_t>(dst.Get());
			bool matches = true;
			for (std::uint32_t i = 0; i < count; ++i) {
				std::uint32_t source_i = i;
				if (testCase.flip) {
					const auto corner = i % 3;
					source_i = i - corner + (corner == 1 ? 2 : (corner == 2 ? 1 : 0));
				}
				const auto value = indices[source_i] >= vertexCount ? 0 : indices[source_i];
				matches = matches && output[dstIndex + i] == value + vertexBase;
			}
			CHECK(matches);
			const auto errorCounts = a_gpu.Read<std::uint32_t>(errors.Get());
			CHECK(errorCounts[testCase.errorSlot] == 1);
		}
	}

	struct TestMember
	{
		GpuMember gpu;
		std::vector<std::uint32_t> indices;
	};

	std::vector<TestMember> MakeMembers(std::span<const std::array<float, 4>> a_spheres, std::span<const std::uint32_t> a_counts)
	{
		std::vector<TestMember> members(a_counts.size());
		std::uint32_t start = 5;
		for (std::size_t m = 0; m < members.size(); ++m) {
			auto& member = members[m];
			std::memcpy(member.gpu.sphere, a_spheres[m].data(), sizeof(member.gpu.sphere));
			member.gpu.indexStart = start;
			member.gpu.indexCount = a_counts[m];
			member.gpu.bucket = static_cast<std::uint32_t>(m);
			for (std::uint32_t i = 0; i < a_counts[m]; ++i) {
				member.indices.push_back(static_cast<std::uint32_t>(m) * 100000 + i);
			}
			start += a_counts[m] + 3;
		}
		return members;
	}

	struct CullResources
	{
		ComPtr<ID3D11Buffer> members;
		ComPtr<ID3D11ShaderResourceView> membersView;
		ComPtr<ID3D11Buffer> arena;
		ComPtr<ID3D11ShaderResourceView> arenaView;
		ComPtr<ID3D11Buffer> work;
		ComPtr<ID3D11ShaderResourceView> workView;
		ComPtr<ID3D11Buffer> args;
		ComPtr<ID3D11UnorderedAccessView> argsView;
		ComPtr<ID3D11Buffer> ring;
		ComPtr<ID3D11UnorderedAccessView> ringView;
	};

	CullResources MakeCullResources(Gpu& a_gpu, const std::vector<TestMember>& a_members, const std::vector<std::uint32_t>& a_work, const std::vector<std::uint32_t>& a_argsInit)
	{
		CullResources r;
		std::vector<GpuMember> table;
		std::vector<std::uint32_t> arena(4096, 0xFFFFFFFF);
		for (const auto& member : a_members) {
			table.push_back(member.gpu);
			std::copy(member.indices.begin(), member.indices.end(), arena.begin() + member.gpu.indexStart);
		}
		r.members = a_gpu.MakeBuffer(static_cast<UINT>(table.size() * sizeof(GpuMember)), D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, table.data(), sizeof(GpuMember));
		r.membersView = a_gpu.StructuredSRV(r.members.Get(), static_cast<UINT>(table.size()));
		r.arena = a_gpu.MakeBuffer(static_cast<UINT>(arena.size() * 4), D3D11_BIND_INDEX_BUFFER | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, arena.data());
		r.arenaView = a_gpu.RawSRV(r.arena.Get(), static_cast<UINT>(arena.size() * 4));

		// Work items go through the same dynamic structured buffer the plugin uses.
		r.work = a_gpu.MakeBuffer(static_cast<UINT>(a_work.size() * 4), D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, nullptr, 8, D3D11_USAGE_DYNAMIC);
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (SUCCEEDED(a_gpu.context->Map(r.work.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			std::memcpy(mapped.pData, a_work.data(), a_work.size() * 4);
			a_gpu.context->Unmap(r.work.Get(), 0);
		}
		r.workView = a_gpu.StructuredSRV(r.work.Get(), static_cast<UINT>(a_work.size() / 2));

		std::vector<std::uint32_t> args(64 * 5, 0xDEAD);
		std::copy(a_argsInit.begin(), a_argsInit.end(), args.begin());
		r.args = a_gpu.MakeBuffer(static_cast<UINT>(args.size() * 4), D3D11_BIND_UNORDERED_ACCESS, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, args.data());
		r.argsView = a_gpu.RawUAV(r.args.Get(), static_cast<UINT>(args.size() * 4));
		r.ring = a_gpu.MakeBuffer(4096 * 4, D3D11_BIND_INDEX_BUFFER | D3D11_BIND_UNORDERED_ACCESS, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, std::vector<std::uint32_t>(4096, 0xFFFFFFFF).data());
		r.ringView = a_gpu.RawUAV(r.ring.Get(), 4096 * 4);
		return r;
	}

	void TestCullCompaction(Gpu& a_gpu)
	{
		std::puts("Cull.hlsl CSCompact");
		auto shader = a_gpu.shaders.CompileCompute("Cull.hlsl", "CSCompact");
		CHECK(shader != nullptr);
		if (!shader) {
			return;
		}

		const std::array<float, 4> sphere{ 0.0F, 0.0F, 0.0F, 1.0F };
		const std::array<std::array<float, 4>, 6> spheres{ sphere, sphere, sphere, sphere, sphere, sphere };
		const std::uint32_t counts[]{ 3, 64, 65, 130, 1, 7 };
		const auto members = MakeMembers(spheres, counts);

		// slot 0: members 0, 2, 3 | slot 1: members 1, 5 | member 4 not visible in this view
		const std::vector<std::uint32_t> work{ 0, 0, 1, 1, 2, 0, 5, 1, 3, 0 };
		const std::uint32_t ringStart0 = 10;
		const std::uint32_t slot0 = counts[0] + counts[2] + counts[3];
		const std::uint32_t ringStart1 = ringStart0 + slot0;
		const std::vector<std::uint32_t> argsInit{ 0, 1, ringStart0, 0, 0, 0, 1, ringStart1, 0, 0 };
		auto r = MakeCullResources(a_gpu, members, work, argsInit);

		ShaderConstants::Cull params{};
		params.workCount = 5;
		params.groupsX = 5;
		const auto constants = a_gpu.Constants(params);
		a_gpu.Run(shader.Get(), constants.Get(), { r.membersView.Get(), r.workView.Get(), r.arenaView.Get(), nullptr }, { r.argsView.Get(), r.ringView.Get() }, 5);

		const auto args = a_gpu.Read<std::uint32_t>(r.args.Get());
		const auto ring = a_gpu.Read<std::uint32_t>(r.ring.Get());
		CHECK(args[0] == slot0 && args[1] == 1 && args[2] == ringStart0 && args[3] == 0 && args[4] == 0);
		CHECK(args[5] == counts[1] + counts[5] && args[7] == ringStart1);

		// Each slot holds its members' index runs back to back, in any order.
		const auto verifySlot = [&](std::uint32_t a_start, std::uint32_t a_total, std::initializer_list<std::size_t> a_members) {
			std::uint32_t covered = 0;
			for (const auto m : a_members) {
				const auto& expected = members[m].indices;
				const auto it = std::search(ring.begin() + a_start, ring.begin() + a_start + a_total, expected.begin(), expected.end());
				CHECK(it != ring.begin() + a_start + a_total);
				covered += static_cast<std::uint32_t>(expected.size());
			}
			CHECK(covered == a_total);
		};
		verifySlot(ringStart0, slot0, { 0, 2, 3 });
		verifySlot(ringStart1, counts[1] + counts[5], { 1, 5 });
		CHECK(ring[ringStart0 - 1] == 0xFFFFFFFF);
		CHECK(ring[ringStart1 + counts[1] + counts[5]] == 0xFFFFFFFF);
	}

	// world (x, y, z) -> clip with the camera at the origin looking down +y:
	// x' = x, y' = z, z' = a*y + b, w = y; depth 0 at y = 1, 1 at y = 1000.
	void PerspectiveRowMajor(float (&a_matrix)[4][4])
	{
		constexpr float n = 1.0F;
		constexpr float f = 1000.0F;
		const float a = f / (f - n);
		const float b = -n * f / (f - n);
		const float m[4][4]{
			{ 1, 0, 0, 0 },
			{ 0, 0, 1, 0 },
			{ 0, a, 0, b },
			{ 0, 1, 0, 0 }
		};
		std::memcpy(a_matrix, m, sizeof(m));
	}

	void TestMultiDrawAndOcclusion(Gpu& a_gpu)
	{
		std::puts("Cull.hlsl CSMultiDraw + Hi-Z occlusion");
		auto shader = a_gpu.shaders.CompileCompute("Cull.hlsl", "CSMultiDraw");
		HiZ hiz;
		CHECK(shader != nullptr);
		CHECK(hiz.Initialize(a_gpu.device.Get(), a_gpu.shaders));
		if (!shader) {
			return;
		}

		// Depth buffer cleared to 0.5: the occluder plane sits at y = 1.998.
		constexpr UINT size = 64;
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = size;
		desc.Height = size;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R32_TYPELESS;
		desc.SampleDesc.Count = 1;
		desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
		ComPtr<ID3D11Texture2D> depth;
		CHECK(SUCCEEDED(a_gpu.device->CreateTexture2D(&desc, nullptr, depth.GetAddressOf())));
		D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
		dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
		dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
		ComPtr<ID3D11DepthStencilView> dsv;
		CHECK(SUCCEEDED(a_gpu.device->CreateDepthStencilView(depth.Get(), &dsvDesc, dsv.GetAddressOf())));
		a_gpu.context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 0.5F, 0);

		float viewProj[4][4];
		PerspectiveRowMajor(viewProj);
		const D3D11_VIEWPORT viewport{ 0.0F, 0.0F, static_cast<float>(size), static_cast<float>(size), 0.0F, 1.0F };
		CHECK(hiz.Build(a_gpu.context.Get(), dsv.Get(), viewport, viewProj, false, 1));
		CHECK(hiz.UsableFor(2));
		CHECK(hiz.Mips() == 7);

		struct Probe
		{
			std::array<float, 4> sphere;
			bool visible;
			const char* what;
		};
		const Probe probes[]{
			{ { 0.0F, 1.5F, 0.0F, 0.1F }, true, "in front of the occluder" },
			{ { 0.0F, 10.0F, 0.0F, 0.5F }, false, "behind the occluder" },
			{ { 5000.0F, 10.0F, 0.0F, 0.5F }, true, "outside the previous view" },
			{ { 0.0F, 0.5F, 0.0F, 1.0F }, true, "crossing the camera plane" },
			{ { 0.0F, 1.9F, 0.0F, 0.05F }, true, "just in front" },
			{ { 0.3F, 2.5F, 0.2F, 0.1F }, false, "just behind" },
			{ { 0.0F, 3.0F, 0.0F, 1.5F }, true, "large, straddling the occluder" },
		};

		std::vector<std::array<float, 4>> spheres;
		std::vector<std::uint32_t> counts;
		std::vector<std::uint32_t> work;
		for (std::uint32_t i = 0; i < std::size(probes); ++i) {
			spheres.push_back(probes[i].sphere);
			counts.push_back(9 + i);
			work.push_back(i);
			work.push_back(0);
		}
		const auto members = MakeMembers(spheres, counts);
		auto r = MakeCullResources(a_gpu, members, work, {});

		constexpr std::uint32_t argsBase = 7;
		ShaderConstants::Cull params{};
		std::memcpy(params.viewProj, hiz.ViewProj(), sizeof(params.viewProj));
		params.hizSize[0] = static_cast<float>(hiz.Width());
		params.hizSize[1] = static_cast<float>(hiz.Height());
		params.hizSize[2] = 1.0F / static_cast<float>(hiz.Width());
		params.hizSize[3] = 1.0F / static_cast<float>(hiz.Height());
		params.workCount = static_cast<std::uint32_t>(std::size(probes));
		params.groupsX = 1;
		params.occlusion = 1;
		params.hizMips = hiz.Mips();
		params.depthBias = 0.0005F;
		params.argsBase = argsBase;
		const auto constants = a_gpu.Constants(params);
		a_gpu.Run(shader.Get(), constants.Get(), { r.membersView.Get(), r.workView.Get(), r.arenaView.Get(), hiz.SRV() }, { r.argsView.Get(), nullptr }, 1);

		const auto args = a_gpu.Read<std::uint32_t>(r.args.Get());
		for (std::uint32_t i = 0; i < std::size(probes); ++i) {
			const auto* const record = &args[(argsBase + i) * 5];
			const bool visible = record[0] != 0;
			if (visible != probes[i].visible) {
				std::printf("  probe %u (%s): expected %s\n", i, probes[i].what, probes[i].visible ? "visible" : "culled");
			}
			CHECK(visible == probes[i].visible);
			CHECK(record[0] == (probes[i].visible ? counts[i] : 0) && record[1] == 1 && record[2] == members[i].gpu.indexStart && record[3] == 0 && record[4] == 0);
		}
		CHECK(args[(argsBase - 1) * 5] == 0xDEAD);  // records before argsBase untouched
	}

	void TestHiZPyramid(Gpu& a_gpu)
	{
		std::puts("HiZ.hlsl pyramid (odd size, gradient depth)");
		HiZ hiz;
		CHECK(hiz.Initialize(a_gpu.device.Get(), a_gpu.shaders));

		constexpr UINT width = 37;
		constexpr UINT height = 23;
		std::vector<float> depth(width * height);
		for (UINT y = 0; y < height; ++y) {
			for (UINT x = 0; x < width; ++x) {
				depth[y * width + x] = 0.2F + 0.6F * static_cast<float>((x * 7 + y * 13) % 17) / 16.0F;
			}
		}

		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = width;
		desc.Height = height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R32_TYPELESS;
		desc.SampleDesc.Count = 1;
		desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
		const D3D11_SUBRESOURCE_DATA initial{ depth.data(), width * 4, 0 };
		ComPtr<ID3D11Texture2D> texture;
		if (FAILED(a_gpu.device->CreateTexture2D(&desc, &initial, texture.GetAddressOf()))) {
			std::puts("  skipped: device does not accept initial data for depth textures");
			return;
		}
		D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
		dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
		dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
		ComPtr<ID3D11DepthStencilView> dsv;
		CHECK(SUCCEEDED(a_gpu.device->CreateDepthStencilView(texture.Get(), &dsvDesc, dsv.GetAddressOf())));

		float viewProj[4][4];
		PerspectiveRowMajor(viewProj);
		const D3D11_VIEWPORT viewport{ 0.0F, 0.0F, static_cast<float>(width), static_cast<float>(height), 0.0F, 1.0F };
		CHECK(hiz.Build(a_gpu.context.Get(), dsv.Get(), viewport, viewProj, false, 1));
		CHECK(hiz.Mips() == 6);

		ComPtr<ID3D11Resource> resource;
		hiz.SRV()->GetResource(resource.GetAddressOf());
		ComPtr<ID3D11Texture2D> pyramid;
		resource.As(&pyramid);

		// CPU reference: the same conservative reduction.
		std::vector<std::vector<float>> reference{ depth };
		std::vector<std::pair<UINT, UINT>> sizes{ { width, height } };
		for (UINT mip = 1; mip < hiz.Mips(); ++mip) {
			const auto [sw, sh] = sizes.back();
			const UINT dw = std::max(sw / 2, 1u);
			const UINT dh = std::max(sh / 2, 1u);
			std::vector<float> level(static_cast<std::size_t>(dw) * dh);
			const auto& src = reference.back();
			for (UINT y = 0; y < dh; ++y) {
				for (UINT x = 0; x < dw; ++x) {
					const UINT x1 = (x == dw - 1 && (sw & 1)) ? std::min(2 * x + 2, sw - 1) : std::min(2 * x + 1, sw - 1);
					const UINT y1 = (y == dh - 1 && (sh & 1)) ? std::min(2 * y + 2, sh - 1) : std::min(2 * y + 1, sh - 1);
					float value = 0.0F;
					for (UINT yy = std::min(2 * y, sh - 1); yy <= y1; ++yy) {
						for (UINT xx = std::min(2 * x, sw - 1); xx <= x1; ++xx) {
							value = std::max(value, src[static_cast<std::size_t>(yy) * sw + xx]);
						}
					}
					level[static_cast<std::size_t>(y) * dw + x] = value;
				}
			}
			reference.push_back(std::move(level));
			sizes.emplace_back(dw, dh);
		}

		for (UINT mip = 0; mip < hiz.Mips(); ++mip) {
			const auto [w, h] = sizes[mip];
			const auto gpu = a_gpu.ReadMip(pyramid.Get(), mip, w, h);
			CHECK(gpu == reference[mip]);

			// Conservative: the texel x >> mip always covers mip-0 texel x.
			bool conservative = true;
			for (UINT y = 0; y < height; ++y) {
				for (UINT x = 0; x < width; ++x) {
					const UINT tx = std::min(x >> mip, w - 1);
					const UINT ty = std::min(y >> mip, h - 1);
					conservative = conservative && gpu[static_cast<std::size_t>(ty) * w + tx] >= depth[static_cast<std::size_t>(y) * width + x];
				}
			}
			CHECK(conservative);
		}
	}
}

int main(int a_argc, char** a_argv)
{
	for (int i = 1; i + 1 < a_argc; ++i) {
		if (std::string_view{ a_argv[i] } == "--shaders") {
			Settings::Get().shaderDirectory = a_argv[i + 1];
		}
	}

	TestRangeAllocator();
	TestMergeTransform();
	TestVertexPlan();

	Gpu gpu;
	if (!gpu.Create()) {
		std::puts("no Direct3D 11 device or d3dcompiler_47.dll; GPU tests skipped");
		++g_failures;
	} else {
		TestMergeVertices(gpu);
		TestMergeIndices(gpu);
		TestCullCompaction(gpu);
		TestMultiDrawAndOcclusion(gpu);
		TestHiZPyramid(gpu);
	}

	std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
