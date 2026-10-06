#pragma once

#include "Util/SpinLock.h"

namespace GWP
{
	struct Bucket;
	struct ViewState;

	// The anchor passes a carrier returned this frame in a view whose
	// accumulator never reaches the FinishAccumulating hooks (the main view:
	// the world accumulator draws its G-buffer passes inside
	// DrawWorld::DeferredPrePass, not through the vtable). There is no hooked
	// draw window to tell which view such a draw belongs to, so the pass itself
	// identifies it: the draw of a recorded pass is the draw of its batch in
	// the view and epoch it was carried for.
	class CarriedPasses
	{
	public:
		struct Carried
		{
			ViewState* view{ nullptr };
			std::uint32_t epoch{ 0 };
			Bucket* bucket{ nullptr };
		};

		struct Stats
		{
			std::uint64_t carried{ 0 };  // anchor passes recorded
			std::uint64_t drawn{ 0 };    // ... whose draw reached the draw hook
			std::uint64_t undrawn{ 0 };  // ... never drawn before Present (counted as batch anomalies)
		};

		// Any thread, from the carrier's registration. Records at most
		// a_maxPasses passes of a_geometry chained from a_pass.
		void Add(void* a_pass, const RE::BSGeometry* a_geometry, const Carried& a_carried, std::uint32_t a_maxPasses);

		// Render thread, from the draw hook: the view a_pass was carried for,
		// if it was; marks it drawn.
		[[nodiscard]] std::optional<Carried> Find(const void* a_pass);

		// Render thread, at Present: counts passes that were never drawn as
		// anomalies of their batch (their members were suppressed for nothing),
		// unless nothing carried was drawn at all, and forgets the frame.
		void EndFrame();

		[[nodiscard]] Stats TakeStats() noexcept;

	private:
		struct Entry
		{
			Carried carried;
			bool drawn{ false };
		};

		SpinLock _lock;
		std::unordered_map<const void*, Entry> _entries;  // _lock
		std::atomic<std::size_t> _count{ 0 };
		std::uint64_t _drawnThisFrame{ 0 };  // _lock
		Stats _stats;                         // _lock
	};
}
