// Shared definitions for the GPU World Pipeline compute shaders.
// All shaders target cs_5_0 so they run on every Direct3D 11 GPU Fallout 4 supports.

#ifndef GWP_COMMON_HLSLI
#define GWP_COMMON_HLSLI

// One merged object ("member") inside a batch. Mirrors GWP::GpuMember (32 bytes).
struct Member
{
	float4 sphere;      // world-space bounding sphere: xyz = centre (absolute coordinates), w = radius
	uint indexStart;    // first index of the member inside the arena index buffer (elements)
	uint indexCount;
	uint bucket;
	uint flags;
};

// One visible member registered by the engine for the view being drawn.
struct WorkItem
{
	uint member;
	uint slot;          // compaction: args record of the member's batch; multi-draw: unused
};

// D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS, five dwords.
#define ARGS_STRIDE 20

// Vertex element formats understood by MergeVertices.hlsl (GWP::VertexFormat).
#define FMT_RAW    0
#define FMT_F32X3  1
#define FMT_F32X4  2
#define FMT_F16X4  3
#define FMT_UN8X4  4
#define FMT_SN8X4  5

// Vertex element kinds.
#define KIND_COPY      0
#define KIND_POSITION  1
#define KIND_DIRECTION 2

#endif
