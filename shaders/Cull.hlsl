// Per-view GPU culling and draw generation.
//
// The CPU uploads one work item per batched object that the engine registered
// for the view (the engine has already applied frustum, PreVis and occlusion
// plane culling). This pass adds hierarchical-Z occlusion culling against the
// previous frame's depth and then produces the indirect draw data:
//
//   CSCompact   - copies the indices of every surviving object into a ring
//                 buffer, one contiguous range per batch, and accumulates the
//                 batch's IndexCountPerInstance. One DrawIndexedInstancedIndirect
//                 per batch draws it. Works on every Direct3D 11 GPU.
//   CSMultiDraw - writes one DrawIndexedInstancedIndirect record per object
//                 (zero indices when culled). Consumed by NVAPI's
//                 MultiDrawIndexedInstancedIndirect or a CPU loop.
//
// This mirrors Nvidium's design, where task shaders test section visibility
// and emit mesh-shader work; Direct3D 11 has no mesh shaders, so compute plus
// indirect draws take their place.

#include "Common.hlsli"

StructuredBuffer<Member> gMembers : register(t0);
StructuredBuffer<WorkItem> gWork : register(t1);
ByteAddressBuffer gArenaIndices : register(t2);
Texture2D<float> gHiZ : register(t3);

RWByteAddressBuffer gArgs : register(u0);
RWByteAddressBuffer gRing : register(u1);

cbuffer CullParams : register(b0)
{
	row_major float4x4 gViewProj;  // world -> clip of the frame the Hi-Z pyramid was built from
	float4 gHiZSize;               // mip 0 width, height, 1/width, 1/height
	uint gWorkCount;
	uint gGroupsX;
	uint gOcclusion;               // 0: occlusion test disabled for this view
	uint gReversedZ;
	uint gHiZMipCount;
	float gDepthBias;
	uint gArgsBase;                // CSMultiDraw: first args record written by this dispatch
	uint gPad;
};

bool OcclusionVisible(float4 sphere)
{
	if (gOcclusion == 0) {
		return true;
	}

	float2 lo = 1e30f;
	float2 hi = -1e30f;
	float nearest = gReversedZ != 0 ? 0.0f : 1.0f;

	[unroll]
	for (uint i = 0; i < 8; ++i) {
		const float3 corner = sphere.xyz + sphere.w * float3((i & 1) != 0 ? 1.0f : -1.0f, (i & 2) != 0 ? 1.0f : -1.0f, (i & 4) != 0 ? 1.0f : -1.0f);
		const float4 clip = mul(gViewProj, float4(corner, 1.0f));
		if (clip.w <= 1e-3f) {
			return true;  // crosses the camera plane: no reliable screen bounds
		}
		const float3 ndc = clip.xyz / clip.w;
		lo = min(lo, ndc.xy);
		hi = max(hi, ndc.xy);
		nearest = gReversedZ != 0 ? max(nearest, ndc.z) : min(nearest, ndc.z);
	}

	// Outside the previous view the pyramid holds no information.
	if (hi.x < -1.0f || lo.x > 1.0f || hi.y < -1.0f || lo.y > 1.0f) {
		return true;
	}
	if (gReversedZ != 0 ? nearest >= 1.0f : nearest <= 0.0f) {
		return true;
	}

	lo = saturate(lo * 0.5f + 0.5f);
	hi = saturate(hi * 0.5f + 0.5f);
	const float2 uvMin = float2(lo.x, 1.0f - hi.y);
	const float2 uvMax = float2(hi.x, 1.0f - lo.y);

	// Work in mip-0 texels and shift down: texel t of mip L is reduced from
	// mip-0 texels [t << L, ((t + 1) << L) - 1] (the last one also owns the odd
	// remainder), so x >> L always lands on a texel that covers x, even for
	// sizes that are not powers of two.
	const uint2 size0 = uint2(gHiZSize.xy);
	const uint2 t0 = min(uint2(uvMin * gHiZSize.xy), size0 - 1);
	const uint2 t1 = min(uint2(uvMax * gHiZSize.xy), size0 - 1);

	// Smallest mip where the rectangle spans at most 2x2 texels: e texels in a
	// row touch at most two blocks of 2^L texels when e <= 2^L + 1.
	const uint2 extentXY = t1 - t0 + 1;
	const uint extent = max(extentXY.x, extentXY.y);
	const uint mip = min(extent <= 2 ? 0u : firstbithigh(extent - 2) + 1, gHiZMipCount - 1);
	const uint2 size = max(size0 >> mip, uint2(1, 1));

	const int2 p0 = int2(min(t0 >> mip, size - 1));
	const int2 p1 = int2(min(t1 >> mip, size - 1));

	const float d0 = gHiZ.Load(int3(p0.x, p0.y, mip));
	const float d1 = gHiZ.Load(int3(p1.x, p0.y, mip));
	const float d2 = gHiZ.Load(int3(p0.x, p1.y, mip));
	const float d3 = gHiZ.Load(int3(p1.x, p1.y, mip));

	if (gReversedZ != 0) {
		const float occluder = min(min(d0, d1), min(d2, d3));
		return nearest >= occluder - gDepthBias;
	}
	const float occluder = max(max(d0, d1), max(d2, d3));
	return nearest <= occluder + gDepthBias;
}

groupshared uint sVisible;
groupshared uint sDestination;

[numthreads(64, 1, 1)]
void CSCompact(uint3 group : SV_GroupID, uint thread : SV_GroupIndex)
{
	const uint w = group.y * gGroupsX + group.x;
	const bool valid = w < gWorkCount;

	WorkItem work = (WorkItem)0;
	Member member = (Member)0;
	if (valid) {
		work = gWork[w];
		member = gMembers[work.member];
	}

	if (thread == 0) {
		uint visible = 0;
		uint destination = 0;
		if (valid && OcclusionVisible(member.sphere)) {
			const uint args = work.slot * ARGS_STRIDE;
			uint offset;
			gArgs.InterlockedAdd(args, member.indexCount, offset);
			destination = gArgs.Load(args + 8) + offset;  // StartIndexLocation set by the CPU
			visible = 1;
		}
		sVisible = visible;
		sDestination = destination;
	}
	GroupMemoryBarrierWithGroupSync();

	if (sVisible != 0) {
		for (uint i = thread; i < member.indexCount; i += 64) {
			gRing.Store((sDestination + i) * 4, gArenaIndices.Load((member.indexStart + i) * 4));
		}
	}
}

[numthreads(64, 1, 1)]
void CSMultiDraw(uint3 id : SV_DispatchThreadID)
{
	const uint w = id.y * gGroupsX * 64 + id.x;
	if (w >= gWorkCount) {
		return;
	}

	const Member member = gMembers[gWork[w].member];
	const bool visible = OcclusionVisible(member.sphere);
	const uint address = (gArgsBase + w) * ARGS_STRIDE;
	gArgs.Store4(address, uint4(visible ? member.indexCount : 0, 1, member.indexStart, 0));
	gArgs.Store(address + 16, 0);
}
