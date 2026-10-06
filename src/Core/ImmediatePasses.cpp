#include "Core/ImmediatePasses.h"

#include "Engine/Layouts.h"
#include "Scene/BucketManager.h"
#include "Scene/ViewRegistry.h"

namespace GWP
{
	namespace
	{
		// Detaching is judged once this many passes were detached: at least this
		// share of them must have been drawn through SetupGeometry.
		inline constexpr std::uint64_t kCheckAfterDetached = 2000;
		inline constexpr std::uint64_t kMinDrawnPercent = 5;
		// New buffers recorded for detached passes leak the detached ones.
		inline constexpr std::uint64_t kMaxRebuilt = 64;

		[[nodiscard]] std::atomic_ref<std::byte*> CommandBuffer(void* a_pass) noexcept
		{
			return std::atomic_ref<std::byte*>{ Engine::Field<std::byte*>(a_pass, Engine::Offsets::kPassCommandBuffer) };
		}

		[[nodiscard]] const RE::BSGeometry* GeometryOf(const void* a_pass) noexcept
		{
			return Engine::Field<const RE::BSGeometry*>(a_pass, Engine::Offsets::kPassGeometry);
		}
	}

	bool ImmediatePasses::Detach(void* a_pass, const RE::BSGeometry* a_geometry, Reason a_reason, Bucket* a_bucket, const ViewState* a_view, std::uint32_t a_maxPasses)
	{
		const bool enabled = Enabled();
		bool replays = false;
		void* pass = a_pass;
		for (std::uint32_t i = 0; pass && i < a_maxPasses && GeometryOf(pass) == a_geometry; ++i) {
			auto field = CommandBuffer(pass);
			if (field.load(std::memory_order_relaxed)) {
				if (!enabled) {
					replays = true;
				} else if (auto* const buffer = field.exchange(nullptr, std::memory_order_acq_rel)) {
					// Another view may have detached it first this frame; only the
					// thread that took the buffer owns putting it back.
					std::scoped_lock lock{ _lock };
					_index[pass] = _entries.size();
					_entries.push_back({ pass, buffer, a_geometry, a_reason == Reason::kAnchor ? a_bucket : nullptr, a_view, false });
					_count.store(_entries.size(), std::memory_order_release);
					++(a_reason == Reason::kAnchor ? _stats.anchors : _stats.captures);
				}
			}
			pass = Engine::Field<void*>(pass, Engine::Offsets::kPassPropertyNext);
		}
		return !replays;
	}

	void ImmediatePasses::NoteSetup(const void* a_pass)
	{
		if (_count.load(std::memory_order_acquire) == 0) {
			return;
		}
		std::scoped_lock lock{ _lock };
		if (const auto it = _index.find(a_pass); it != _index.end()) {
			_entries[it->second].drawn = true;
		}
	}

	void ImmediatePasses::ReattachAll()
	{
		if (_count.load(std::memory_order_acquire) == 0) {
			return;
		}
		{
			std::scoped_lock lock{ _lock };
			_reattaching.swap(_entries);
			_index.clear();
			_count.store(0, std::memory_order_release);
		}

		std::uint64_t drawn = 0;
		std::uint64_t undrawnAnchors = 0;
		std::uint64_t rebuilt = 0;
		std::uint64_t lost = 0;
		for (const auto& entry : _reattaching) {
			// The engine keeps a pass alive while it is registered, and the frame
			// that registered it ends here; still, never touch a pass that no
			// longer belongs to the geometry it was taken from.
			if (GeometryOf(entry.pass) != entry.geometry) {
				++lost;
				continue;
			}
			std::byte* expected = nullptr;
			if (!CommandBuffer(entry.pass).compare_exchange_strong(expected, entry.buffer, std::memory_order_acq_rel)) {
				++rebuilt;  // keep the engine's new buffer; the detached one is abandoned
				continue;
			}
			if (entry.drawn) {
				++drawn;
			} else if (entry.bucket && entry.view && entry.view->finishes.load(std::memory_order_relaxed) != 0) {
				// The view was rendered, the anchor's pass was not: neither were
				// the members it suppressed there.
				entry.bucket->drawAnomalies.fetch_add(1, std::memory_order_relaxed);
				++undrawnAnchors;
			}
		}
		const auto detached = static_cast<std::uint64_t>(_reattaching.size());
		_reattaching.clear();

		{
			std::scoped_lock lock{ _lock };
			_stats.drawn += drawn;
			_stats.undrawnAnchors += undrawnAnchors;
			_stats.rebuilt += rebuilt;
			_stats.lost += lost;
		}

		_totalDetached += detached;
		_totalDrawn += drawn;
		_totalRebuilt += rebuilt;
		if (!Enabled()) {
			return;
		}
		if (_totalRebuilt > kMaxRebuilt) {
			Disable("the engine records new command buffers for passes whose buffer was detached");
		} else if (_totalDetached >= kCheckAfterDetached && _totalDrawn * 100 < _totalDetached * kMinDrawnPercent) {
			Disable(fmt::format("only {} of {} passes without a command buffer were drawn through SetupGeometry", _totalDrawn, _totalDetached));
		}
	}

	void ImmediatePasses::Disable(std::string_view a_reason)
	{
		if (_enabled.exchange(false, std::memory_order_relaxed)) {
			logger::warn("command buffers: objects drawn from command buffers are no longer batched: {}", a_reason);
		}
	}

	bool ImmediatePasses::ReserveCapture(std::uint32_t a_frame, std::uint32_t a_limit) noexcept
	{
		auto current = _budget.load(std::memory_order_relaxed);
		for (;;) {
			const auto used = static_cast<std::uint32_t>(current >> 32) == a_frame ? static_cast<std::uint32_t>(current) : 0u;
			if (used >= a_limit) {
				return false;
			}
			const auto next = (std::uint64_t{ a_frame } << 32) | (used + 1);
			if (_budget.compare_exchange_weak(current, next, std::memory_order_relaxed)) {
				return true;
			}
		}
	}

	ImmediatePasses::Stats ImmediatePasses::TakeStats() noexcept
	{
		std::scoped_lock lock{ _lock };
		return std::exchange(_stats, Stats{});
	}
}
