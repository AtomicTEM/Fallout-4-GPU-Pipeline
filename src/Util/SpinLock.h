#pragma once

namespace GWP
{
	// Tiny test-and-test-and-set lock for rarely contended per-object state.
	class SpinLock
	{
	public:
		void lock() noexcept
		{
			for (;;) {
				if (!_flag.exchange(true, std::memory_order_acquire)) {
					return;
				}
				while (_flag.load(std::memory_order_relaxed)) {
					::YieldProcessor();
				}
			}
		}

		[[nodiscard]] bool try_lock() noexcept
		{
			return !_flag.load(std::memory_order_relaxed) && !_flag.exchange(true, std::memory_order_acquire);
		}

		void unlock() noexcept
		{
			_flag.store(false, std::memory_order_release);
		}

	private:
		std::atomic_bool _flag{ false };
	};
}
