#include "Settings.h"

namespace GWP
{
	namespace
	{
		constexpr auto kPath = "Data\\F4SE\\Plugins\\GPUWorldPipeline.ini";

		std::uint32_t ReadUInt(const char* a_section, const char* a_key, std::uint32_t a_default)
		{
			return ::GetPrivateProfileIntA(a_section, a_key, static_cast<INT>(a_default), kPath);
		}

		bool ReadBool(const char* a_section, const char* a_key, bool a_default)
		{
			return ReadUInt(a_section, a_key, a_default ? 1u : 0u) != 0;
		}

		std::string ReadString(const char* a_section, const char* a_key, const std::string& a_default)
		{
			char buffer[MAX_PATH]{};
			::GetPrivateProfileStringA(a_section, a_key, a_default.c_str(), buffer, static_cast<DWORD>(std::size(buffer)), kPath);
			return buffer;
		}

		float ReadFloat(const char* a_section, const char* a_key, float a_default)
		{
			const auto text = ReadString(a_section, a_key, fmt::format("{}", a_default));
			try {
				return std::stof(text);
			} catch (...) {
				logger::warn("settings: [{}] {}={} is not a number, using {}", a_section, a_key, text, a_default);
				return a_default;
			}
		}
	}

	void Settings::Load()
	{
		enabled = ReadBool("General", "bEnabled", enabled);
		mode = static_cast<PipelineMode>(std::min<std::uint32_t>(ReadUInt("General", "iMode", static_cast<std::uint32_t>(mode)), 1));
		toggleKey = ReadUInt("General", "iToggleKey", toggleKey);
		statsIntervalSeconds = ReadUInt("General", "iStatsIntervalSeconds", statsIntervalSeconds);
		verboseLogging = ReadBool("General", "bVerboseLogging", verboseLogging);

		minMembers = std::max<std::uint32_t>(ReadUInt("Batching", "iMinMembers", minMembers), 2);
		maxMembersPerBucket = std::clamp<std::uint32_t>(ReadUInt("Batching", "iMaxMembersPerBucket", maxMembersPerBucket), 2, 65535);
		maxVerticesPerBucket = std::clamp<std::uint32_t>(ReadUInt("Batching", "iMaxVerticesPerBucket", maxVerticesPerBucket), 1024, 1u << 24);
		gridSize = std::clamp(ReadFloat("Batching", "fGridSize", gridSize), 512.0F, 16384.0F);
		settleFrames = ReadUInt("Batching", "iSettleFrames", settleFrames);
		stableFrames = ReadUInt("Batching", "iStableFrames", stableFrames);
		ingestVertexBudget = std::max<std::uint32_t>(ReadUInt("Batching", "iIngestVertexBudget", ingestVertexBudget), 1024);
		maxPendingReferences = ReadUInt("Batching", "iMaxPendingReferences", maxPendingReferences);
		batchMainView = ReadBool("Batching", "bBatchMainView", batchMainView);
		batchShadows = ReadBool("Batching", "bBatchShadows", batchShadows);
		useCanMerge = ReadBool("Batching", "bUseCanMerge", useCanMerge);
		rotateBitangentW = ReadBool("Batching", "bRotateBitangentW", rotateBitangentW);
		batchCommandBufferObjects = ReadBool("Batching", "bBatchCommandBufferObjects", batchCommandBufferObjects);

		occlusionCulling = ReadBool("Culling", "bOcclusionCulling", occlusionCulling);
		occlusionDepthBias = std::clamp(ReadFloat("Culling", "fOcclusionDepthBias", occlusionDepthBias), 0.0F, 0.1F);
		cameraCutDistance = ReadFloat("Culling", "fCameraCutDistance", cameraCutDistance);
		indirectMode = static_cast<IndirectMode>(std::min<std::uint32_t>(ReadUInt("Culling", "iIndirectMode", static_cast<std::uint32_t>(indirectMode)), 3));

		arenaVertexMB = std::clamp<std::uint32_t>(ReadUInt("Memory", "iArenaVertexMB", arenaVertexMB), 16, 2048);
		arenaIndexMB = std::clamp<std::uint32_t>(ReadUInt("Memory", "iArenaIndexMB", arenaIndexMB), 8, 1024);
		ringIndexMB = std::clamp<std::uint32_t>(ReadUInt("Memory", "iRingIndexMB", ringIndexMB), 4, 1024);
		stagingMB = std::clamp<std::uint32_t>(ReadUInt("Memory", "iStagingMB", stagingMB), 4, 256);

		calibrationMinSamples = ReadUInt("Calibration", "iMinSamples", calibrationMinSamples);
		calibrationMinFrames = ReadUInt("Calibration", "iMinFrames", calibrationMinFrames);

		shaderDirectory = ReadString("Debug", "sShaderDirectory", shaderDirectory);
		validateDraws = ReadBool("Debug", "bValidateDraws", validateDraws);
	}

	void Settings::Log() const
	{
		logger::info("settings: enabled={} mode={} toggleKey=0x{:X}", enabled, mode == PipelineMode::kBatch ? "batch"sv : "observe"sv, toggleKey);
		logger::info("settings: batching minMembers={} maxMembers={} maxVertices={} grid={} settle={} stable={} budget={}",
			minMembers, maxMembersPerBucket, maxVerticesPerBucket, gridSize, settleFrames, stableFrames, ingestVertexBudget);
		logger::info("settings: mainView={} shadows={} canMerge={} rotateBitangentW={} commandBufferObjects={}", batchMainView, batchShadows, useCanMerge, rotateBitangentW, batchCommandBufferObjects);
		logger::info("settings: occlusion={} bias={} cameraCut={} indirectMode={}", occlusionCulling, occlusionDepthBias, cameraCutDistance, static_cast<std::uint32_t>(indirectMode));
		logger::info("settings: arena VB={}MB IB={}MB ring={}MB staging={}MB", arenaVertexMB, arenaIndexMB, ringIndexMB, stagingMB);
	}
}
