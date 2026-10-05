#pragma once

#include "Render/GpuBuffers.h"
#include "Render/ShaderConstants.h"
#include "Scene/ObjectRegistry.h"
#include "Scene/VertexFormat.h"
#include "Scene/ViewRegistry.h"

namespace GWP
{
	class ShaderLibrary;

	struct MemberSlot
	{
		std::atomic<ObjectRecord*> record{ nullptr };  // cleared when the object is evicted
		std::uint32_t bucket{ kInvalidIndex };
		GpuMember gpu;
	};

	enum class BucketState : std::uint32_t
	{
		kFree,
		kBuilding,  // members are being merged on the GPU
		kActive,    // the engine routes its members through the anchor's draw
		kRetiring   // no new registrations; storage freed after a delay
	};

	// A batch: objects sharing a material, vertex format and grid cell, merged
	// into one vertex/index range expressed in the anchor object's local space.
	// The engine keeps drawing the anchor with its normal shaders and states;
	// the pipeline swaps the anchor's draw for an indirect draw of every member
	// that is visible in the view being rendered.
	struct Bucket
	{
		std::atomic<BucketState> state{ BucketState::kFree };
		std::uint32_t id{ 0 };

		// anchor (we hold a reference on the geometry)
		RE::BSGeometry* anchor{ nullptr };
		RE::BSShaderProperty* anchorProperty{ nullptr };
		RE::NiTransform anchorWorld;

		// anchor's own draw, used to validate the draw we replace
		ID3D11Buffer* anchorIndexBuffer{ nullptr };
		UINT anchorIndexCount{ 0 };
		UINT anchorStartIndex{ 0 };
		INT anchorBaseVertex{ 0 };

		VertexPlan plan;

		// arena placement
		std::uint64_t vertexOffset{ 0 };  // bytes in the vertex arena
		std::uint64_t vertexBytes{ 0 };
		std::uint32_t vertexCount{ 0 };
		std::uint64_t indexOffset{ 0 };   // elements in the index arena
		std::uint32_t indexCount{ 0 };
		std::uint32_t firstMember{ 0 };   // contiguous member id range
		std::uint32_t memberCount{ 0 };

		std::array<std::atomic<std::uint32_t>, kMaxViews> gates{};  // epoch in which the anchor's passes were emitted
		std::atomic<std::uint32_t> evictions{ 0 };
		std::atomic<std::uint32_t> drawAnomalies{ 0 };
		std::atomic<bool> retireRequested{ false };  // set by worker threads, honoured at Present

		std::uint32_t createdFrame{ 0 };
		std::uint32_t activatedFrame{ 0 };
		std::uint32_t retireFrame{ 0 };

		// build job (render thread)
		std::vector<ObjectRecord*> buildRecords;
		std::vector<std::uint32_t> buildVertexBase;
		std::vector<std::uint32_t> buildIndexBase;
		std::uint32_t buildCursor{ 0 };
		std::uint32_t errorSlot{ kInvalidIndex };

		// per-view draw bookkeeping (render thread)
		std::uint32_t viewEpoch{ 0 };
		std::uint32_t viewSlot{ 0 };        // compaction: args record; per-member: first record
		std::uint32_t viewDrawCount{ 0 };   // per-member: records
		std::uint32_t viewMembers{ 0 };
		std::uint32_t viewIndexTotal{ 0 };  // upper bound of indices drawn in the view
		bool viewCompacted{ false };
	};

	struct BucketStats
	{
		std::uint32_t pendingGroups{ 0 };
		std::uint32_t pendingObjects{ 0 };
		std::uint32_t building{ 0 };
		std::uint32_t active{ 0 };
		std::uint32_t retiring{ 0 };
		std::uint32_t activeMembers{ 0 };
		std::uint64_t vertexArenaUsed{ 0 };
		std::uint64_t indexArenaUsed{ 0 };
		std::uint32_t retiredDetached{ 0 };
		std::uint32_t retiredMoved{ 0 };
		std::uint32_t retiredEvictions{ 0 };
		std::uint32_t retiredErrors{ 0 };
		std::uint32_t rejected{ 0 };
		std::uint32_t mergedThisFrame{ 0 };
	};

	class BucketManager
	{
	public:
		static constexpr std::uint32_t kMaxBuckets = 1u << 15;
		static constexpr std::uint32_t kRetireDelay = 8;

		bool Initialize(ID3D11Device* a_device, ShaderLibrary& a_shaders);
		void Shutdown();

		// Render thread, inside the draw hook while the engine draws the object.
		// Assigns a captured record to a pending group (or rejects it).
		void AddCaptured(ObjectRecord& a_record, std::uint32_t a_frame);

		// Render thread, at Present.
		void Update(ID3D11DeviceContext* a_context, ObjectRegistry& a_registry, std::uint32_t a_frame);

		// Called from any thread; valid for buckets referenced by a member record.
		[[nodiscard]] Bucket* Get(std::uint32_t a_id) const noexcept
		{
			return a_id < kMaxBuckets ? _buckets[a_id].load(std::memory_order_acquire) : nullptr;
		}

		[[nodiscard]] MemberSlot* Member(std::uint32_t a_id) const noexcept;

