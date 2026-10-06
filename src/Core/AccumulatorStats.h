#pragma once

#include "Engine/Layouts.h"

namespace GWP
{
	// Diagnostics per BSShaderAccumulator: which accumulators register eligible
	// objects, whether their StartAccumulating/FinishAccumulating calls reach
	// the vtable hooks, and where the passes they queue are drawn. Batching
	// depends on a pass being drawn inside the hooked FinishAccumulating of the
	// accumulator that queued it. Counters are reset with each stats line.
	class AccumulatorStats
	{
	public:
		static constexpr std::uint8_t kNone = 0xFF;
		static constexpr std::size_t kSlots = 64;

		struct Slot
		{
			std::atomic<const RE::BSShaderAccumulator*> accumulator{ nullptr };
			std::atomic<std::uintptr_t> vtable{ 0 };

			std::atomic<std::uint32_t> starts{ 0 };
			std::array<std::atomic<std::uint32_t>, 3> finishes{};       // FinishKind
			std::array<std::atomic<std::uint32_t>, 2> registrations{};  // PassKind: main, shadow
			std::atomic<std::uint32_t> withoutEpoch{ 0 };               // registered before any hooked StartAccumulating
			std::array<std::atomic<std::uint64_t>, 2> modes{};          // render modes (< 64) seen, per PassKind
			std::atomic<std::uint32_t> carriers{ 0 };
			std::atomic<std::uint32_t> suppressed{ 0 };

			// SetupGeometry/draws inside this accumulator's hooked FinishAccumulating
			std::atomic<std::uint32_t> surfaceSetups{ 0 };
			std::atomic<std::uint32_t> utilitySetups{ 0 };
			std::atomic<std::uint32_t> replays{ 0 };
			// SetupGeometry of objects last queued here, drawn outside every hooked
			// FinishAccumulating / inside another accumulator's
			std::atomic<std::uint32_t> drawnOutside{ 0 };
			std::atomic<std::uint32_t> drawnElsewhere{ 0 };
		};

		// Any thread. kNone when the table is full or a_accumulator is null.
		[[nodiscard]] std::uint8_t IndexOf(const RE::BSShaderAccumulator* a_accumulator) noexcept
		{
			if (!a_accumulator) {
				return kNone;
			}
			for (std::size_t i = 0; i < kSlots; ++i) {
				auto& slot = _slots[i];
				const auto* current = slot.accumulator.load(std::memory_order_acquire);
				if (current == a_accumulator) {
					return static_cast<std::uint8_t>(i);
				}
				if (!current) {
					const RE::BSShaderAccumulator* expected = nullptr;
					if (slot.accumulator.compare_exchange_strong(expected, a_accumulator, std::memory_order_acq_rel)) {
						slot.vtable.store(Engine::VTableOf(a_accumulator), std::memory_order_relaxed);
						return static_cast<std::uint8_t>(i);
					}
					if (expected == a_accumulator) {
						return static_cast<std::uint8_t>(i);
					}
				}
			}
			_untracked.fetch_add(1, std::memory_order_relaxed);
			return kNone;
		}

		// Calls for accumulators that found no free slot since the last call.
		[[nodiscard]] std::uint64_t TakeUntracked() noexcept
		{
			return _untracked.exchange(0, std::memory_order_relaxed);
		}

		[[nodiscard]] Slot* At(std::uint8_t a_index) noexcept
		{
			return a_index < kSlots ? &_slots[a_index] : nullptr;
		}

		template <class F>
		void ForEach(F&& a_func)
		{
			for (std::size_t i = 0; i < kSlots; ++i) {
				if (_slots[i].accumulator.load(std::memory_order_acquire)) {
					a_func(static_cast<std::uint8_t>(i), _slots[i]);
				}
			}
		}

	private:
		std::array<Slot, kSlots> _slots;
		std::atomic<std::uint64_t> _untracked{ 0 };
	};
}
