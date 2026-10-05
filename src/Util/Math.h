#pragma once

namespace GWP::Math
{
	// Affine transform with the scale folded into the 3x3 part:
	//   p' = m * p + t   (m applied to a column vector, rows dotted with p)
	// Gamebryo's NiTransform maps a point as rotate * (scale * p) + translate.
	struct Affine
	{
		double m[3][3]{ { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
		double t[3]{ 0, 0, 0 };

		[[nodiscard]] static Affine FromNiTransform(const RE::NiTransform& a_transform, bool a_transposed = false) noexcept
		{
			Affine result;
			for (std::size_t row = 0; row < 3; ++row) {
				for (std::size_t col = 0; col < 3; ++col) {
					const auto value = a_transposed ? a_transform.rotate.entry[col].pt[row] : a_transform.rotate.entry[row].pt[col];
					result.m[row][col] = static_cast<double>(value) * static_cast<double>(a_transform.scale);
				}
				result.t[row] = static_cast<double>(a_transform.translate[static_cast<std::uint32_t>(row)]);
			}
			return result;
		}

		[[nodiscard]] double Determinant() const noexcept
		{
			return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
			       m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
			       m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
		}

		[[nodiscard]] std::optional<Affine> Inverse() const noexcept
		{
			const auto det = Determinant();
			if (std::abs(det) < 1e-12) {
				return std::nullopt;
			}

			const auto inv = 1.0 / det;
			Affine result;
			result.m[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) * inv;
			result.m[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * inv;
			result.m[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * inv;
			result.m[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) * inv;
			result.m[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * inv;
			result.m[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * inv;
			result.m[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) * inv;
			result.m[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * inv;
			result.m[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * inv;

			for (std::size_t row = 0; row < 3; ++row) {
				result.t[row] = -(result.m[row][0] * t[0] + result.m[row][1] * t[1] + result.m[row][2] * t[2]);
			}
			return result;
		}

		// (this * a_rhs)(p) == this(a_rhs(p))
		[[nodiscard]] Affine operator*(const Affine& a_rhs) const noexcept
		{
			Affine result;
			for (std::size_t row = 0; row < 3; ++row) {
				for (std::size_t col = 0; col < 3; ++col) {
					result.m[row][col] = m[row][0] * a_rhs.m[0][col] + m[row][1] * a_rhs.m[1][col] + m[row][2] * a_rhs.m[2][col];
				}
				result.t[row] = m[row][0] * a_rhs.t[0] + m[row][1] * a_rhs.t[1] + m[row][2] * a_rhs.t[2] + t[row];
			}
			return result;
		}

		void Apply(const double a_in[3], double a_out[3]) const noexcept
		{
			for (std::size_t row = 0; row < 3; ++row) {
				a_out[row] = m[row][0] * a_in[0] + m[row][1] * a_in[1] + m[row][2] * a_in[2] + t[row];
			}
		}

		// Inverse-transpose of the 3x3 part, used for normals/tangents.
		[[nodiscard]] std::optional<Affine> NormalMatrix() const noexcept
		{
			auto inverse = Inverse();
			if (!inverse) {
				return std::nullopt;
			}
			Affine result;
			for (std::size_t row = 0; row < 3; ++row) {
				for (std::size_t col = 0; col < 3; ++col) {
					result.m[row][col] = inverse->m[col][row];
				}
			}
			return result;
		}
	};

	// Row-major float3x4 as consumed by the HLSL side (three float4 rows, xyz = matrix row, w = translation).
	struct alignas(16) GpuAffine
	{
		float rows[3][4];

		[[nodiscard]] static GpuAffine From(const Affine& a_affine) noexcept
		{
			GpuAffine result{};
			for (std::size_t row = 0; row < 3; ++row) {
				for (std::size_t col = 0; col < 3; ++col) {
					result.rows[row][col] = static_cast<float>(a_affine.m[row][col]);
				}
				result.rows[row][3] = static_cast<float>(a_affine.t[row]);
			}
			return result;
		}
	};
	static_assert(sizeof(GpuAffine) == 48);

	[[nodiscard]] inline bool NearlyEqual(const RE::NiTransform& a_lhs, const RE::NiTransform& a_rhs, float a_epsilon = 1e-4F) noexcept
	{
		for (std::uint32_t row = 0; row < 3; ++row) {
			for (std::uint32_t col = 0; col < 3; ++col) {
				if (std::abs(a_lhs.rotate.entry[row].pt[col] - a_rhs.rotate.entry[row].pt[col]) > a_epsilon) {
					return false;
				}
			}
			if (std::abs(a_lhs.translate[row] - a_rhs.translate[row]) > a_epsilon * 100.0F) {
				return false;
			}
		}
		return std::abs(a_lhs.scale - a_rhs.scale) <= a_epsilon;
	}

	[[nodiscard]] inline std::array<double, 4> MulRowMajor(const float (&a_m)[4][4], double a_x, double a_y, double a_z) noexcept
	{
		std::array<double, 4> out{};
		for (std::size_t row = 0; row < 4; ++row) {
			out[row] = a_m[row][0] * a_x + a_m[row][1] * a_y + a_m[row][2] * a_z + a_m[row][3];
		}
		return out;
	}

	[[nodiscard]] constexpr std::uint64_t HashCombine(std::uint64_t a_seed, std::uint64_t a_value) noexcept
	{
		a_value *= 0x9E3779B97F4A7C15ull;
		a_value ^= a_value >> 32;
		return (a_seed ^ a_value) * 0xBF58476D1CE4E5B9ull;
	}
}
