#include "Core/CarriedPasses.h"

#include "Engine/EngineHooks.h"
#include "Engine/Layouts.h"
#include "Scene/BucketManager.h"

namespace GWP
{
	void CarriedPasses::Add(void* a_pass, const RE::BSGeometry* a_geometry, const Carried& a_carried, std::uint32_t a_maxPasses)
	{
		// Only passes of the hooked shaders are drawn through SetupGeometry;
		// the list also holds passes that are never drawn in this view.
		const auto& shaders = EngineHooks::HookedShaderVTables();
		std::scoped_lock lock{ _lock };
		void* pass = a_pass;
		for (std::uint32_t i = 0; pass && i < a_maxPasses && Engine::Field<const RE::BSGeometry*>(pass, Engine::Offsets::kPassGeometry) == a_geometry; ++i) {
			const auto shader = Engine::VTableOf(Engine::Field<const void*>(pass, Engine::Offsets::kPassShader));
			if (shader && (shader == shaders.prePass || shader == shaders.lighting || shader == shaders.utility)) {
				_entries.insert_or_assign(pass, Entry{ a_carried, false });
				++_stats.carried;
			}
			pass = Engine::Field<void*>(pass, Engine::Offsets::kPassPropertyNext);
		}
		_count.store(_entries.size(), std::memory_order_release);
	}

	std::optional<CarriedPasses::Carried> CarriedPasses::Find(const void* a_pass)
	{
		if (!a_pass || _count.load(std::memory_order_acquire) == 0) {
			return std::nullopt;
		}
		std::scoped_lock lock{ _lock };
		const auto it = _entries.find(a_pass);
		if (it == _entries.end()) {
			return std::nullopt;
		}
		if (!it->second.drawn) {
			it->second.drawn = true;
			++_stats.drawn;
			++_drawnThisFrame;
		}
		return it->second.carried;
	}

	void CarriedPasses::EndFrame()
	{
		if (_count.load(std::memory_order_acquire) == 0) {
			return;
		}
		std::scoped_lock lock{ _lock };
		// Nothing drawn at all: the view was not rendered this frame.
		if (_drawnThisFrame != 0) {
			for (const auto& [pass, entry] : _entries) {
				if (!entry.drawn && entry.carried.bucket) {
					entry.carried.bucket->drawAnomalies.fetch_add(1, std::memory_order_relaxed);
					++_stats.undrawn;
				}
			}
		}
		_drawnThisFrame = 0;
		_entries.clear();
		_count.store(0, std::memory_order_release);
	}

	CarriedPasses::Stats CarriedPasses::TakeStats() noexcept
	{
		std::scoped_lock lock{ _lock };
		return std::exchange(_stats, Stats{});
	}
}
