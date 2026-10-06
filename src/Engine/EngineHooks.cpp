#include "Engine/EngineHooks.h"

#include "Core/Guard.h"
#include "Core/Pipeline.h"

namespace GWP::EngineHooks
{
	namespace
	{
		[[nodiscard]] bool InRData(std::uintptr_t a_address, std::size_t a_size) noexcept
		{
			const auto rdata = REL::Module::get().segment(REL::Segment::rdata);
			return a_address >= rdata.address() && a_address <= rdata.address() + rdata.size() - a_size;
		}

		// MSVC RTTICompleteObjectLocator: the primary vtable of a class has
		// offset 0; secondary vtables (other base subobjects) do not. Vtables
		// and locators live in .rdata; an address from a wrong or damaged ID
		// database is rejected instead of dereferenced.
		[[nodiscard]] bool IsPrimaryVTable(std::uintptr_t a_vtable) noexcept
		{
			constexpr auto kPointer = sizeof(std::uintptr_t);
			if (!a_vtable || a_vtable % kPointer != 0 || !InRData(a_vtable - kPointer, kPointer)) {
				return false;
			}
			const auto locator = *reinterpret_cast<const std::uintptr_t*>(a_vtable - kPointer);
			if (locator % alignof(std::uint32_t) != 0 || !InRData(locator, 2 * sizeof(std::uint32_t))) {
				return false;
			}
			const auto* const fields = reinterpret_cast<const std::uint32_t*>(locator);
			return fields[0] == 1 && fields[1] == 0;  // x64 signature, offset
		}

		// MSVC RTTI type name of a vtable's class (".?AVName@@"), read through
		// its complete object locator; empty if anything lies outside the image.
		[[nodiscard]] std::string_view RTTIName(std::uintptr_t a_vtable) noexcept
		{
			const auto& module = REL::Module::get();
			const auto base = module.base();
			const auto end = base + module.image_size();
			const auto inImage = [&](std::uintptr_t a_address, std::size_t a_size) { return a_address >= base && a_address <= end && a_size <= end - a_address; };

			constexpr auto kPointer = sizeof(std::uintptr_t);
			if (!inImage(a_vtable - kPointer, kPointer)) {
				return {};
			}
			const auto locator = *reinterpret_cast<const std::uintptr_t*>(a_vtable - kPointer);
			if (!inImage(locator, 0x18)) {
				return {};
			}
			const auto typeDescriptor = base + *reinterpret_cast<const std::uint32_t*>(locator + 0x0C);
			constexpr std::size_t kNameOffset = 0x10;
			constexpr std::size_t kMaxName = 128;
			if (!inImage(typeDescriptor + kNameOffset, 1)) {
				return {};
			}
			const auto* const name = reinterpret_cast<const char*>(typeDescriptor + kNameOffset);
			const auto available = std::min<std::size_t>(kMaxName, end - (typeDescriptor + kNameOffset));
			const auto length = ::strnlen(name, available);
			return length < available ? std::string_view{ name, length } : std::string_view{};
		}

		// Resolves without REL::Relocation's fatal error path, so a missing or
		// outdated runtime database disables the plugin instead of the game.
		// The vtable's RTTI name must match the class, so a wrong ID can never
		// put a hook on another class.
		template <std::size_t N>
		[[nodiscard]] std::uintptr_t PrimaryVTable(const std::array<REL::ID, N>& a_ids, std::string_view a_name)
		{
			const auto expected = fmt::format(".?AV{}@@", a_name);
			for (const auto& id : a_ids) {
				const auto result = REL::IDDatabase::get().resolve(id);
				if (!result) {
					logger::error("hooks: vtable of {} unresolved: {} {}", a_name, REL::id_resolve_status_text(result.status), result.note);
					continue;
				}
				const auto address = REL::Module::get().base() + *result.rva;
				if (!IsPrimaryVTable(address)) {
					continue;
				}
				const auto name = RTTIName(address);
				if (name.empty()) {
					logger::warn("hooks: RTTI name of the {} vtable could not be read", a_name);
				} else if (name != expected) {
					logger::error("hooks: vtable for {} belongs to {}", a_name, name);
					continue;
				}
				return address;
			}
			logger::error("hooks: no primary vtable found for {}", a_name);
			return 0;
		}

		std::uintptr_t g_triShapeVTable{ 0 };
		std::uintptr_t g_accumulatorVTable{ 0 };
		ShaderVTables g_shaderVTables;

