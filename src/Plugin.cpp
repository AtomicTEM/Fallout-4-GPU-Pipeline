#include "Plugin.h"

#include "Core/Pipeline.h"
#include "Settings.h"

namespace
{
	[[nodiscard]] constexpr std::uint32_t PackVersion(
		std::uint32_t a_major,
		std::uint32_t a_minor,
		std::uint32_t a_build,
		std::uint32_t a_sub = 0) noexcept
	{
		return ((a_major & 0xFF) << 24) |
		       ((a_minor & 0xFF) << 16) |
		       ((a_build & 0xFFF) << 4) |
		       (a_sub & 0xF);
	}

	[[nodiscard]] constexpr F4SE::PluginVersionData MakePluginVersionData() noexcept
	{
		F4SE::PluginVersionData data{};
		data.pluginVersion = PackVersion(
			static_cast<std::uint32_t>(Version::MAJOR),
			static_cast<std::uint32_t>(Version::MINOR),
			static_cast<std::uint32_t>(Version::PATCH));

		for (std::size_t i = 0; i < Version::PROJECT.size() && i < std::size(data.name) - 1; ++i) {
			data.name[i] = Version::PROJECT[i];
		}

		// All engine addresses come from the CommonLibF4RD runtime database
		// (vtable IDs) or from COM vtables, never from fixed executable offsets.
		data.addressIndependence = F4SE::PluginVersionData::kAddressIndependence_Signatures;

		// The few raw field offsets the plugin reads are the NG/AE layout; they
		// are verified at runtime before batching is switched on.
		data.structureIndependence =
			F4SE::PluginVersionData::kStructureIndependence_1_10_980Layout |
			F4SE::PluginVersionData::kStructureIndependence_1_11_137Layout;
		return data;
	}

	[[nodiscard]] bool InitializeLogger()
	{
#ifndef NDEBUG
		auto sink = std::make_shared<spdlog::sinks::msvc_sink_mt>();
#else
		auto path = logger::log_directory();
		if (!path) {
			return false;
		}

		*path /= fmt::format(FMT_STRING("{}.log"), Version::PROJECT);
		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
#endif

		auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));

#ifndef NDEBUG
		log->set_level(spdlog::level::trace);
#else
		log->set_level(spdlog::level::info);
		log->flush_on(spdlog::level::info);
#endif

		spdlog::set_default_logger(std::move(log));
		spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] %v"s);
		return true;
	}

	void F4SEAPI OnF4SEMessage(F4SE::MessagingInterface::Message* a_message)
	{
		if (a_message && a_message->type == F4SE::MessagingInterface::kGameDataReady) {
			GWP::Pipeline::Get().InitializeRenderer();
		}
	}
}

extern "C" DLLEXPORT constinit F4SE::PluginVersionData F4SEPlugin_Version = MakePluginVersionData();

bool Plugin::Initialize(const F4SE::LoadInterface* a_f4se)
{
	if (!InitializeLogger()) {
		return false;
	}

	if (a_f4se->IsEditor()) {
		logger::critical("loaded in editor");
		return false;
	}

	F4SE::Init(a_f4se);
	logger::info("{} v{} loaded on runtime {}", Version::PROJECT, Version::NAME, a_f4se->RuntimeVersion().string());

	auto& settings = GWP::Settings::Get();
	settings.Load();
	settings.Log();
	if (settings.verboseLogging) {
		spdlog::default_logger()->set_level(spdlog::level::debug);
	}

	if (!settings.enabled) {
		logger::info("disabled in GPUWorldPipeline.ini");
		return true;
	}

	if (!GWP::Pipeline::Get().InstallEarlyHooks()) {
		logger::error("hooks could not be installed; the game runs unmodified");
		return true;
	}

	if (const auto* messaging = F4SE::GetMessagingInterface(); !messaging || !messaging->RegisterListener(OnF4SEMessage)) {
		logger::warn("F4SE messaging unavailable; renderer initialisation will happen on the first frames");
	}
	return true;
}