		// Bucket (active or retiring) whose anchor is a_geometry. Independent of
		// the object registry so an anchor is always recognised, even if its
		// record was replaced.
		[[nodiscard]] Bucket* FindByAnchor(const RE::BSGeometry* a_geometry) const;

		// Disables every bucket (used when batching is switched off or a fatal
		// inconsistency is detected).
		void RetireAll(std::uint32_t a_frame);

		[[nodiscard]] ArenaBuffer& VertexArena() noexcept { return _vertices; }
		[[nodiscard]] ArenaBuffer& IndexArena() noexcept { return _indices; }
		[[nodiscard]] ArenaBuffer& MemberTable() noexcept { return _members; }

		// Set by calibration if NiTransform rotations turn out to be stored transposed.
		void SetTransposedTransforms(bool a_transposed) noexcept { _transposedTransforms = a_transposed; }

		[[nodiscard]] const BucketStats& Stats() const noexcept { return _stats; }
		[[nodiscard]] std::uint32_t TotalActiveMembers() const noexcept { return _activeMembers; }

	private:
		struct PendingGroup
		{
			std::uint64_t key{ 0 };
			std::vector<ObjectRecord*> records;
			std::uint64_t vertexTotal{ 0 };
			std::uint32_t lastAddedFrame{ 0 };
			bool alive{ true };
		};

		struct Readback
		{
			std::uint32_t staging{ 0 };
			std::uint32_t bucket{ 0 };
			std::uint32_t slot{ 0 };
			std::uint32_t frame{ 0 };
		};

		[[nodiscard]] std::uint64_t GroupKey(const ObjectRecord& a_record) const;
		[[nodiscard]] bool Compatible(const ObjectRecord& a_representative, const ObjectRecord& a_record) const;

		void CleanGroups(std::uint32_t a_frame);
		void StartBuilds(ID3D11DeviceContext* a_context, std::uint32_t a_frame);
		[[nodiscard]] bool CreateBucket(ID3D11DeviceContext* a_context, std::span<ObjectRecord* const> a_records, std::uint32_t a_frame);
		void PruneBuilds(std::uint32_t a_frame);
		void ProcessBuilds(ID3D11DeviceContext* a_context, std::uint32_t a_frame);
		[[nodiscard]] bool MergeMember(ID3D11DeviceContext* a_context, Bucket& a_bucket, std::uint32_t a_index);
		void Activate(ID3D11DeviceContext* a_context, Bucket& a_bucket, std::uint32_t a_frame);
		void CheckActive(std::uint32_t a_frame);
		void Retire(Bucket& a_bucket, std::uint32_t a_frame);
		void FreeRetired(std::uint32_t a_frame);
		void PollReadbacks(ID3D11DeviceContext* a_context, std::uint32_t a_frame);
		void ReleaseRecord(ObjectRecord& a_record, ObjectState a_state, std::uint32_t a_frame);

		[[nodiscard]] Bucket* AllocateBucket();
		[[nodiscard]] bool EnsureMemberSlots(std::uint32_t a_end);

		ID3D11Device* _device{ nullptr };

		ArenaBuffer _vertices;
		ArenaBuffer _indices;
		ArenaBuffer _members;
		ArenaBuffer _staging;
		std::uint64_t _stagingCursor{ 0 };

		Microsoft::WRL::ComPtr<ID3D11Buffer> _errors;
		Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> _errorsUAV;
		static constexpr std::uint32_t kErrorSlots = 256;
		static constexpr std::uint32_t kReadbackBuffers = 4;
		std::array<Microsoft::WRL::ComPtr<ID3D11Buffer>, kReadbackBuffers> _readbackBuffers;
		std::vector<std::uint32_t> _freeErrorSlots;
		std::vector<Readback> _readbacks;

		Microsoft::WRL::ComPtr<ID3D11ComputeShader> _mergeVertices;
		Microsoft::WRL::ComPtr<ID3D11ComputeShader> _mergeIndices;
		Microsoft::WRL::ComPtr<ID3D11Buffer> _mergeConstants;
		Microsoft::WRL::ComPtr<ID3D11Buffer> _indexConstants;

		std::unique_ptr<std::atomic<Bucket*>[]> _buckets;
		std::vector<std::unique_ptr<Bucket>> _bucketStorage;
		std::vector<Bucket*> _freeBuckets;
		std::vector<Bucket*> _buildQueue;

		static constexpr std::uint32_t kMemberChunk = 1u << 14;
		static constexpr std::uint32_t kMaxMemberChunks = 64;
		std::array<std::atomic<MemberSlot*>, kMaxMemberChunks> _memberChunks{};
		std::vector<std::unique_ptr<MemberSlot[]>> _memberStorage;

		mutable std::shared_mutex _anchorLock;
		std::unordered_map<const RE::BSGeometry*, Bucket*> _anchors;

		std::vector<PendingGroup> _groups;
		std::vector<std::uint32_t> _freeGroups;
		std::unordered_multimap<std::uint64_t, std::uint32_t> _groupsByKey;
		bool _transposedTransforms{ false };
		std::uint32_t _heldReferences{ 0 };

		std::uint32_t _activeMembers{ 0 };
		BucketStats _stats;
	};
}
