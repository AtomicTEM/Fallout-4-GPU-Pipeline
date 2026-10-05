#include "Scene/MergeMath.h"

namespace GWP
{
	static_assert(VertexPlan::kMaxElements == std::extent_v<decltype(ShaderConstants::Merge::elements)>);

	std::optional<MergeTransform> ComputeMergeTransform(const RE::NiTransform& a_anchorWorld, const RE::NiTransform& a_memberWorld, bool a_transposed, bool a_canRotateDirections)
	{
		const auto anchor = Math::Affine::FromNiTransform(a_anchorWorld, a_transposed);
		const auto member = Math::Affine::FromNiTransform(a_memberWorld, a_transposed);
		const auto anchorInverse = anchor.Inverse();
		if (!anchorInverse) {
			return std::nullopt;
		}

		MergeTransform result;
		result.position = *anchorInverse * member;

		const auto determinant = result.position.Determinant();
		if (std::abs(determinant) < 1e-12) {
			return std::nullopt;
		}
		result.mirrored = determinant < 0.0;

		// Members oriented like the anchor keep their directions bit-exact.
		const auto scale = std::cbrt(std::abs(determinant));
		bool identity = true;
		for (std::size_t row = 0; row < 3 && identity; ++row) {
			for (std::size_t col = 0; col < 3; ++col) {
				const auto expected = row == col ? 1.0 : 0.0;
				if (std::abs(result.position.m[row][col] / scale - expected) > 1e-6) {
					identity = false;
					break;
				}
			}
		}

		if (!identity && a_canRotateDirections) {
			if (auto normal = result.position.NormalMatrix()) {
				// Remove the uniform scale so unit vectors stay unit length.
				const auto factor = 1.0 / std::cbrt(std::abs(normal->Determinant()));
				for (auto& row : normal->m) {
					for (auto& value : row) {
						value *= factor;
					}
				}
				result.direction = *normal;
			}
		}
		return result;
	}

	ShaderConstants::Merge MakeMergeConstants(const VertexPlan& a_plan, const MergeTransform& a_transform, std::uint32_t a_srcBase, std::uint32_t a_dstBase, std::uint32_t a_vertexCount)
	{
		ShaderConstants::Merge merge{};
		const auto position = Math::GpuAffine::From(a_transform.position);
		const auto direction = Math::GpuAffine::From(a_transform.direction);
		std::memcpy(merge.position, position.rows, sizeof(merge.position));
		std::memcpy(merge.direction, direction.rows, sizeof(merge.direction));
		merge.srcBase = a_srcBase;
		merge.srcStride = a_plan.srcStride;
		merge.dstBase = a_dstBase;
		merge.dstStride = a_plan.dstStride;
		merge.vertexCount = a_vertexCount;
		merge.elementCount = static_cast<std::uint32_t>(a_plan.elements.size());
		merge.flags = a_plan.ShaderFlags();
		for (std::size_t e = 0; e < a_plan.elements.size(); ++e) {
			const auto& element = a_plan.elements[e];
			merge.elements[e][0] = element.srcOffset | (element.dstOffset << 16);
			merge.elements[e][1] = static_cast<std::uint32_t>(element.srcFormat);
			merge.elements[e][2] = static_cast<std::uint32_t>(element.dstFormat);
			merge.elements[e][3] = static_cast<std::uint32_t>(element.kind) | (element.dwords << 8);
		}
		return merge;
	}
}
