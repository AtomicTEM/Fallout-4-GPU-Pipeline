#include "Engine/EngineHooks.h"

#include "Core/Pipeline.h"

namespace GWP::EngineHooks
{
	namespace
	{
		// MSVC RTTICompleteObjectLocator: the primary vtable of a class has
		// offset 0; secondary vtables (other base subobjects) do not.
		[[nodiscard]] bool IsPrimaryVTable(std::uintptr_t a_vtable) noexcept
		{
			if (!a_vtable) {
				return false;
			}
			const auto locator = *reinterpret_cast<const std::uintptr_t*>(a_vtable - sizeof(std::uintptr_t));
			if (!locator) {
				return false;
			}
			const auto* const fields = reinterpret_cast<const std::uint32_t*>(locator);
			return fields[0] == 1 && fields[1] == 0;  // x64 signature, offset
		}

		// Resolves without REL::Relocation's fatal error path, so a missing or
		// outdated runtime database disables the plugin instead of the game.
		template <std::size_t N>
		[[nodiscard]] std::uintptr_t PrimaryVTable(const std::array<REL::ID, N>& a_ids, std::string_view a_name)
		{
			for (const auto& id : a_ids) {
				const auto result = REL::IDDatabase::get().resolve(id);
				if (!result) {
					logger::error("hooks: vtable of {} unresolved: {} {}", a_name, REL::id_resolve_status_text(result.status), result.note);
					continue;
				}
				const auto address = REL::Module::get().base() + *result.rva;
				if (IsPrimaryVTable(address)) {
					return address;
				}
			}
			logger::error("hooks: no primary vtable found for {}", a_name);
			return 0;
		}

		std::uintptr_t g_triShapeVTable{ 0 };

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
				return Pipeline::Get().OnGetRenderPasses(PassKind::kMain, getRenderPasses, a_this, a_geometry, a_mode, a_accumulator);
			}

			static RenderPassArray* GetRenderPassesShadowMapOrMask(RE::BSShaderProperty* a_this, RE::BSGeometry* a_geometry, std::uint32_t a_mode, RE::BSShaderAccumulator* a_accumulator)
			{
				return Pipeline::Get().OnGetRenderPasses(PassKind::kShadow, getRenderPassesShadowMapOrMask, a_this, a_geometry, a_mode, a_accumulator);
			}

			static RE::BSRenderPass* GetRenderDepthPass(RE::BSShaderProperty* a_this, RE::BSGeometry* a_geometry)
			{
				return Pipeline::Get().OnGetRenderDepthPass(getRenderDepthPass, a_this, a_geometry);
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
				Pipeline::Get().OnSetupGeometry(Kind, a_pass);
			}

			static void RestoreGeometry(RE::BSShader* a_this, RE::BSRenderPass* a_pass)
			{
				Pipeline::Get().OnRestoreGeometry(Kind, a_pass);
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
				Pipeline::Get().OnStartAccumulating(a_this);
				start(a_this, a_camera);
			}

			template <FinishKind Kind>
			static void Finish(RE::BSShaderAccumulator* a_this)
			{
				auto& pipeline = Pipeline::Get();
				pipeline.OnFinishBegin(a_this, Kind);
				finish[static_cast<std::size_t>(Kind)](a_this);
				pipeline.OnFinishEnd(a_this, Kind);
			}

			static inline Start_t start{ nullptr };
			static inline std::array<Finish_t, 3> finish{};
		};
	}

	std::uintptr_t TriShapeVTable() noexcept
	{
		return g_triShapeVTable;
	}

	bool Install()
	{
		const auto triShape = PrimaryVTable(RE::VTABLE::BSTriShape, "BSTriShape");
		const auto property = PrimaryVTable(RE::VTABLE::BSLightingShaderProperty, "BSLightingShaderProperty");
		const auto lighting = PrimaryVTable(RE::VTABLE::BSLightingShader, "BSLightingShader");
		const auto utility = PrimaryVTable(RE::VTABLE::BSUtilityShader, "BSUtilityShader");
		const auto accumulator = PrimaryVTable(RE::VTABLE::BSShaderAccumulator, "BSShaderAccumulator");
		if (!triShape || !property || !lighting || !utility || !accumulator) {
			logger::critical("hooks: required vtables missing, plugin inactive (is Data/F4SE/Plugins/f4rd-runtime.bin installed?)");
			return false;
		}
		g_triShapeVTable = triShape;

		LightingProperty::getRenderPasses = Patch(property, Slot::kGetRenderPasses, &LightingProperty::GetRenderPasses, "BSLightingShaderProperty::GetRenderPasses");
		LightingProperty::getRenderPassesShadowMapOrMask = Patch(property, Slot::kGetRenderPassesShadowMapOrMask, &LightingProperty::GetRenderPassesShadowMapOrMask, "BSLightingShaderProperty::GetRenderPasses_ShadowMapOrMask");
		LightingProperty::getRenderDepthPass = Patch(property, Slot::kGetRenderDepthPass, &LightingProperty::GetRenderDepthPass, "BSLightingShaderProperty::GetRenderDepthPass");

		Shader<ShaderKind::kLighting>::setupGeometry = Patch(lighting, Slot::kSetupGeometry, &Shader<ShaderKind::kLighting>::SetupGeometry, "BSLightingShader::SetupGeometry");
		Shader<ShaderKind::kLighting>::restoreGeometry = Patch(lighting, Slot::kRestoreGeometry, &Shader<ShaderKind::kLighting>::RestoreGeometry, "BSLightingShader::RestoreGeometry");
		Shader<ShaderKind::kUtility>::setupGeometry = Patch(utility, Slot::kSetupGeometry, &Shader<ShaderKind::kUtility>::SetupGeometry, "BSUtilityShader::SetupGeometry");
		Shader<ShaderKind::kUtility>::restoreGeometry = Patch(utility, Slot::kRestoreGeometry, &Shader<ShaderKind::kUtility>::RestoreGeometry, "BSUtilityShader::RestoreGeometry");

		Accumulator::start = Patch(accumulator, Slot::kStartAccumulating, &Accumulator::StartAccumulating, "BSShaderAccumulator::StartAccumulating");
		Accumulator::finish[static_cast<std::size_t>(FinishKind::kFinish)] = Patch(accumulator, Slot::kFinishAccumulating, &Accumulator::Finish<FinishKind::kFinish>, "BSShaderAccumulator::FinishAccumulating");
		Accumulator::finish[static_cast<std::size_t>(FinishKind::kPreResolveDepth)] = Patch(accumulator, Slot::kFinishAccumulatingPreResolveDepth, &Accumulator::Finish<FinishKind::kPreResolveDepth>, "BSShaderAccumulator::FinishAccumulatingPreResolveDepth");
		Accumulator::finish[static_cast<std::size_t>(FinishKind::kPostResolveDepth)] = Patch(accumulator, Slot::kFinishAccumulatingPostResolveDepth, &Accumulator::Finish<FinishKind::kPostResolveDepth>, "BSShaderAccumulator::FinishAccumulatingPostResolveDepth");
		return true;
	}
}
