#pragma once

// Engine object layouts that CommonLibF4RD only forward-declares.
//
// Offsets come from two independent public sources that agree with each other:
//   - F4SE (ianpatt/f4se, f4se/BSGeometry.h, f4se/NiObjects.h)
//   - libxse/commonlibf4 (RE/B/BSGeometry.h, RE/B/BSTriShape.h, RE/N/NiCamera.h)
// They describe the NG/AE layout family. Everything the batching path depends
// on is additionally validated at runtime (see Calibration), so a layout
// mismatch disables batching instead of corrupting the frame.

namespace GWP::Engine
{
	namespace Offsets
	{
		// BSGeometry : NiAVObject (size 0x160)
		inline constexpr std::size_t kGeometryModelBound = 0x120;
		inline constexpr std::size_t kGeometryAlphaProperty = 0x130;   // NiPointer<NiProperty> effectState (NiAlphaProperty)
		inline constexpr std::size_t kGeometryShaderProperty = 0x138;  // NiPointer<NiProperty> shaderProperty
		inline constexpr std::size_t kGeometrySkinInstance = 0x140;
		inline constexpr std::size_t kGeometryRendererData = 0x148;    // BSGraphics::TriShape*
		inline constexpr std::size_t kGeometryVertexDesc = 0x150;
		inline constexpr std::size_t kGeometryType = 0x158;

		// BSTriShape : BSGeometry (size 0x170)
		inline constexpr std::size_t kTriShapeNumTriangles = 0x160;
		inline constexpr std::size_t kTriShapeNumVertices = 0x164;

		// NiAlphaProperty : NiProperty
		inline constexpr std::size_t kAlphaFlags = 0x28;
		inline constexpr std::size_t kAlphaThreshold = 0x2A;

		// NiCamera : NiAVObject (size 0x1A0)
		inline constexpr std::size_t kCameraWorldToCam = 0x120;  // float[4][4], row-major, absolute world space
		inline constexpr std::size_t kCameraFrustumNear = 0x170;
		inline constexpr std::size_t kCameraFrustumFar = 0x174;
		inline constexpr std::size_t kCameraFrustumOrtho = 0x178;
	}

	// BSGraphics::Vertex flags packed into the high bits of a vertex descriptor.
	namespace VertexFlags
	{
		inline constexpr std::uint16_t kVertex = 1 << 0;
		inline constexpr std::uint16_t kUV = 1 << 1;
		inline constexpr std::uint16_t kUV2 = 1 << 2;
		inline constexpr std::uint16_t kNormal = 1 << 3;
		inline constexpr std::uint16_t kTangent = 1 << 4;
		inline constexpr std::uint16_t kColors = 1 << 5;
		inline constexpr std::uint16_t kSkinned = 1 << 6;
		inline constexpr std::uint16_t kLandData = 1 << 7;
		inline constexpr std::uint16_t kEyeData = 1 << 8;
		inline constexpr std::uint16_t kFullPrecision = 0x400;
	}

	// Gamebryo NiAlphaProperty flag bits.
	namespace AlphaFlags
	{
		inline constexpr std::uint16_t kBlendEnable = 1 << 0;
		inline constexpr std::uint16_t kTestEnable = 1 << 9;
	}

	template <class T>
	[[nodiscard]] inline T& Field(const void* a_base, std::size_t a_offset) noexcept
	{
		return *reinterpret_cast<T*>(reinterpret_cast<std::uintptr_t>(a_base) + a_offset);
	}

	[[nodiscard]] inline std::uintptr_t VTableOf(const void* a_object) noexcept
	{
		return a_object ? *reinterpret_cast<const std::uintptr_t*>(a_object) : 0;
	}

	// BSGeometry derives from NiAVObject through single inheritance.
	[[nodiscard]] inline RE::NiAVObject* AsAVObject(RE::BSGeometry* a_geometry) noexcept
	{
		return reinterpret_cast<RE::NiAVObject*>(a_geometry);
	}

	struct Geometry
	{
		[[nodiscard]] static RE::NiProperty* AlphaProperty(const RE::BSGeometry* a_geometry) noexcept
		{
			return Field<RE::NiProperty*>(a_geometry, Offsets::kGeometryAlphaProperty);
		}

		[[nodiscard]] static RE::BSShaderProperty* ShaderProperty(const RE::BSGeometry* a_geometry) noexcept
		{
			return Field<RE::BSShaderProperty*>(a_geometry, Offsets::kGeometryShaderProperty);
		}

		[[nodiscard]] static void* SkinInstance(const RE::BSGeometry* a_geometry) noexcept
		{
			return Field<void*>(a_geometry, Offsets::kGeometrySkinInstance);
		}

		[[nodiscard]] static void* RendererData(const RE::BSGeometry* a_geometry) noexcept
		{
			return Field<void*>(a_geometry, Offsets::kGeometryRendererData);
		}

		[[nodiscard]] static std::uint64_t VertexDesc(const RE::BSGeometry* a_geometry) noexcept
		{
			return Field<std::uint64_t>(a_geometry, Offsets::kGeometryVertexDesc);
		}

		[[nodiscard]] static std::uint16_t VertexDescFlags(std::uint64_t a_desc) noexcept
		{
			return static_cast<std::uint16_t>(a_desc >> 44);
		}

		[[nodiscard]] static std::uint32_t NumTriangles(const RE::BSGeometry* a_triShape) noexcept
		{
			return Field<std::uint32_t>(a_triShape, Offsets::kTriShapeNumTriangles);
		}

		[[nodiscard]] static std::uint16_t NumVertices(const RE::BSGeometry* a_triShape) noexcept
		{
			return Field<std::uint16_t>(a_triShape, Offsets::kTriShapeNumVertices);
		}
	};

	struct AlphaState
	{
		std::uint16_t flags{ 0 };
		std::uint8_t threshold{ 0 };

		[[nodiscard]] static AlphaState Read(const RE::NiProperty* a_alpha) noexcept
		{
			if (!a_alpha) {
				return {};
			}
			return { Field<std::uint16_t>(a_alpha, Offsets::kAlphaFlags), Field<std::uint8_t>(a_alpha, Offsets::kAlphaThreshold) };
		}

		[[nodiscard]] bool Blended() const noexcept { return (flags & AlphaFlags::kBlendEnable) != 0; }
		[[nodiscard]] bool Tested() const noexcept { return (flags & AlphaFlags::kTestEnable) != 0; }
		[[nodiscard]] std::uint32_t Key() const noexcept { return (static_cast<std::uint32_t>(flags) << 8) | threshold; }
	};

	struct Camera
	{
		[[nodiscard]] static const float (&WorldToCam(const RE::NiCamera* a_camera) noexcept)[4][4]
		{
			return Field<const float[4][4]>(a_camera, Offsets::kCameraWorldToCam);
		}

		[[nodiscard]] static float Near(const RE::NiCamera* a_camera) noexcept { return Field<float>(a_camera, Offsets::kCameraFrustumNear); }
		[[nodiscard]] static float Far(const RE::NiCamera* a_camera) noexcept { return Field<float>(a_camera, Offsets::kCameraFrustumFar); }
		[[nodiscard]] static bool Ortho(const RE::NiCamera* a_camera) noexcept { return Field<bool>(a_camera, Offsets::kCameraFrustumOrtho); }

		[[nodiscard]] static const RE::NiTransform& World(const RE::NiCamera* a_camera) noexcept
		{
			return reinterpret_cast<const RE::NiAVObject*>(a_camera)->world;
		}
	};
}
