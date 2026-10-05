#pragma once

#include "Render/ShaderConstants.h"
#include "Scene/VertexFormat.h"
#include "Util/Math.h"

namespace GWP
{
	// How one member's vertices are re-expressed in its batch anchor's space.
	struct MergeTransform
	{
		Math::Affine position;   // inverse(anchorWorld) * memberWorld
		Math::Affine direction;  // rotation for normals, tangents and bitangents (exact identity when unrotated)
		bool mirrored{ false };  // negative determinant: triangle winding must be flipped
	};

	// Returns nullopt for degenerate transforms (zero scale).
	[[nodiscard]] std::optional<MergeTransform> ComputeMergeTransform(const RE::NiTransform& a_anchorWorld, const RE::NiTransform& a_memberWorld, bool a_transposed, bool a_canRotateDirections);

	[[nodiscard]] ShaderConstants::Merge MakeMergeConstants(const VertexPlan& a_plan, const MergeTransform& a_transform, std::uint32_t a_srcBase, std::uint32_t a_dstBase, std::uint32_t a_vertexCount);
}
