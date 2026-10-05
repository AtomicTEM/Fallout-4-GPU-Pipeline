// Copies one object's indices into the arena index buffer as 32-bit,
// batch-local indices (offset by the object's first vertex inside the batch).
// Out-of-range indices are counted so the CPU can discard a corrupt batch.

#include "Common.hlsli"

ByteAddressBuffer gSource : register(t0);
RWByteAddressBuffer gDest : register(u0);
RWByteAddressBuffer gErrors : register(u1);

cbuffer IndexParams : register(b0)
{
	uint gSrcByteOffset;  // byte offset of the first source index in gSource
	uint gIndexCount;
	uint gDstIndex;       // first destination element in gDest
	uint gVertexBase;     // first vertex of the object inside its batch
	uint gVertexCount;    // vertices owned by the object
	uint gFlipWinding;    // mirrored transform relative to the anchor
	uint gIndex32;        // source indices are 32-bit
	uint gErrorSlot;
};

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
	const uint i = id.x;
	if (i >= gIndexCount) {
		return;
	}

	uint source = i;
	if (gFlipWinding != 0) {
		const uint corner = i % 3;
		source = i - corner + (corner == 1 ? 2 : (corner == 2 ? 1 : 0));
	}

	uint value;
	if (gIndex32 != 0) {
		value = gSource.Load(gSrcByteOffset + source * 4);
	} else {
		const uint address = gSrcByteOffset + source * 2;
		const uint word = gSource.Load(address & ~3u);
		value = (address & 2) != 0 ? (word >> 16) : (word & 0xFFFF);
	}

	if (value >= gVertexCount) {
		gErrors.InterlockedAdd(gErrorSlot * 4, 1);
		value = 0;
	}

	gDest.Store((gDstIndex + i) * 4, value + gVertexBase);
}
