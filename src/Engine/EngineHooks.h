#pragma once

namespace GWP::EngineHooks
{
	// Virtual function slots (CommonLibF4RD and libxse/commonlibf4 agree).
	namespace Slot
	{
		// BSShaderProperty
		inline constexpr std::size_t kGetRenderPasses = 0x2B;
		inline constexpr std::size_t kGetRenderPassesShadowMapOrMask = 0x2C;
		inline constexpr std::size_t kGetRenderDepthPass = 0x30;

		// BSShader
		inline constexpr std::size_t kSetupGeometry = 0x07;
		inline constexpr std::size_t kRestoreGeometry = 0x08;

		// NiAccumulator / BSShaderAccumulator
		inline constexpr std::size_t kStartAccumulating = 0x28;
		inline constexpr std::size_t kFinishAccumulating = 0x29;
		inline constexpr std::size_t kFinishAccumulatingPreResolveDepth = 0x2E;
		inline constexpr std::size_t kFinishAccumulatingPostResolveDepth = 0x2F;
	}

	// Patches the vtables of BSLightingShaderProperty, BSLightingShader,
	// BSUtilityShader and BSShaderAccumulator. Only vtable entries are
	// replaced; no code bytes are modified. Returns false (and installs
	// nothing) if any vtable cannot be resolved.
	bool Install();

	// Primary vtable of BSTriShape, used to recognise plain triangle shapes.
	[[nodiscard]] std::uintptr_t TriShapeVTable() noexcept;
}
