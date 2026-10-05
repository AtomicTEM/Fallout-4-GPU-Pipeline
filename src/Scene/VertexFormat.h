#pragma once

#include "Render/InputLayouts.h"

namespace GWP
{
	// Must match shaders/Common.hlsli.
	enum class ElementFormat : std::uint32_t
	{
		kRaw = 0,
		kF32x3 = 1,
		kF32x4 = 2,
		kF16x4 = 3,
		kUN8x4 = 4,
		kSN8x4 = 5
	};

	enum class ElementKind : std::uint32_t
	{
		kCopy = 0,
		kPosition = 1,
		kDirection = 2
	};

	struct ElementPlan
	{
		std::uint32_t srcOffset{ 0 };
		std::uint32_t dstOffset{ 0 };
		ElementFormat srcFormat{ ElementFormat::kRaw };
		ElementFormat dstFormat{ ElementFormat::kRaw };
		ElementKind kind{ ElementKind::kCopy };
		std::uint32_t dwords{ 0 };
	};

	// How one Creation Engine vertex format is rewritten into a batch vertex:
	// identical attribute order and encodings, except that POSITION is always
	// stored as 32-bit floats.
	struct VertexPlan
	{
		static constexpr std::uint32_t kMaxElements = 8;
		static constexpr std::uint32_t kNone = 15;

		std::uint64_t vertexDesc{ 0 };
		std::uint32_t srcStride{ 0 };
		std::uint32_t dstStride{ 0 };
		std::uint32_t positionBytes{ 0 };  // bytes POSITION occupies in the source vertex
		std::uint32_t shift{ 0 };          // bytes every later attribute moves by
		std::vector<ElementPlan> elements;
		std::uint32_t positionIndex{ kNone };
		std::uint32_t normalIndex{ kNone };
		std::uint32_t tangentIndex{ kNone };
		bool bitangentInW{ false };
		bool canRotateDirections{ true };  // false: members must share the anchor's orientation

		[[nodiscard]] std::uint32_t ShaderFlags() const noexcept
		{
			return (bitangentInW ? 1u : 0u) | (positionIndex << 4) | (normalIndex << 8) | (tangentIndex << 12);
		}
	};

	// Builds the plan from the engine's vertex descriptor and cross-checks it
	// against the input layout the lighting shader bound when the object was
	// captured. Returns nullopt (with a reason) for anything unsupported.
	[[nodiscard]] std::optional<VertexPlan> BuildVertexPlan(std::uint64_t a_vertexDesc, std::uint32_t a_capturedStride, const InputLayouts::Layout* a_layout, bool a_rotateBitangentW, std::string* a_reason);
}