		template <class F>
		F Patch(std::uintptr_t a_vtable, std::size_t a_slot, F a_hook, std::string_view a_name)
		{
			REL::Relocation<std::uintptr_t> vtable{ a_vtable };
			const auto original = vtable.write_vfunc(a_slot, a_hook);
			logger::info("hooks: {} slot 0x{:X} (original at +0x{:X})", a_name, a_slot, original - REL::Module::get().base());
			return reinterpret_cast<F>(original);
		}

		struct LightingProperty
		{
			static RenderPassArray* GetRenderPasses(RE::BSShaderProperty* a_this, RE::BSGeometry* a_geometry, std::uint32_t a_mode, RE::BSShaderAccumulator* a_accumulator)
			{
				return Guarded(
					"GetRenderPasses", [&] { return getRenderPasses(a_this, a_geometry, a_mode, a_accumulator); },
					[&] { return Pipeline::Get().OnGetRenderPasses(PassKind::kMain, getRenderPasses, a_this, a_geometry, a_mode, a_accumulator); });
			}

			static RenderPassArray* GetRenderPassesShadowMapOrMask(RE::BSShaderProperty* a_this, RE::BSGeometry* a_geometry, std::uint32_t a_mode, RE::BSShaderAccumulator* a_accumulator)
			{
				return Guarded(
					"GetRenderPassesShadowMapOrMask", [&] { return getRenderPassesShadowMapOrMask(a_this, a_geometry, a_mode, a_accumulator); },
					[&] { return Pipeline::Get().OnGetRenderPasses(PassKind::kShadow, getRenderPassesShadowMapOrMask, a_this, a_geometry, a_mode, a_accumulator); });
			}

			static RE::BSRenderPass* GetRenderDepthPass(RE::BSShaderProperty* a_this, RE::BSGeometry* a_geometry)
			{
				return Guarded(
					"GetRenderDepthPass", [&] { return getRenderDepthPass(a_this, a_geometry); },
					[&] { return Pipeline::Get().OnGetRenderDepthPass(getRenderDepthPass, a_this, a_geometry); });
			}

			static inline GetRenderPasses_t getRenderPasses{ nullptr };
			static inline GetRenderPasses_t getRenderPassesShadowMapOrMask{ nullptr };
			static inline GetRenderDepthPass_t getRenderDepthPass{ nullptr };
		};

		using SetupGeometry_t = void (*)(RE::BSShader*, RE::BSRenderPass*);

		template <ShaderKind Kind>
		struct Shader
		{
			static void SetupGeometry(RE::BSShader* a_this, RE::BSRenderPass* a_pass)
			{
				setupGeometry(a_this, a_pass);
				Guarded("SetupGeometry", [&] { Pipeline::Get().OnSetupGeometry(Kind, a_pass); });
			}

			static void RestoreGeometry(RE::BSShader* a_this, RE::BSRenderPass* a_pass)
			{
				Guarded("RestoreGeometry", [&] { Pipeline::Get().OnRestoreGeometry(Kind, a_pass); });
				restoreGeometry(a_this, a_pass);
			}

			static inline SetupGeometry_t setupGeometry{ nullptr };
			static inline SetupGeometry_t restoreGeometry{ nullptr };
		};

		struct Accumulator
		{
			using Start_t = void (*)(RE::BSShaderAccumulator*, const RE::NiCamera*);
			using Finish_t = void (*)(RE::BSShaderAccumulator*);

			static void StartAccumulating(RE::BSShaderAccumulator* a_this, const RE::NiCamera* a_camera)
			{
				Guarded("StartAccumulating", [&] { Pipeline::Get().OnStartAccumulating(a_this); });
				start(a_this, a_camera);
			}

			template <FinishKind Kind>
			static void Finish(RE::BSShaderAccumulator* a_this)
			{
				Guarded("FinishAccumulating (begin)", [&] { Pipeline::Get().OnFinishBegin(a_this, Kind); });
				finish[static_cast<std::size_t>(Kind)](a_this);
				Guarded("FinishAccumulating (end)", [&] { Pipeline::Get().OnFinishEnd(a_this, Kind); });
			}

			static inline Start_t start{ nullptr };
			static inline std::array<Finish_t, 3> finish{};
		};
	}

	std::uintptr_t TriShapeVTable() noexcept
	{
		return g_triShapeVTable;
	}

	std::string_view ClassName(std::uintptr_t a_vtable) noexcept
	{
		return RTTIName(a_vtable);
	}

	const ShaderVTables& HookedShaderVTables() noexcept
	{
		return g_shaderVTables;
	}

	std::uintptr_t AccumulatorVTable() noexcept
	{
		return g_accumulatorVTable;
	}

