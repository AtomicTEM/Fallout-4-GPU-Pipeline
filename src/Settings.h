#pragma once

namespace GWP
{
	enum class PipelineMode : std::uint32_t
	{
		kObserve = 0,  // hooks only collect statistics, the game renders exactly as vanilla
		kBatch = 1     // eligible world geometry is drawn through GPU-driven batches
	};

	enum class IndirectMode : std::uint32_t
	{
		kAuto = 0,        // NVAPI multi-draw when available, otherwise index compaction
		kCompaction = 1,  // compute shader compacts visible index ranges, one indirect draw per batch
		kNvMultiDraw = 2, // NvAPI_D3D11_MultiDrawIndexedInstancedIndirect, one call per batch
		kDrawLoop = 3     // one DrawIndexedInstancedIndirect per member (debug / fallback)
	};

	struct Settings
	{
		// [General]
		bool enabled{ true };
		PipelineMode mode{ PipelineMode::kBatch };
		std::uint32_t toggleKey{ 0x79 };  // VK_F10
		std::uint32_t statsIntervalSeconds{ 10 };
		bool verboseLogging{ false };

		// [Batching]
		std::uint32_t minMembers{ 2 };
		std::uint32_t maxMembersPerBucket{ 512 };
		std::uint32_t maxVerticesPerBucket{ 262144 };
		float gridSize{ 4096.0F };
		std::uint32_t settleFrames{ 30 };
		std::uint32_t stableFrames{ 60 };
		std::uint32_t ingestVertexBudget{ 400000 };
		std::uint32_t maxPendingReferences{ 8192 };
		bool batchMainView{ true };
		bool batchShadows{ true };
		bool useCanMerge{ true };
		bool rotateBitangentW{ true };

		// [Culling]
		bool occlusionCulling{ true };
		float occlusionDepthBias{ 0.0005F };
		float cameraCutDistance{ 512.0F };
		IndirectMode indirectMode{ IndirectMode::kAuto };

		// [Memory]
		std::uint32_t arenaVertexMB{ 512 };
		std::uint32_t arenaIndexMB{ 192 };
		std::uint32_t ringIndexMB{ 64 };
		std::uint32_t stagingMB{ 32 };

		// [Calibration]
		std::uint32_t calibrationMinSamples{ 512 };
		std::uint32_t calibrationMinFrames{ 120 };

		// [Debug]
		std::string shaderDirectory;
		bool validateDraws{ true };

		static Settings& Get() noexcept
		{
			static Settings singleton;
			return singleton;
		}

		void Load();
		void Log() const;
	};
}
