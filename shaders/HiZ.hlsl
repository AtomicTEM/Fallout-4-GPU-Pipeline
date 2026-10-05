// Builds the hierarchical depth pyramid used for occlusion culling.
//
// CSInit copies the active viewport of the scene depth into mip 0. CSReduce
// builds each further mip from the previous one, keeping the farthest depth
// (nearest for reversed-Z) so the test in Cull.hlsl is conservative. Odd
// source sizes fold the extra row/column into the last destination texel.

Texture2D<float> gSource : register(t0);
RWTexture2D<float> gDest : register(u0);

cbuffer HiZParams : register(b0)
{
	uint2 gSourceOffset;  // CSInit: viewport origin inside the depth texture
	uint2 gSourceSize;    // size of the source level (CSReduce) or viewport (CSInit)
	uint2 gDestSize;
	uint gReversedZ;
	uint gPad;
};

float Farthest(float a, float b)
{
	return gReversedZ != 0 ? min(a, b) : max(a, b);
}

[numthreads(8, 8, 1)]
void CSInit(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= gDestSize)) {
		return;
	}
	gDest[id.xy] = gSource.Load(int3(gSourceOffset + id.xy, 0));
}

[numthreads(8, 8, 1)]
void CSReduce(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= gDestSize)) {
		return;
	}

	const uint2 base = id.xy * 2;
	const uint2 last = gSourceSize - 1;

	float depth = gSource.Load(int3(min(base, last), 0));
	depth = Farthest(depth, gSource.Load(int3(min(base + uint2(1, 0), last), 0)));
	depth = Farthest(depth, gSource.Load(int3(min(base + uint2(0, 1), last), 0)));
	depth = Farthest(depth, gSource.Load(int3(min(base + uint2(1, 1), last), 0)));

	// Odd source width/height: the last destination column/row also covers the extra texels.
	const bool extraX = (gSourceSize.x & 1) != 0 && id.x == gDestSize.x - 1;
	const bool extraY = (gSourceSize.y & 1) != 0 && id.y == gDestSize.y - 1;
	if (extraX) {
		depth = Farthest(depth, gSource.Load(int3(min(base + uint2(2, 0), last), 0)));
		depth = Farthest(depth, gSource.Load(int3(min(base + uint2(2, 1), last), 0)));
	}
	if (extraY) {
		depth = Farthest(depth, gSource.Load(int3(min(base + uint2(0, 2), last), 0)));
		depth = Farthest(depth, gSource.Load(int3(min(base + uint2(1, 2), last), 0)));
	}
	if (extraX && extraY) {
		depth = Farthest(depth, gSource.Load(int3(min(base + uint2(2, 2), last), 0)));
	}

	gDest[id.xy] = depth;
}
