#pragma once

// GPU data layouts shared by the plugin and tests/GpuTests.cpp. Each struct
// mirrors a cbuffer or structured buffer element in shaders/ byte for byte.

namespace GWP
{
	// One merged object; mirrors `Member` in shaders/Common.hlsli.
	struct GpuMember
	{
		float sphere[4]{};  // world-space centre (absolute) and radius
		std::uint32_t indexStart{ 0 };
		std::uint32_t indexCount{ 0 };
		std::uint32_t bucket{ 0 };
		std::uint32_t flags{ 0 };
	};
	static_assert(sizeof(GpuMember) == 32);
}

namespace GWP::ShaderConstants
{
	// shaders/MergeVertices.hlsl : MergeParams
	struct alignas(16) Merge
	{
		float position[3][4];   // member -> anchor rows, w = translation
		float direction[3][4];  // rotation for normals/tangents/bitangents
		std::uint32_t srcBase;
		std::uint32_t srcStride;
		std::uint32_t dstBase;
		std::uint32_t dstStride;
		std::uint32_t vertexCount;
		std::uint32_t elementCount;
		std::uint32_t flags;
		std::uint32_t pad;
		std::uint32_t elements[8][4];  // srcOffset | dstOffset << 16, srcFormat, dstFormat, kind | dwords << 8
	};
	static_assert(sizeof(Merge) == 256);

	// shaders/MergeIndices.hlsl : IndexParams
	struct alignas(16) Index
	{
		std::uint32_t srcByteOffset;
		std::uint32_t indexCount;
		std::uint32_t dstIndex;
		std::uint32_t vertexBase;
		std::uint32_t vertexCount;
		std::uint32_t flipWinding;
		std::uint32_t index32;
		std::uint32_t errorSlot;
	};
	static_assert(sizeof(Index) == 32);

	// shaders/Cull.hlsl : CullParams
	struct alignas(16) Cull
	{
		float viewProj[4][4];  // row-major world -> clip
		float hizSize[4];
		std::uint32_t workCount;
		std::uint32_t groupsX;
		std::uint32_t occlusion;
		std::uint32_t reversedZ;
		std::uint32_t hizMips;
		float depthBias;
		std::uint32_t argsBase;
		std::uint32_t pad;
	};
	static_assert(sizeof(Cull) == 112);

	// shaders/HiZ.hlsl : HiZParams
	struct alignas(16) HiZ
	{
		std::uint32_t sourceOffset[2];
		std::uint32_t sourceSize[2];
		std::uint32_t destSize[2];
		std::uint32_t reversedZ;
		std::uint32_t pad;
	};
	static_assert(sizeof(HiZ) == 32);

	inline constexpr std::uint32_t kArgsStride = 20;  // D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS
}
