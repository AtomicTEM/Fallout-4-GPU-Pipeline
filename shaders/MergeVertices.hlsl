// Copies one object's vertices into a batch's merged vertex buffer,
// re-expressing them in the batch anchor's local space.
//
// The engine draws the batch with the anchor's own world matrix, so a member
// vertex p must be stored as  inverse(anchorWorld) * memberWorld * p.
// Positions are widened to 32-bit floats because members can sit thousands
// of units away from the anchor, where half precision would crack seams.
//
// Creation Engine vertices keep the bitangent split across the .w components
// of POSITION (x), NORMAL (y) and TANGENT (z); when BITANGENT_IN_W is set the
// three are reassembled, rotated and written back.

#include "Common.hlsli"

ByteAddressBuffer gSource : register(t0);
RWByteAddressBuffer gDest : register(u0);

cbuffer MergeParams : register(b0)
{
	float4 gPosition[3];   // rows of the member -> anchor affine transform, w = translation
	float4 gDirection[3];  // rows of the rotation applied to normals, tangents and bitangents
	uint gSrcBase;         // byte offset of vertex 0 in gSource
	uint gSrcStride;
	uint gDstBase;         // byte offset of vertex 0 in gDest
	uint gDstStride;
	uint gVertexCount;
	uint gElementCount;
	uint gFlags;           // bit 0: bitangent in w; bits 4-7 position, 8-11 normal, 12-15 tangent element index (15 = none)
	uint gPad;
	uint4 gElements[8];    // x: srcOffset | dstOffset << 16, y: srcFormat, z: dstFormat, w: kind | dwords << 8
};

#define NO_ELEMENT 15u

float4 LoadElement(uint address, uint format)
{
	if (format == FMT_F32X4) {
		return asfloat(gSource.Load4(address));
	}
	if (format == FMT_F32X3) {
		return float4(asfloat(gSource.Load3(address)), 1.0f);
	}
	if (format == FMT_F16X4) {
		const uint2 bits = gSource.Load2(address);
		return float4(f16tof32(bits.x), f16tof32(bits.x >> 16), f16tof32(bits.y), f16tof32(bits.y >> 16));
	}
	if (format == FMT_UN8X4) {
		const uint bits = gSource.Load(address);
		// Creation Engine stores unit vectors as unsigned bytes mapped from [-1, 1].
		return float4(bits & 0xFF, (bits >> 8) & 0xFF, (bits >> 16) & 0xFF, bits >> 24) * (2.0f / 255.0f) - 1.0f;
	}
	if (format == FMT_SN8X4) {
		const uint bits = gSource.Load(address);
		const int4 s = asint(uint4(bits << 24, bits << 16, bits << 8, bits)) >> 24;
		return max(float4(s) / 127.0f, -1.0f);
	}
	return 0.0f;
}

uint EncodeByte(float value, uint format)
{
	if (format == FMT_SN8X4) {
		return uint(int(round(clamp(value, -1.0f, 1.0f) * 127.0f)) & 0xFF);
	}
	return uint(round(saturate(value * 0.5f + 0.5f) * 255.0f));
}

void StoreDirection(uint address, uint format, float4 value)
{
	gDest.Store(address,
		EncodeByte(value.x, format) |
		(EncodeByte(value.y, format) << 8) |
		(EncodeByte(value.z, format) << 16) |
		(EncodeByte(value.w, format) << 24));
}

uint ElementIndex(uint shift)
{
	return (gFlags >> shift) & 0xF;
}

float3 Rotate(float3 v)
{
	return float3(dot(gDirection[0].xyz, v), dot(gDirection[1].xyz, v), dot(gDirection[2].xyz, v));
}

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
	const uint vertex = id.x;
	if (vertex >= gVertexCount) {
		return;
	}

	const uint src = gSrcBase + vertex * gSrcStride;
	const uint dst = gDstBase + vertex * gDstStride;

	const uint positionIndex = ElementIndex(4);
	const uint normalIndex = ElementIndex(8);
	const uint tangentIndex = ElementIndex(12);
	const bool bitangentInW = (gFlags & 1u) != 0 && positionIndex != NO_ELEMENT && normalIndex != NO_ELEMENT && tangentIndex != NO_ELEMENT;

	float3 bitangent = 0.0f;
	if (bitangentInW) {
		const float bx = LoadElement(src + (gElements[positionIndex].x & 0xFFFF), gElements[positionIndex].y).w;
		const float by = LoadElement(src + (gElements[normalIndex].x & 0xFFFF), gElements[normalIndex].y).w;
		const float bz = LoadElement(src + (gElements[tangentIndex].x & 0xFFFF), gElements[tangentIndex].y).w;
		bitangent = Rotate(float3(bx, by, bz));
	}

	for (uint e = 0; e < gElementCount; ++e) {
		const uint4 element = gElements[e];
		const uint srcAddress = src + (element.x & 0xFFFF);
		const uint dstAddress = dst + (element.x >> 16);
		const uint kind = element.w & 0xFF;
		const uint dwords = element.w >> 8;

		if (kind == KIND_POSITION) {
			const float4 p = LoadElement(srcAddress, element.y);
			const float4 local = float4(p.xyz, 1.0f);
			const float3 q = float3(dot(gPosition[0], local), dot(gPosition[1], local), dot(gPosition[2], local));
			const float w = bitangentInW ? bitangent.x : p.w;
			if (element.z == FMT_F32X3) {
				gDest.Store3(dstAddress, asuint(q));
			} else {
				gDest.Store4(dstAddress, asuint(float4(q, w)));
			}
		} else if (kind == KIND_DIRECTION) {
			const float4 d = LoadElement(srcAddress, element.y);
			float w = d.w;
			if (bitangentInW) {
				w = (e == normalIndex) ? bitangent.y : ((e == tangentIndex) ? bitangent.z : d.w);
			}
			StoreDirection(dstAddress, element.z, float4(Rotate(d.xyz), w));
		} else {
			for (uint i = 0; i < dwords; ++i) {
				gDest.Store(dstAddress + i * 4, gSource.Load(srcAddress + i * 4));
			}
		}
	}
}
