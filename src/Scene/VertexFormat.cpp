#include "Scene/VertexFormat.h"

#include "Engine/Layouts.h"

namespace GWP
{
	namespace
	{
		// BSGraphics::Vertex::Attribute
		enum Attribute : std::uint32_t
		{
			kPosition = 0,
			kTexCoord0 = 1,
			kTexCoord1 = 2,
			kNormal = 3,
			kTangent = 4,
			kColor = 5
		};

		[[nodiscard]] std::uint32_t AttributeOffset(std::uint64_t a_desc, std::uint32_t a_attribute) noexcept
		{
			// Same decoding as BSGraphics::VertexDesc::GetAttributeOffset.
			return a_attribute == kPosition ? 0u : static_cast<std::uint32_t>((a_desc >> (4 * a_attribute + 2)) & 0x3C);
		}

		[[nodiscard]] const InputLayouts::Element* ElementAt(const InputLayouts::Layout* a_layout, std::uint32_t a_offset) noexcept
		{
			if (!a_layout) {
				return nullptr;
			}
			for (const auto& element : a_layout->elements) {
				if (element.inputSlot == 0 && element.alignedByteOffset == a_offset) {
					return &element;
				}
			}
			return nullptr;
		}
	}

	std::optional<VertexPlan> BuildVertexPlan(std::uint64_t a_vertexDesc, std::uint32_t a_capturedStride, const InputLayouts::Layout* a_layout, bool a_rotateBitangentW, std::string* a_reason)
	{
		const auto fail = [&](std::string a_why) -> std::optional<VertexPlan> {
			if (a_reason) {
				*a_reason = std::move(a_why);
			}
			return std::nullopt;
		};

		namespace VF = Engine::VertexFlags;
		const auto flags = Engine::Geometry::VertexDescFlags(a_vertexDesc);
		if ((flags & VF::kVertex) == 0) {
			return fail("vertex format has no position");
		}
		if ((flags & (VF::kSkinned | VF::kLandData | VF::kEyeData)) != 0) {
			return fail(fmt::format("unsupported vertex flags 0x{:X}", flags));
		}

		const auto stride = static_cast<std::uint32_t>(a_vertexDesc & 0xF) * 4;
		if (stride == 0 || stride != a_capturedStride) {
			return fail(fmt::format("descriptor stride {} does not match bound stride {}", stride, a_capturedStride));
		}

		struct Attr
		{
			std::uint32_t attribute;
			std::uint32_t offset;
		};

		std::vector<Attr> attributes{ { kPosition, 0 } };
		const std::pair<std::uint16_t, std::uint32_t> optional[]{
			{ VF::kUV, kTexCoord0 },
			{ VF::kUV2, kTexCoord1 },
			{ VF::kNormal, kNormal },
			{ VF::kTangent, kTangent },
			{ VF::kColors, kColor },
		};
		for (const auto& [flag, attribute] : optional) {
			if ((flags & flag) != 0) {
				attributes.push_back({ attribute, AttributeOffset(a_vertexDesc, attribute) });
			}
		}
		std::ranges::sort(attributes, {}, &Attr::offset);

		for (std::size_t i = 1; i < attributes.size(); ++i) {
			if (attributes[i].offset <= attributes[i - 1].offset || attributes[i].offset >= stride) {
				return fail(fmt::format("attribute offsets not increasing (desc 0x{:016X})", a_vertexDesc));
			}
		}
		if (attributes.size() > VertexPlan::kMaxElements) {
			return fail("too many vertex attributes");
		}

		// Every element the lighting shader reads must start on an attribute boundary.
		if (a_layout) {
			for (const auto& element : a_layout->elements) {
				if (element.inputSlot != 0) {
					return fail("input layout uses more than one vertex stream");
				}
				if (element.inputSlotClass != D3D11_INPUT_PER_VERTEX_DATA) {
					return fail("input layout uses instanced data");
				}
				const bool aligned = std::ranges::any_of(attributes, [&](const Attr& a_attr) { return a_attr.offset == element.alignedByteOffset; });
				if (!aligned) {
					return fail(fmt::format("layout element {}{} at offset {} does not start an attribute", element.semanticName, element.semanticIndex, element.alignedByteOffset));
				}
			}
		}

		VertexPlan plan;
		plan.vertexDesc = a_vertexDesc;
		plan.srcStride = stride;

		for (std::size_t i = 0; i < attributes.size(); ++i) {
			const auto& attr = attributes[i];
			const auto next = i + 1 < attributes.size() ? attributes[i + 1].offset : stride;
			const auto size = next - attr.offset;
			if (size == 0 || size % 4 != 0) {
				return fail(fmt::format("attribute {} has size {}", attr.attribute, size));
			}

			ElementPlan element;
			element.srcOffset = attr.offset;
			element.dwords = size / 4;
			const auto* const bound = ElementAt(a_layout, attr.offset);

			switch (attr.attribute) {
			case kPosition:
				{
					const bool fullPrecision = (flags & VF::kFullPrecision) != 0;
					element.kind = ElementKind::kPosition;
					if (size == 8 && !fullPrecision) {
						if (bound && bound->format != DXGI_FORMAT_R16G16B16A16_FLOAT) {
							return fail("half-precision position bound with an unexpected format");
						}
						element.srcFormat = ElementFormat::kF16x4;
						element.dstFormat = ElementFormat::kF32x4;
						plan.shift = 8;
					} else if (size == 16 && fullPrecision) {
						element.srcFormat = ElementFormat::kF32x4;
						element.dstFormat = ElementFormat::kF32x4;
					} else if (size == 12 && fullPrecision) {
						element.srcFormat = ElementFormat::kF32x3;
						element.dstFormat = ElementFormat::kF32x3;
					} else {
						return fail(fmt::format("position block of {} bytes (full precision: {})", size, fullPrecision));
					}
					plan.positionBytes = size;
					plan.positionIndex = static_cast<std::uint32_t>(i);
					break;
				}
			case kNormal:
			case kTangent:
				{
					ElementFormat format = ElementFormat::kRaw;
					if (size == 4 && bound) {
						if (bound->format == DXGI_FORMAT_R8G8B8A8_UNORM) {
							format = ElementFormat::kUN8x4;
						} else if (bound->format == DXGI_FORMAT_R8G8B8A8_SNORM) {
							format = ElementFormat::kSN8x4;
						}
					}
					if (format == ElementFormat::kRaw) {
						// Encoding not confirmed by the layout: copy verbatim and
						// only accept members that share the anchor's rotation.
						plan.canRotateDirections = false;
						element.kind = ElementKind::kCopy;
					} else {
						element.kind = ElementKind::kDirection;
						element.srcFormat = format;
						element.dstFormat = format;
						(attr.attribute == kNormal ? plan.normalIndex : plan.tangentIndex) = static_cast<std::uint32_t>(i);
					}
					break;
				}
			default:
				element.kind = ElementKind::kCopy;
				break;
			}

			plan.elements.push_back(element);
		}

		for (auto& element : plan.elements) {
			element.dstOffset = element.srcOffset == 0 ? 0 : element.srcOffset + plan.shift;
		}
		plan.dstStride = stride + plan.shift;

		const auto& position = plan.elements[plan.positionIndex];
		const bool fourComponentPosition = position.srcFormat == ElementFormat::kF16x4 || position.srcFormat == ElementFormat::kF32x4;
		plan.bitangentInW = a_rotateBitangentW && fourComponentPosition && plan.normalIndex != VertexPlan::kNone && plan.tangentIndex != VertexPlan::kNone;

		return plan;
	}
}
