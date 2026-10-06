#pragma once

namespace GWP
{
	inline constexpr std::uint32_t kMaxViews = 64;

	// One accumulation of one BSShaderAccumulator (main view, a shadow cascade,
	// a reflection, ...). The engine culls the scene into an accumulator and
	// then renders it inside FinishAccumulating; every batched member the
	// engine registers in between is appended to the view's list, and the
	// first batch draw of the view turns that list into GPU work.
	struct ViewState
	{
		struct Storage
		{
			std::unique_ptr<std::uint32_t[]> items;
			std::uint32_t capacity{ 0 };
		};

		std::atomic<const RE::BSShaderAccumulator*> accumulator{ nullptr };
		std::atomic<std::uint32_t> epoch{ 0 };  // 0: never started
		std::atomic<std::uint32_t> count{ 0 };
		std::atomic<Storage*> storage{ nullptr };
		std::atomic<bool> overflowed{ false };

		// calibration counters (per frame, reset at Present)
		std::atomic<std::uint32_t> starts{ 0 };
		std::atomic<std::uint32_t> finishes{ 0 };
		std::atomic<std::uint32_t> registrationsBeforeStart{ 0 };

		// world view support (see Pipeline::PrepareWorldView)
		std::atomic<bool> hookedStart{ false };              // StartAccumulating reached the vtable hook
		std::atomic<bool> firstPerson{ false };              // BSShaderAccumulator::firstPerson
		std::atomic<std::uint32_t> mainRegistrations{ 0 };  // per frame, reset at Present
		std::atomic<std::uint32_t> drawnEpoch{ 0 };         // epoch whose first batch draw happened
		std::uint32_t lightingSetups{ 0 };       // render thread
		std::uint32_t utilitySetups{ 0 };        // render thread

		// render thread
		std::uint32_t preparedEpoch{ 0 };        // epoch whose GPU work was dispatched
		bool occlusionThisEpoch{ false };
	};

	class ViewRegistry
	{
	public:
		// Returns the slot for an accumulator, creating it if there is room.
		[[nodiscard]] ViewState* Acquire(const RE::BSShaderAccumulator* a_accumulator) noexcept;
		[[nodiscard]] ViewState* Find(const RE::BSShaderAccumulator* a_accumulator) const noexcept;
		[[nodiscard]] std::uint32_t IndexOf(const ViewState* a_view) const noexcept { return static_cast<std::uint32_t>(a_view - _views.data()); }
		[[nodiscard]] ViewState& At(std::uint32_t a_index) noexcept { return _views[a_index]; }

		// StartAccumulating: opens a new epoch with room for a_capacity members.
		std::uint32_t Begin(ViewState& a_view, std::uint32_t a_capacity, std::uint32_t a_frame);

		// Worker threads: appends a member to the open epoch. Fails when full.
		[[nodiscard]] bool Append(ViewState& a_view, std::uint32_t a_member) noexcept;

		[[nodiscard]] std::span<const std::uint32_t> Items(const ViewState& a_view) const noexcept;

		// Render thread, at Present: frees storage retired at least 8 frames ago.
		void Collect(std::uint32_t a_frame);

		template <class F>
		void ForEach(F&& a_func)
		{
			for (auto& view : _views) {
				if (view.accumulator.load(std::memory_order_acquire)) {
					a_func(view);
				}
			}
		}

	private:
		std::array<ViewState, kMaxViews> _views;
		std::atomic<std::uint32_t> _nextEpoch{ 1 };

		std::mutex _retiredLock;
		std::deque<std::pair<std::uint32_t, std::unique_ptr<ViewState::Storage>>> _retired;
	};
}
