#pragma once

#include <thread>

namespace GWP
{
	// Counter incremented from many renderer job threads; striping keeps the
	// threads from fighting over one cache line.
	class StripedCounter
	{
	public:
		void Add(std::uint64_t a_value = 1) noexcept
		{
			_stripes[StripeIndex()].value.fetch_add(a_value, std::memory_order_relaxed);
		}

		[[nodiscard]] std::uint64_t Take() noexcept
		{
			std::uint64_t total = 0;
			for (auto& stripe : _stripes) {
				total += stripe.value.exchange(0, std::memory_order_relaxed);
			}
			return total;
		}

	private:
		static constexpr std::size_t kStripes = 16;

		struct alignas(64) Stripe
		{
			std::atomic<std::uint64_t> value{ 0 };
		};

		[[nodiscard]] static std::size_t StripeIndex() noexcept
		{
			thread_local const std::size_t index = std::hash<std::thread::id>{}(std::this_thread::get_id()) % kStripes;
			return index;
		}

		std::array<Stripe, kStripes> _stripes{};
	};
}
