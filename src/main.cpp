#include "Plugin.h"

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Load(const F4SE::LoadInterface* a_f4se)
{
	// OG, NG and AE all enter here; CommonLibF4RD selects runtime-specific IDs.
	return Plugin::Initialize(a_f4se);
}