	bool Install()
	{
		const auto triShape = PrimaryVTable(RE::VTABLE::BSTriShape, "BSTriShape");
		const auto property = PrimaryVTable(RE::VTABLE::BSLightingShaderProperty, "BSLightingShaderProperty");
		const auto lighting = PrimaryVTable(RE::VTABLE::BSLightingShader, "BSLightingShader");
		const auto utility = PrimaryVTable(RE::VTABLE::BSUtilityShader, "BSUtilityShader");
		// Optional: without it only forward-lit objects can be batched.
		const auto prePass = PrimaryVTable(RE::VTABLE::BSDFPrePassShader, "BSDFPrePassShader");
		const auto accumulator = PrimaryVTable(RE::VTABLE::BSShaderAccumulator, "BSShaderAccumulator");
		if (!triShape || !property || !lighting || !utility || !accumulator) {
			logger::critical("hooks: required vtables missing, plugin inactive (is Data/F4SE/Plugins/f4rd-runtime.bin installed?)");
			return false;
		}
		g_triShapeVTable = triShape;
		g_accumulatorVTable = accumulator;
		g_shaderVTables = { lighting, utility, prePass };

		LightingProperty::getRenderPasses = Patch(property, Slot::kGetRenderPasses, &LightingProperty::GetRenderPasses, "BSLightingShaderProperty::GetRenderPasses");
		LightingProperty::getRenderPassesShadowMapOrMask = Patch(property, Slot::kGetRenderPassesShadowMapOrMask, &LightingProperty::GetRenderPassesShadowMapOrMask, "BSLightingShaderProperty::GetRenderPasses_ShadowMapOrMask");
		LightingProperty::getRenderDepthPass = Patch(property, Slot::kGetRenderDepthPass, &LightingProperty::GetRenderDepthPass, "BSLightingShaderProperty::GetRenderDepthPass");

		Shader<ShaderKind::kLighting>::setupGeometry = Patch(lighting, Slot::kSetupGeometry, &Shader<ShaderKind::kLighting>::SetupGeometry, "BSLightingShader::SetupGeometry");
		Shader<ShaderKind::kLighting>::restoreGeometry = Patch(lighting, Slot::kRestoreGeometry, &Shader<ShaderKind::kLighting>::RestoreGeometry, "BSLightingShader::RestoreGeometry");
		Shader<ShaderKind::kUtility>::setupGeometry = Patch(utility, Slot::kSetupGeometry, &Shader<ShaderKind::kUtility>::SetupGeometry, "BSUtilityShader::SetupGeometry");
		Shader<ShaderKind::kUtility>::restoreGeometry = Patch(utility, Slot::kRestoreGeometry, &Shader<ShaderKind::kUtility>::RestoreGeometry, "BSUtilityShader::RestoreGeometry");
		if (prePass) {
			Shader<ShaderKind::kPrePass>::setupGeometry = Patch(prePass, Slot::kSetupGeometry, &Shader<ShaderKind::kPrePass>::SetupGeometry, "BSDFPrePassShader::SetupGeometry");
			Shader<ShaderKind::kPrePass>::restoreGeometry = Patch(prePass, Slot::kRestoreGeometry, &Shader<ShaderKind::kPrePass>::RestoreGeometry, "BSDFPrePassShader::RestoreGeometry");
		} else {
			logger::warn("hooks: BSDFPrePassShader not hooked; only objects drawn by BSLightingShader can be batched");
		}

		Accumulator::start = Patch(accumulator, Slot::kStartAccumulating, &Accumulator::StartAccumulating, "BSShaderAccumulator::StartAccumulating");
		Accumulator::finish[static_cast<std::size_t>(FinishKind::kFinish)] = Patch(accumulator, Slot::kFinishAccumulating, &Accumulator::Finish<FinishKind::kFinish>, "BSShaderAccumulator::FinishAccumulating");
		Accumulator::finish[static_cast<std::size_t>(FinishKind::kPreResolveDepth)] = Patch(accumulator, Slot::kFinishAccumulatingPreResolveDepth, &Accumulator::Finish<FinishKind::kPreResolveDepth>, "BSShaderAccumulator::FinishAccumulatingPreResolveDepth");
		Accumulator::finish[static_cast<std::size_t>(FinishKind::kPostResolveDepth)] = Patch(accumulator, Slot::kFinishAccumulatingPostResolveDepth, &Accumulator::Finish<FinishKind::kPostResolveDepth>, "BSShaderAccumulator::FinishAccumulatingPostResolveDepth");
		return true;
	}
}
