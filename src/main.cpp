#include "Core/Guard.h"
#include "Plugin.h"

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Load(const F4SE::LoadInterface* a_f4se)
{
	// OG, NG and AE all enter here; CommonLibF4RD selects runtime-specific IDs.
	// No exception may reach F4SE. A failed load makes F4SE unload the DLL,
	// which is unsafe once any hook is installed, so an exception disables
	// the plugin and keeps it loaded instead.
	try {
		return Plugin::Initialize(a_f4se);
	} catch (const std::exception& e) {
		GWP::ReportFault("plugin load", e.what());
	} catch (...) {
		GWP::ReportFault("plugin load", "unknown exception");
	}
	return true;
}
