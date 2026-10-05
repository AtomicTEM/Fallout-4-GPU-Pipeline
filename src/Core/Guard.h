#pragma once

namespace GWP
{
	// Hooks run inside game and driver code, so no exception may unwind out of
	// them. The first exception disables the plugin for the rest of the session
	// and every hook falls back to the original function, which leaves the game
	// rendering as vanilla.
	void ReportFault(std::string_view a_where, std::string_view a_what) noexcept;

	[[nodiscard]] bool Faulted() noexcept;

	// Runs a_body, or a_fallback once the plugin has faulted (or when a_body
	// throws). Both must return the same type.
	template <class Fallback, class Body>
	auto Guarded(std::string_view a_where, Fallback&& a_fallback, Body&& a_body) noexcept -> decltype(a_fallback())
	{
		if (!Faulted()) {
			try {
				return a_body();
			} catch (const std::exception& e) {
				ReportFault(a_where, e.what());
			} catch (...) {
				ReportFault(a_where, "unknown exception");
			}
		}
		return a_fallback();
	}

	// For hooks whose original has already run: there is nothing to fall back to.
	template <class Body>
	void Guarded(std::string_view a_where, Body&& a_body) noexcept
	{
		Guarded(a_where, [] {}, std::forward<Body>(a_body));
	}
}
