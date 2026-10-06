#pragma once

#include "Engine/Layouts.h"
#include "Util/SpinLock.h"

namespace GWP
{
	inline constexpr std::uint32_t kInvalidIndex = 0xFFFFFFFFu;

	enum class ObjectState : std::uint8_t
	{
		kTracking,   // seen by the renderer, waiting for its transform to stay stable
		kCandidate,  // stable; waiting for a main-view draw to capture its buffers
		kCaptured,   // buffers captured, waiting in a pending group to become a member
		kMember,     // merged into an active bucket; the engine no longer draws it individually
		kRejected,   // cannot be batched (unsupported layout, format, ...)
		kEvicted     // changed while it was a member; reset during maintenance
	};

	// Everything needed to copy a geometry's vertices/indices out of the
	// engine's buffers, taken from the D3D11 state of a real draw of it.
	struct DrawCapture
	{
		Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer;
		Microsoft::WRL::ComPtr<ID3D11Buffer> indexBuffer;
		ID3D11InputLayout* layout{ nullptr };  // identity only, owned by the engine
		UINT vertexOffset{ 0 };
		UINT stride{ 0 };
		UINT indexOffset{ 0 };
		DXGI_FORMAT indexFormat{ DXGI_FORMAT_UNKNOWN };
		UINT indexCount{ 0 };
		UINT startIndex{ 0 };
		INT baseVertex{ 0 };
	};

	struct ObjectRecord
	{
		// identity
		RE::BSGeometry* geometry{ nullptr };
		void* rendererData{ nullptr };
		RE::BSShaderProperty* property{ nullptr };
		std::uint64_t vertexDesc{ 0 };

		// snapshot (written under lock while tracking, immutable afterwards)
		RE::NiTransform world;
		RE::NiBound worldBound;
		std::uint32_t numVertices{ 0 };
		std::uint32_t numTriangles{ 0 };
		Engine::AlphaState alpha;
		bool shadowCaster{ true };
		std::uint32_t firstSeenFrame{ 0 };
		std::uint32_t stableSinceFrame{ 0 };

		std::atomic<std::uint32_t> lastSeenFrame{ 0 };
		// calibration: last frames the object was queued in a started view /
		// in a queue whose StartAccumulating was never seen
		std::atomic<std::uint32_t> startedViewFrame{ 0 };
		std::atomic<std::uint32_t> otherQueueFrame{ 0 };
		// Some pass of the object is drawn by replaying a command buffer, which
		// bypasses SetupGeometry; its capture draw has to be routed through
		// SetupGeometry (see ImmediatePasses).
		std::atomic<bool> commandBuffers{ false };
		std::atomic<std::uint32_t> detachFrame{ 0 };     // last frame its passes were detached for a capture
		std::atomic<std::uint32_t> detachAttempts{ 0 };  // frames detached since it became a candidate
		Engine::MeshBuffers mesh;  // read at creation
		std::atomic<ObjectState> state{ ObjectState::kTracking };
		std::atomic<std::uint32_t> bucket{ kInvalidIndex };
		std::atomic<std::uint32_t> member{ kInvalidIndex };

		// render thread only
		DrawCapture capture;
		std::uint32_t pendingGroup{ kInvalidIndex };
		bool holdsReference{ false };  // we called IncRefCount on geometry

		SpinLock lock;

		[[nodiscard]] bool Matches(RE::BSGeometry* a_geometry) const noexcept;
	};

	// Concurrent map BSGeometry* -> ObjectRecord. Lookups come from the
	// renderer's accumulation worker threads; structural changes from the
	// render thread at frame boundaries. Records are pooled and only recycled
	// several frames after removal, so a pointer obtained inside a hook stays
	// valid for the rest of that hook call.
	class ObjectRegistry
	{
	public:
		[[nodiscard]] ObjectRecord* Find(const RE::BSGeometry* a_geometry) const;
		[[nodiscard]] ObjectRecord* FindOrCreate(RE::BSGeometry* a_geometry, std::uint32_t a_frame);

		// Render thread: unlinks a record; its storage is recycled later.
		void Remove(ObjectRecord* a_record, std::uint32_t a_frame);

		// Render thread: makes storage removed at least kRecycleDelay frames ago reusable.
		void Recycle(std::uint32_t a_frame);

		template <class F>
		void ForEach(F&& a_func)
		{
			for (auto& shard : _shards) {
				std::shared_lock lock{ shard.lock };
				for (auto& [key, record] : shard.map) {
					a_func(*record);
				}
			}
		}

		[[nodiscard]] std::size_t Size() const;

	private:
		static constexpr std::size_t kShardCount = 64;
		static constexpr std::uint32_t kRecycleDelay = 8;
		static constexpr std::size_t kChunkSize = 4096;

		struct Shard
		{
			mutable std::shared_mutex lock;
			std::unordered_map<const RE::BSGeometry*, ObjectRecord*> map;
		};

		[[nodiscard]] static std::size_t ShardOf(const RE::BSGeometry* a_geometry) noexcept
		{
			auto value = reinterpret_cast<std::uintptr_t>(a_geometry) >> 4;
			value ^= value >> 17;
			return value % kShardCount;
		}

		[[nodiscard]] ObjectRecord* Allocate();

		std::array<Shard, kShardCount> _shards;

		std::mutex _poolLock;
		std::vector<std::unique_ptr<ObjectRecord[]>> _chunks;
		std::size_t _chunkUsed{ kChunkSize };
		std::vector<ObjectRecord*> _free;
		std::deque<std::pair<std::uint32_t, ObjectRecord*>> _retired;
	};
}
