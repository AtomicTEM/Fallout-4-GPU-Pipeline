#include "Core/Guard.h"

namespace GWP
{
	namespace
	{
		std::atomic_bool g_faulted{ false };
	}

	void ReportFault(std::string_view a_where, std::string_view a_what) noexcept
	{
		if (g_faulted.exchange(true)) {
			return;
		}
		try {
			logger::critical("pipeline: exception in {}: {}. The plugin is disabled for this session and the game continues unmodified.", a_where, a_what);
		} catch (...) {
		}
	}

	bool Faulted() noexcept
	{
		return g_faulted.load(std::memory_order_relaxed);
	}
}
