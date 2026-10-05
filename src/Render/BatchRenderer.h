#pragma once

#include "Render/GpuBuffers.h"
#include "Settings.h"

namespace GWP
{
	class BucketManager;
	class HiZ;
	class ShaderLibrary;
	struct Bucket;

	struct OcclusionInput
	{
		const HiZ* hiz{ nullptr };
		float depthBias{ 0.0F };
	};

	struct BatchStats
	{
		std::uint32_t views{ 0 };
		std::uint32_t workItems{ 0 };
		std::uint32_t batchDraws{ 0 };
		std::uint32_t multiDrawCalls{ 0 };
		std::uint32_t loopDraws{ 0 };
		std::uint32_t fallbackViews{ 0 };
		std::uint32_t layoutFailures{ 0 };
		std::uint32_t occlusionViews{ 0 };
	};

	// Turns the engine's per-view registrations into GPU work and replaces each
	// anchor draw with an indirect draw of its batch.
	class BatchRenderer
	{
	public:
		bool Initialize(ID3D11Device* a_device, ShaderLibrary& a_shaders, BucketManager& a_buckets);

		void BeginFrame();

		// Dispatches the culling pass for one view epoch. a_members are the
		// member ids the engine registered for that view.
		bool PrepareView(ID3D11DeviceContext* a_context, std::span<const std::uint32_t> a_members, std::uint32_t a_epoch, const OcclusionInput* a_occlusion);

		// Issues the batch draw in place of the anchor's draw. Returns false when
		// the batch cannot be drawn with the currently bound state.
		bool DrawBucket(ID3D11DeviceContext* a_context, Bucket& a_bucket, std::uint32_t a_epoch);

		[[nodiscard]] IndirectMode ActiveMode() const noexcept { return _mode; }
		[[nodiscard]] const BatchStats& Stats() const noexcept { return _stats; }
		void ResetStats() noexcept { _stats = {}; }

	private:
		enum class ViewMode
		{
			kCompaction,
			kPerMember
		};

		ID3D11Device* _device{ nullptr };
		BucketManager* _buckets{ nullptr };
		IndirectMode _mode{ IndirectMode::kCompaction };

		Microsoft::WRL::ComPtr<ID3D11ComputeShader> _compact;
		Microsoft::WRL::ComPtr<ID3D11ComputeShader> _multiDraw;
		Microsoft::WRL::ComPtr<ID3D11Buffer> _constants;

		DynamicBuffer _work;
		ArenaBuffer _args;
		ArenaBuffer _ring;
		std::uint32_t _argsCapacity{ 0 };   // records
		std::uint32_t _argsCursor{ 0 };     // records
		std::uint64_t _ringCapacity{ 0 };   // indices
		std::uint64_t _ringCursor{ 0 };     // indices

		// scratch, reused across views
		std::vector<Bucket*> _viewBuckets;
		std::vector<std::uint32_t> _workItems;  // pairs (member, slot)
		std::vector<std::uint32_t> _argsInit;

		std::uint32_t _currentEpoch{ 0 };
		ViewMode _currentMode{ ViewMode::kCompaction };

		BatchStats _stats;
	};
}
