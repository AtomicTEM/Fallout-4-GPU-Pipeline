#include "Scene/BucketManager.h"

#include "Render/ShaderConstants.h"
#include "Render/ShaderLibrary.h"
#include "Render/StateGuards.h"
#include "Scene/MergeMath.h"
#include "Settings.h"
#include "Util/Math.h"

namespace GWP
{
	namespace
	{
		using IndexConstants = ShaderConstants::Index;

		inline constexpr std::uint64_t kVertexAlignment = 256;
		inline constexpr std::uint32_t kStaleGroupFrames = 1200;

		[[nodiscard]] constexpr std::uint64_t AlignUp(std::uint64_t a_value, std::uint64_t a_alignment) noexcept
		{
			return (a_value + a_alignment - 1) / a_alignment * a_alignment;
		}

		[[nodiscard]] RE::NiRefObject* RefObject(RE::BSGeometry* a_geometry) noexcept
		{
			return reinterpret_cast<RE::NiRefObject*>(a_geometry);
		}

		[[nodiscard]] bool SameRotation(const RE::NiTransform& a_lhs, const RE::NiTransform& a_rhs) noexcept
		{
			for (std::uint32_t row = 0; row < 3; ++row) {
				for (std::uint32_t col = 0; col < 3; ++col) {
					if (std::abs(a_lhs.rotate.entry[row].pt[col] - a_rhs.rotate.entry[row].pt[col]) > 1e-5F) {
						return false;
					}
				}
			}
			return std::abs(a_lhs.scale - a_rhs.scale) <= 1e-5F;
		}

		[[nodiscard]] UINT BufferBytes(ID3D11Buffer* a_buffer) noexcept
		{
			D3D11_BUFFER_DESC desc{};
			a_buffer->GetDesc(&desc);
			return desc.ByteWidth;
		}

		void CopyBufferRange(ID3D11DeviceContext* a_context, ID3D11Buffer* a_dst, std::uint64_t a_dstOffset, ID3D11Buffer* a_src, std::uint64_t a_srcBegin, std::uint64_t a_srcEnd)
		{
			D3D11_BOX box{};
			box.left = static_cast<UINT>(a_srcBegin);
			box.right = static_cast<UINT>(a_srcEnd);
			box.top = 0;
			box.bottom = 1;
			box.front = 0;
			box.back = 1;
			a_context->CopySubresourceRegion(a_dst, 0, static_cast<UINT>(a_dstOffset), 0, 0, a_src, 0, &box);
		}
	}

	bool BucketManager::Initialize(ID3D11Device* a_device, ShaderLibrary& a_shaders)
	{
		_device = a_device;
		const auto& settings = Settings::Get();

		ArenaBuffer::Desc vertices;
		vertices.name = "vertex arena";
		vertices.bindFlags = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_UNORDERED_ACCESS;
		vertices.miscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		vertices.initialBytes = 32ull << 20;
		vertices.maxBytes = static_cast<std::uint64_t>(settings.arenaVertexMB) << 20;
		vertices.uav = true;

		ArenaBuffer::Desc indices;
		indices.name = "index arena";
		indices.bindFlags = D3D11_BIND_INDEX_BUFFER | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		indices.miscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		indices.initialBytes = 16ull << 20;
		indices.maxBytes = static_cast<std::uint64_t>(settings.arenaIndexMB) << 20;
		indices.srv = true;
		indices.uav = true;

		ArenaBuffer::Desc members;
		members.name = "member table";
		members.bindFlags = D3D11_BIND_SHADER_RESOURCE;
		members.miscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		members.structureStride = sizeof(GpuMember);
		members.initialBytes = sizeof(GpuMember) * kMemberChunk;
		members.maxBytes = static_cast<std::uint64_t>(sizeof(GpuMember)) * kMemberChunk * kMaxMemberChunks;
		members.srv = true;

		ArenaBuffer::Desc staging;
		staging.name = "merge staging";
		staging.bindFlags = D3D11_BIND_SHADER_RESOURCE;
		staging.miscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		staging.initialBytes = static_cast<std::uint64_t>(settings.stagingMB) << 20;
		staging.maxBytes = staging.initialBytes;
		staging.srv = true;

		if (!_vertices.Create(a_device, vertices) || !_indices.Create(a_device, indices) ||
			!_members.Create(a_device, members) || !_staging.Create(a_device, staging)) {
			return false;
		}

		D3D11_BUFFER_DESC errors{};
		errors.ByteWidth = kErrorSlots * 4;
		errors.Usage = D3D11_USAGE_DEFAULT;
		errors.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		errors.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		if (FAILED(a_device->CreateBuffer(&errors, nullptr, _errors.GetAddressOf()))) {
			return false;
		}

		D3D11_UNORDERED_ACCESS_VIEW_DESC errorsUAV{};
		errorsUAV.Format = DXGI_FORMAT_R32_TYPELESS;
		errorsUAV.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		errorsUAV.Buffer.NumElements = kErrorSlots;
		errorsUAV.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
		if (FAILED(a_device->CreateUnorderedAccessView(_errors.Get(), &errorsUAV, _errorsUAV.GetAddressOf()))) {
			return false;
		}

		D3D11_BUFFER_DESC readback{};
		readback.ByteWidth = kErrorSlots * 4;
		readback.Usage = D3D11_USAGE_STAGING;
		readback.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		for (auto& buffer : _readbackBuffers) {
			if (FAILED(a_device->CreateBuffer(&readback, nullptr, buffer.GetAddressOf()))) {
				return false;
			}
		}
		for (std::uint32_t slot = kErrorSlots; slot-- > 0;) {
			_freeErrorSlots.push_back(slot);
		}

		_mergeVertices = a_shaders.CompileCompute("MergeVertices.hlsl", "CSMain");
		_mergeIndices = a_shaders.CompileCompute("MergeIndices.hlsl", "CSMain");
		_mergeConstants = CreateConstantBuffer(a_device, sizeof(ShaderConstants::Merge));
		_indexConstants = CreateConstantBuffer(a_device, sizeof(IndexConstants));
		if (!_mergeVertices || !_mergeIndices || !_mergeConstants || !_indexConstants) {
			return false;
		}

		_buckets = std::make_unique<std::atomic<Bucket*>[]>(kMaxBuckets);
		for (std::uint32_t i = 0; i < kMaxBuckets; ++i) {
			_buckets[i].store(nullptr, std::memory_order_relaxed);
		}
		return EnsureMemberSlots(kMemberChunk);
	}

	void BucketManager::Shutdown()
	{
		RetireAll(0);
		_vertices.Destroy();
		_indices.Destroy();
		_members.Destroy();
		_staging.Destroy();
	}

	MemberSlot* BucketManager::Member(std::uint32_t a_id) const noexcept
	{
		const auto chunk = a_id / kMemberChunk;
		if (chunk >= kMaxMemberChunks) {
			return nullptr;
		}
		auto* const slots = _memberChunks[chunk].load(std::memory_order_acquire);
		return slots ? &slots[a_id % kMemberChunk] : nullptr;
	}

	Bucket* BucketManager::FindByAnchor(const RE::BSGeometry* a_geometry) const
	{
		std::shared_lock lock{ _anchorLock };
		const auto it = _anchors.find(a_geometry);
		return it != _anchors.end() ? it->second : nullptr;
	}

	bool BucketManager::EnsureMemberSlots(std::uint32_t a_end)
	{
		const auto chunks = (a_end + kMemberChunk - 1) / kMemberChunk;
		if (chunks > kMaxMemberChunks) {
			return false;
		}
		for (std::uint32_t chunk = 0; chunk < chunks; ++chunk) {
			if (!_memberChunks[chunk].load(std::memory_order_relaxed)) {
				auto& storage = _memberStorage.emplace_back(std::make_unique<MemberSlot[]>(kMemberChunk));
				_memberChunks[chunk].store(storage.get(), std::memory_order_release);
			}
		}
		return true;
	}

	Bucket* BucketManager::AllocateBucket()
	{
		if (!_freeBuckets.empty()) {
			auto* const bucket = _freeBuckets.back();
			_freeBuckets.pop_back();
			return bucket;
		}
		if (_bucketStorage.size() >= kMaxBuckets) {
			return nullptr;
		}
		auto& bucket = _bucketStorage.emplace_back(std::make_unique<Bucket>());
		bucket->id = static_cast<std::uint32_t>(_bucketStorage.size() - 1);
		_buckets[bucket->id].store(bucket.get(), std::memory_order_release);
		return bucket.get();
	}

	std::uint64_t BucketManager::GroupKey(const ObjectRecord& a_record) const
	{
		const auto grid = static_cast<double>(Settings::Get().gridSize);
		const auto cell = [&](float a_value) {
			return static_cast<std::int64_t>(std::floor(static_cast<double>(a_value) / grid));
		};

		std::uint64_t key = 0x51ED27F4A1C3B6E5ull;
		key = Math::HashCombine(key, reinterpret_cast<std::uintptr_t>(a_record.property->material));
		key = Math::HashCombine(key, a_record.property->flags);
		key = Math::HashCombine(key, a_record.alpha.Key());
		key = Math::HashCombine(key, a_record.vertexDesc);
		key = Math::HashCombine(key, a_record.capture.stride);
		key = Math::HashCombine(key, a_record.shadowCaster ? 1 : 0);
		key = Math::HashCombine(key, static_cast<std::uint64_t>(cell(a_record.worldBound.center.x)));
		key = Math::HashCombine(key, static_cast<std::uint64_t>(cell(a_record.worldBound.center.y)));
		key = Math::HashCombine(key, static_cast<std::uint64_t>(cell(a_record.worldBound.center.z)));
		return key;
	}

	bool BucketManager::Compatible(const ObjectRecord& a_representative, const ObjectRecord& a_record) const
	{
		if (a_representative.vertexDesc != a_record.vertexDesc ||
			a_representative.capture.stride != a_record.capture.stride ||
			a_representative.property->material != a_record.property->material ||
			a_representative.property->flags != a_record.property->flags ||
			a_representative.alpha.Key() != a_record.alpha.Key() ||
			a_representative.shadowCaster != a_record.shadowCaster) {
			return false;
		}

		// The engine's own precombine test. Both properties are alive here: the
		// representative holds a reference, the record is being drawn right now.
		if (Settings::Get().useCanMerge && a_representative.property != a_record.property &&
			!a_representative.property->CanMerge(a_record.property)) {
			return false;
		}
		return true;
	}

	void BucketManager::AddCaptured(ObjectRecord& a_record, std::uint32_t a_frame)
	{
		const auto& settings = Settings::Get();

		std::string reason;
		const auto layout = InputLayouts::Get().Find(a_record.capture.layout);
		const auto plan = BuildVertexPlan(a_record.vertexDesc, a_record.capture.stride, layout.get(), settings.rotateBitangentW, &reason);
		if (!plan) {
			ReleaseRecord(a_record, ObjectState::kRejected, a_frame);
			++_stats.rejected;
			if (settings.verboseLogging) {
				logger::debug("buckets: rejected {}: {}", fmt::ptr(a_record.geometry), reason);
			}
			return;
		}

		if (_heldReferences >= settings.maxPendingReferences) {
			ReleaseRecord(a_record, ObjectState::kTracking, a_frame);
			return;
		}

		RefObject(a_record.geometry)->IncRefCount();
		a_record.holdsReference = true;
		++_heldReferences;

		const auto key = GroupKey(a_record);
		const auto [begin, end] = _groupsByKey.equal_range(key);
		for (auto it = begin; it != end; ++it) {
			auto& group = _groups[it->second];
			if (!group.alive || group.records.empty() || group.records.size() >= std::size_t{ settings.maxMembersPerBucket } * 4) {
				continue;
			}
			const auto& representative = *group.records.front();
			if (!Compatible(representative, a_record)) {
				continue;
			}
			if (!plan->canRotateDirections && !SameRotation(representative.world, a_record.world)) {
				continue;
			}

			group.records.push_back(&a_record);
			group.vertexTotal += a_record.numVertices;
			group.lastAddedFrame = a_frame;
			a_record.pendingGroup = it->second;
			return;
		}

		std::uint32_t index = 0;
		if (!_freeGroups.empty()) {
			index = _freeGroups.back();
			_freeGroups.pop_back();
		} else {
			index = static_cast<std::uint32_t>(_groups.size());
			_groups.emplace_back();
		}

		auto& group = _groups[index];
		group = PendingGroup{};
		group.key = key;
		group.records.push_back(&a_record);
		group.vertexTotal = a_record.numVertices;
		group.lastAddedFrame = a_frame;
		group.alive = true;
		_groupsByKey.emplace(key, index);
		a_record.pendingGroup = index;
	}

	void BucketManager::ReleaseRecord(ObjectRecord& a_record, ObjectState a_state, std::uint32_t a_frame)
	{
		if (a_record.holdsReference) {
			a_record.holdsReference = false;
			--_heldReferences;
			RefObject(a_record.geometry)->DecRefCount();
		}
		a_record.capture = {};
		a_record.pendingGroup = kInvalidIndex;

		// Records evicted by a worker were already unlinked from the registry.
		auto expected = a_record.state.load(std::memory_order_acquire);
		if (expected == ObjectState::kEvicted) {
			return;
		}
		{
			std::scoped_lock lock{ a_record.lock };
			a_record.stableSinceFrame = a_frame;
		}
		a_record.bucket.store(kInvalidIndex, std::memory_order_relaxed);
		a_record.member.store(kInvalidIndex, std::memory_order_relaxed);
		a_record.state.compare_exchange_strong(expected, a_state, std::memory_order_acq_rel);
	}

	void BucketManager::CleanGroups(std::uint32_t a_frame)
	{
		for (std::uint32_t index = 0; index < _groups.size(); ++index) {
			auto& group = _groups[index];
			if (!group.alive) {
				continue;
			}

			std::erase_if(group.records, [&](ObjectRecord* a_record) {
				if (a_record->state.load(std::memory_order_acquire) == ObjectState::kCaptured) {
					return false;
				}
				ReleaseRecord(*a_record, ObjectState::kTracking, a_frame);
				return true;
			});

			const bool stale = a_frame - group.lastAddedFrame > kStaleGroupFrames && group.records.size() < Settings::Get().minMembers;
			if (stale) {
				for (auto* record : group.records) {
					ReleaseRecord(*record, ObjectState::kTracking, a_frame);
				}
				group.records.clear();
			}

			if (group.records.empty()) {
				group.alive = false;
				const auto [begin, end] = _groupsByKey.equal_range(group.key);
				for (auto it = begin; it != end; ++it) {
					if (it->second == index) {
						_groupsByKey.erase(it);
						break;
					}
				}
				_freeGroups.push_back(index);
			}
		}
	}

	void BucketManager::StartBuilds(ID3D11DeviceContext* a_context, std::uint32_t a_frame)
	{
		const auto& settings = Settings::Get();

		for (std::uint32_t index = 0; index < _groups.size(); ++index) {
			auto& group = _groups[index];
			if (!group.alive || group.records.size() < settings.minMembers || a_frame - group.lastAddedFrame < settings.settleFrames) {
				continue;
			}

			// Drop objects that were unloaded or moved since they were captured.
			std::vector<ObjectRecord*> records;
			records.reserve(group.records.size());
			for (auto* record : group.records) {
				const auto* const object = Engine::AsAVObject(record->geometry);
				if (object->parent && Math::NearlyEqual(object->world, record->world)) {
					records.push_back(record);
				} else {
					ReleaseRecord(*record, ObjectState::kTracking, a_frame);
				}
			}
			group.records.clear();

			// Split into buckets that respect the member and vertex limits.
			std::size_t begin = 0;
			while (begin < records.size()) {
				std::size_t end = begin;
				std::uint64_t vertices = 0;
				while (end < records.size() && end - begin < settings.maxMembersPerBucket &&
					   (end == begin || vertices + records[end]->numVertices <= settings.maxVerticesPerBucket)) {
					vertices += records[end]->numVertices;
					++end;
				}

				const std::span<ObjectRecord* const> chunk{ records.data() + begin, end - begin };
				if (chunk.size() < settings.minMembers || !CreateBucket(a_context, chunk, a_frame)) {
					for (auto* record : chunk) {
						ReleaseRecord(*record, ObjectState::kTracking, a_frame);
					}
				}
				begin = end;
			}
		}
	}

	bool BucketManager::CreateBucket(ID3D11DeviceContext* a_context, std::span<ObjectRecord* const> a_records, std::uint32_t a_frame)
	{
		auto& anchor = *a_records.front();
		const auto layout = InputLayouts::Get().Find(anchor.capture.layout);
		auto plan = BuildVertexPlan(anchor.vertexDesc, anchor.capture.stride, layout.get(), Settings::Get().rotateBitangentW, nullptr);
		if (!plan || _freeErrorSlots.empty()) {
			return false;
		}

		std::uint64_t vertexCount = 0;
		std::uint64_t indexCount = 0;
		for (const auto* record : a_records) {
			vertexCount += record->numVertices;
			indexCount += record->capture.indexCount;
		}

		const auto vertexBytes = vertexCount * plan->dstStride;
		const auto vertexOffset = _vertices.Allocate(a_context, vertexBytes, kVertexAlignment);
		if (!vertexOffset) {
			return false;
		}
		const auto indexOffset = _indices.Allocate(a_context, indexCount * 4, 4);
		if (!indexOffset) {
			_vertices.Free(*vertexOffset, vertexBytes);
			return false;
		}
		const auto memberOffset = _members.Allocate(a_context, a_records.size() * sizeof(GpuMember), sizeof(GpuMember));
		const auto firstMember = memberOffset ? static_cast<std::uint32_t>(*memberOffset / sizeof(GpuMember)) : 0;
		if (!memberOffset || !EnsureMemberSlots(firstMember + static_cast<std::uint32_t>(a_records.size()))) {
			if (memberOffset) {
				_members.Free(*memberOffset, a_records.size() * sizeof(GpuMember));
			}
			_indices.Free(*indexOffset, indexCount * 4);
			_vertices.Free(*vertexOffset, vertexBytes);
			return false;
		}

		auto* const bucket = AllocateBucket();
		if (!bucket) {
			_members.Free(*memberOffset, a_records.size() * sizeof(GpuMember));
			_indices.Free(*indexOffset, indexCount * 4);
			_vertices.Free(*vertexOffset, vertexBytes);
			return false;
		}

		bucket->anchor = anchor.geometry;
		bucket->anchorProperty = anchor.property;
		bucket->anchorWorld = anchor.world;
		bucket->anchorIndexBuffer = anchor.capture.indexBuffer.Get();
		bucket->anchorIndexCount = anchor.capture.indexCount;
		bucket->anchorStartIndex = anchor.capture.startIndex;
		bucket->anchorBaseVertex = anchor.capture.baseVertex;
		bucket->plan = std::move(*plan);
		bucket->vertexOffset = *vertexOffset;
		bucket->vertexBytes = vertexBytes;
		bucket->vertexCount = static_cast<std::uint32_t>(vertexCount);
		bucket->indexOffset = *indexOffset / 4;
		bucket->indexCount = static_cast<std::uint32_t>(indexCount);
		bucket->firstMember = firstMember;
		bucket->memberCount = static_cast<std::uint32_t>(a_records.size());
		bucket->evictions.store(0, std::memory_order_relaxed);
		bucket->drawAnomalies.store(0, std::memory_order_relaxed);
		bucket->retireRequested.store(false, std::memory_order_relaxed);
		for (auto& gate : bucket->gates) {
			gate.store(0, std::memory_order_relaxed);
		}
		bucket->createdFrame = a_frame;
		bucket->viewEpoch = 0;

		bucket->buildRecords.assign(a_records.begin(), a_records.end());
		bucket->buildVertexBase.clear();
		bucket->buildIndexBase.clear();
		std::uint32_t vertexBase = 0;
		std::uint32_t indexBase = 0;
		for (const auto* record : a_records) {
			bucket->buildVertexBase.push_back(vertexBase);
			bucket->buildIndexBase.push_back(indexBase);
			vertexBase += record->numVertices;
			indexBase += record->capture.indexCount;
		}
		bucket->buildCursor = 0;

		bucket->errorSlot = _freeErrorSlots.back();
		_freeErrorSlots.pop_back();
		const std::uint32_t zero = 0;
		D3D11_BOX box{ bucket->errorSlot * 4, 0, 0, bucket->errorSlot * 4 + 4, 1, 1 };
		a_context->UpdateSubresource(_errors.Get(), 0, &box, &zero, 0, 0);

		for (std::uint32_t i = 0; i < bucket->memberCount; ++i) {
			auto* const slot = Member(firstMember + i);
			slot->record.store(nullptr, std::memory_order_relaxed);
			slot->bucket = bucket->id;
		}

		bucket->state.store(BucketState::kBuilding, std::memory_order_release);
		_buildQueue.push_back(bucket);
		return true;
	}

	bool BucketManager::MergeMember(ID3D11DeviceContext* a_context, Bucket& a_bucket, std::uint32_t a_index)
	{
		auto* const slot = Member(a_bucket.firstMember + a_index);
		slot->gpu = {};
		slot->gpu.bucket = a_bucket.id;
		slot->gpu.indexStart = static_cast<std::uint32_t>(a_bucket.indexOffset + a_bucket.buildIndexBase[a_index]);

		auto* const recordPtr = a_bucket.buildRecords[a_index];
		if (!recordPtr) {
			return true;  // pruned while waiting; leave an empty member
		}
		auto& record = *recordPtr;
		const auto& capture = record.capture;
		const auto& plan = a_bucket.plan;

		if (record.state.load(std::memory_order_acquire) != ObjectState::kCaptured || !capture.vertexBuffer || !capture.indexBuffer) {
			return true;
		}

		const auto indexSize = capture.indexFormat == DXGI_FORMAT_R32_UINT ? 4u : 2u;
		const auto vertexStart = static_cast<std::int64_t>(capture.vertexOffset) + static_cast<std::int64_t>(capture.baseVertex) * capture.stride;
		const auto vertexBytes = static_cast<std::int64_t>(record.numVertices) * capture.stride;
		const auto indexStart = static_cast<std::int64_t>(capture.indexOffset) + static_cast<std::int64_t>(capture.startIndex) * indexSize;
		const auto indexBytes = static_cast<std::int64_t>(capture.indexCount) * indexSize;

		const auto vertexBufferSize = static_cast<std::int64_t>(BufferBytes(capture.vertexBuffer.Get()));
		const auto indexBufferSize = static_cast<std::int64_t>(BufferBytes(capture.indexBuffer.Get()));
		if (vertexStart < 0 || vertexStart + vertexBytes > vertexBufferSize || indexStart < 0 || indexStart + indexBytes > indexBufferSize) {
			logger::warn("buckets: capture of {} lies outside its buffers, skipping", fmt::ptr(record.geometry));
			return true;
		}

		// Stage both ranges (4-byte aligned) in one raw buffer the shaders can read.
		const auto vertexLo = static_cast<std::uint64_t>(vertexStart) & ~3ull;
		const auto vertexHi = std::min<std::uint64_t>(AlignUp(static_cast<std::uint64_t>(vertexStart + vertexBytes), 4), static_cast<std::uint64_t>(vertexBufferSize));
		const auto indexLo = static_cast<std::uint64_t>(indexStart) & ~3ull;
		const auto indexHi = std::min<std::uint64_t>(AlignUp(static_cast<std::uint64_t>(indexStart + indexBytes), 4), static_cast<std::uint64_t>(indexBufferSize));

		const auto vertexStage = AlignUp(_stagingCursor, 16);
		const auto indexStage = AlignUp(vertexStage + (vertexHi - vertexLo), 16);
		const auto stageEnd = indexStage + (indexHi - indexLo);
		if (stageEnd > _staging.Size()) {
			if (_stagingCursor == 0) {
				logger::warn("buckets: object {} does not fit the merge staging buffer", fmt::ptr(record.geometry));
				return true;
			}
			return false;  // continue next frame with an empty staging buffer
		}
		_stagingCursor = stageEnd;

		CopyBufferRange(a_context, _staging.Buffer(), vertexStage, capture.vertexBuffer.Get(), vertexLo, vertexHi);
		CopyBufferRange(a_context, _staging.Buffer(), indexStage, capture.indexBuffer.Get(), indexLo, indexHi);

		const auto transform = ComputeMergeTransform(a_bucket.anchorWorld, record.world, _transposedTransforms, plan.canRotateDirections);
		if (!transform) {
			return true;  // degenerate (zero scale) object
		}

		const auto merge = MakeMergeConstants(plan, *transform,
			static_cast<std::uint32_t>(vertexStage + (static_cast<std::uint64_t>(vertexStart) - vertexLo)),
			static_cast<std::uint32_t>(a_bucket.vertexOffset + static_cast<std::uint64_t>(a_bucket.buildVertexBase[a_index]) * plan.dstStride),
			record.numVertices);

		IndexConstants indices{};
		indices.srcByteOffset = static_cast<std::uint32_t>(indexStage + (static_cast<std::uint64_t>(indexStart) - indexLo));
		indices.indexCount = capture.indexCount;
		indices.dstIndex = slot->gpu.indexStart;
		indices.vertexBase = a_bucket.buildVertexBase[a_index];
		indices.vertexCount = record.numVertices;
		indices.flipWinding = transform->mirrored ? 1 : 0;
		indices.index32 = indexSize == 4 ? 1 : 0;
		indices.errorSlot = a_bucket.errorSlot;

		ID3D11ShaderResourceView* const staging = _staging.SRV();
		ID3D11Buffer* const mergeCB = _mergeConstants.Get();
		ID3D11Buffer* const indexCB = _indexConstants.Get();

		UploadConstants(a_context, mergeCB, merge);
		ID3D11UnorderedAccessView* vertexUAVs[2]{ _vertices.UAV(), nullptr };
		a_context->CSSetShader(_mergeVertices.Get(), nullptr, 0);
		a_context->CSSetConstantBuffers(0, 1, &mergeCB);
		a_context->CSSetShaderResources(0, 1, &staging);
		a_context->CSSetUnorderedAccessViews(0, 2, vertexUAVs, nullptr);
		a_context->Dispatch((record.numVertices + 63) / 64, 1, 1);

		UploadConstants(a_context, indexCB, indices);
		ID3D11UnorderedAccessView* indexUAVs[2]{ _indices.UAV(), _errorsUAV.Get() };
		a_context->CSSetShader(_mergeIndices.Get(), nullptr, 0);
		a_context->CSSetConstantBuffers(0, 1, &indexCB);
		a_context->CSSetUnorderedAccessViews(0, 2, indexUAVs, nullptr);
		a_context->Dispatch((capture.indexCount + 63) / 64, 1, 1);

		const auto& bound = record.worldBound;
		slot->gpu.sphere[0] = bound.center.x;
		slot->gpu.sphere[1] = bound.center.y;
		slot->gpu.sphere[2] = bound.center.z;
		slot->gpu.sphere[3] = bound.fRadius;
		slot->gpu.indexCount = capture.indexCount;
		++_stats.mergedThisFrame;
		return true;
	}

	void BucketManager::PruneBuilds(std::uint32_t a_frame)
	{
		// Records evicted by worker threads are recycled by the registry after a
		// few frames, so building buckets must drop them now.
		std::vector<Bucket*> abandoned;
		for (auto* bucket : _buildQueue) {
			for (std::size_t i = 0; i < bucket->buildRecords.size(); ++i) {
				auto* const record = bucket->buildRecords[i];
				if (!record || record->state.load(std::memory_order_acquire) != ObjectState::kEvicted) {
					continue;
				}
				if (i == 0) {
					abandoned.push_back(bucket);  // the anchor changed; start over
					break;
				}
				ReleaseRecord(*record, ObjectState::kTracking, a_frame);
				bucket->buildRecords[i] = nullptr;
			}
		}
		for (auto* bucket : abandoned) {
			Retire(*bucket, a_frame);
		}
	}

	void BucketManager::ProcessBuilds(ID3D11DeviceContext* a_context, std::uint32_t a_frame)
	{
		if (_buildQueue.empty()) {
			return;
		}

		ComputeStateGuard guard{ a_context };
		std::int64_t budget = Settings::Get().ingestVertexBudget;
		_stagingCursor = 0;

		std::size_t finished = 0;
		for (auto* bucket : _buildQueue) {
			while (bucket->buildCursor < bucket->buildRecords.size() && budget > 0) {
				if (!MergeMember(a_context, *bucket, bucket->buildCursor)) {
					budget = 0;
					break;
				}
				if (const auto* record = bucket->buildRecords[bucket->buildCursor]) {
					budget -= record->numVertices;
				}
				++bucket->buildCursor;
			}
			if (bucket->buildCursor < bucket->buildRecords.size()) {
				break;
			}
			Activate(a_context, *bucket, a_frame);
			++finished;
		}
		_buildQueue.erase(_buildQueue.begin(), _buildQueue.begin() + static_cast<std::ptrdiff_t>(finished));

		ID3D11UnorderedAccessView* const nullUAVs[2]{};
		a_context->CSSetUnorderedAccessViews(0, 2, nullUAVs, nullptr);
	}

	void BucketManager::Activate(ID3D11DeviceContext* a_context, Bucket& a_bucket, std::uint32_t a_frame)
	{
		std::vector<GpuMember> table(a_bucket.memberCount);
		for (std::uint32_t i = 0; i < a_bucket.memberCount; ++i) {
			table[i] = Member(a_bucket.firstMember + i)->gpu;
		}
		D3D11_BOX box{};
		box.left = a_bucket.firstMember * static_cast<UINT>(sizeof(GpuMember));
		box.right = box.left + a_bucket.memberCount * static_cast<UINT>(sizeof(GpuMember));
		box.bottom = 1;
		box.back = 1;
		a_context->UpdateSubresource(_members.Buffer(), 0, &box, table.data(), 0, 0);

		// Queue the index validation result for asynchronous readback.
		const auto staging = a_frame % kReadbackBuffers;
		D3D11_BOX errorBox{ a_bucket.errorSlot * 4, 0, 0, a_bucket.errorSlot * 4 + 4, 1, 1 };
		a_context->CopySubresourceRegion(_readbackBuffers[staging].Get(), 0, a_bucket.errorSlot * 4, 0, 0, _errors.Get(), 0, &errorBox);
		_readbacks.push_back({ staging, a_bucket.id, a_bucket.errorSlot, a_frame });  // a_frame doubles as the activation stamp
		a_bucket.errorSlot = kInvalidIndex;

		// Publish the bucket before any record points at it.
		a_bucket.activatedFrame = a_frame;
		a_bucket.state.store(BucketState::kActive, std::memory_order_release);
		{
			std::unique_lock lock{ _anchorLock };
			_anchors[a_bucket.anchor] = &a_bucket;
		}

		for (std::uint32_t i = 0; i < a_bucket.memberCount; ++i) {
			auto* const slot = Member(a_bucket.firstMember + i);
			if (!a_bucket.buildRecords[i]) {
				slot->record.store(nullptr, std::memory_order_release);
				continue;
			}
			auto& record = *a_bucket.buildRecords[i];

			record.bucket.store(a_bucket.id, std::memory_order_relaxed);
			record.member.store(a_bucket.firstMember + i, std::memory_order_relaxed);
			slot->record.store(&record, std::memory_order_release);

			auto expected = ObjectState::kCaptured;
			if (!record.state.compare_exchange_strong(expected, ObjectState::kMember, std::memory_order_acq_rel)) {
				slot->record.store(nullptr, std::memory_order_release);
			}

			if (i != 0 && record.holdsReference) {
				record.holdsReference = false;
				--_heldReferences;
				RefObject(record.geometry)->DecRefCount();
			}
			record.capture = {};
			record.pendingGroup = kInvalidIndex;
		}

		// The anchor keeps its reference for the bucket's lifetime.
		auto& anchor = *a_bucket.buildRecords.front();
		if (anchor.holdsReference) {
			anchor.holdsReference = false;
			--_heldReferences;
		} else {
			RefObject(a_bucket.anchor)->IncRefCount();
		}

		_activeMembers += a_bucket.memberCount;
		a_bucket.buildRecords.clear();
		a_bucket.buildVertexBase.clear();
		a_bucket.buildIndexBase.clear();

		if (Settings::Get().verboseLogging) {
			logger::debug("buckets: #{} active, {} members, {} vertices, {} indices", a_bucket.id, a_bucket.memberCount, a_bucket.vertexCount, a_bucket.indexCount);
		}
	}

	void BucketManager::PollReadbacks(ID3D11DeviceContext* a_context, std::uint32_t a_frame)
	{
		for (std::uint32_t staging = 0; staging < kReadbackBuffers; ++staging) {
			const bool due = std::ranges::any_of(_readbacks, [&](const Readback& a_readback) {
				return a_readback.staging == staging && a_frame - a_readback.frame >= 2;
			});
			if (!due) {
				continue;
			}

			D3D11_MAPPED_SUBRESOURCE mapped{};
			const auto result = a_context->Map(_readbackBuffers[staging].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
			if (result == DXGI_ERROR_WAS_STILL_DRAWING || FAILED(result)) {
				continue;
			}
			const auto* const values = static_cast<const std::uint32_t*>(mapped.pData);

			std::erase_if(_readbacks, [&](const Readback& a_readback) {
				if (a_readback.staging != staging || a_frame - a_readback.frame < 2) {
					return false;
				}
				const auto errors = values[a_readback.slot];
				if (errors != 0) {
					auto* const bucket = Get(a_readback.bucket);
					logger::warn("buckets: #{} had {} out-of-range indices, retiring", a_readback.bucket, errors);
					if (bucket && bucket->activatedFrame == a_readback.frame && bucket->state.load(std::memory_order_acquire) == BucketState::kActive) {
						Retire(*bucket, a_frame);
						++_stats.retiredErrors;
					}
				}
				_freeErrorSlots.push_back(a_readback.slot);
				return true;
			});
			a_context->Unmap(_readbackBuffers[staging].Get(), 0);
		}
	}

	void BucketManager::CheckActive(std::uint32_t a_frame)
	{
		for (auto& storage : _bucketStorage) {
			auto& bucket = *storage;
			if (bucket.state.load(std::memory_order_acquire) != BucketState::kActive) {
				continue;
			}

			const auto* const anchor = Engine::AsAVObject(bucket.anchor);
			if (!anchor->parent) {
				Retire(bucket, a_frame);
				++_stats.retiredDetached;
			} else if (!Math::NearlyEqual(anchor->world, bucket.anchorWorld) || Engine::Geometry::ShaderProperty(bucket.anchor) != bucket.anchorProperty) {
				Retire(bucket, a_frame);
				++_stats.retiredMoved;
			} else if (bucket.evictions.load(std::memory_order_relaxed) * 4 >= bucket.memberCount ||
					   bucket.drawAnomalies.load(std::memory_order_relaxed) > 16 ||
					   bucket.retireRequested.load(std::memory_order_relaxed)) {
				Retire(bucket, a_frame);
				++_stats.retiredEvictions;
			}
		}
	}

	void BucketManager::Retire(Bucket& a_bucket, std::uint32_t a_frame)
	{
		const auto previous = a_bucket.state.exchange(BucketState::kRetiring, std::memory_order_acq_rel);
		a_bucket.retireFrame = a_frame;

		if (previous == BucketState::kBuilding) {
			std::erase(_buildQueue, &a_bucket);
			for (auto* record : a_bucket.buildRecords) {
				if (record) {
					ReleaseRecord(*record, ObjectState::kTracking, a_frame);
				}
			}
			a_bucket.buildRecords.clear();
			if (a_bucket.errorSlot != kInvalidIndex) {
				_freeErrorSlots.push_back(a_bucket.errorSlot);
				a_bucket.errorSlot = kInvalidIndex;
			}
			a_bucket.anchor = nullptr;  // the anchor reference belonged to its record
			return;
		}

		if (previous != BucketState::kActive) {
			return;
		}

		for (std::uint32_t i = 0; i < a_bucket.memberCount; ++i) {
			auto* const slot = Member(a_bucket.firstMember + i);
			auto* const record = slot->record.exchange(nullptr, std::memory_order_acq_rel);
			if (!record) {
				continue;
			}
			{
				std::scoped_lock lock{ record->lock };
				record->stableSinceFrame = a_frame;
			}
			auto expected = ObjectState::kMember;
			if (record->state.compare_exchange_strong(expected, ObjectState::kTracking, std::memory_order_acq_rel)) {
				record->bucket.store(kInvalidIndex, std::memory_order_relaxed);
				record->member.store(kInvalidIndex, std::memory_order_relaxed);
			}
		}
		_activeMembers -= std::min(_activeMembers, a_bucket.memberCount);
	}

	void BucketManager::FreeRetired(std::uint32_t a_frame)
	{
		for (auto& storage : _bucketStorage) {
			auto& bucket = *storage;
			if (bucket.state.load(std::memory_order_acquire) != BucketState::kRetiring || a_frame - bucket.retireFrame < kRetireDelay) {
				continue;
			}

			_vertices.Free(bucket.vertexOffset, bucket.vertexBytes);
			_indices.Free(bucket.indexOffset * 4, static_cast<std::uint64_t>(bucket.indexCount) * 4);
			_members.Free(static_cast<std::uint64_t>(bucket.firstMember) * sizeof(GpuMember), static_cast<std::uint64_t>(bucket.memberCount) * sizeof(GpuMember));
			for (std::uint32_t i = 0; i < bucket.memberCount; ++i) {
				auto* const slot = Member(bucket.firstMember + i);
				slot->record.store(nullptr, std::memory_order_relaxed);
				slot->bucket = kInvalidIndex;
				slot->gpu = {};
			}

			if (bucket.anchor) {
				{
					std::unique_lock lock{ _anchorLock };
					if (const auto it = _anchors.find(bucket.anchor); it != _anchors.end() && it->second == &bucket) {
						_anchors.erase(it);
					}
				}
				RefObject(bucket.anchor)->DecRefCount();
			}
			bucket.anchor = nullptr;
			bucket.anchorProperty = nullptr;
			bucket.anchorIndexBuffer = nullptr;
			bucket.memberCount = 0;
			bucket.state.store(BucketState::kFree, std::memory_order_release);
			_freeBuckets.push_back(&bucket);
		}
	}

	void BucketManager::RetireAll(std::uint32_t a_frame)
	{
		for (auto& storage : _bucketStorage) {
			const auto state = storage->state.load(std::memory_order_acquire);
			if (state == BucketState::kActive || state == BucketState::kBuilding) {
				Retire(*storage, a_frame);
			}
		}
		_freeGroups.clear();
		for (std::uint32_t index = 0; index < _groups.size(); ++index) {
			auto& group = _groups[index];
			for (auto* record : group.records) {
				ReleaseRecord(*record, ObjectState::kTracking, a_frame);
			}
			group.records.clear();
			group.alive = false;
			_freeGroups.push_back(index);
		}
		_groupsByKey.clear();
	}

	void BucketManager::Update(ID3D11DeviceContext* a_context, ObjectRegistry&, std::uint32_t a_frame)
	{
		_stats.mergedThisFrame = 0;

		PollReadbacks(a_context, a_frame);
		CheckActive(a_frame);
		FreeRetired(a_frame);
		CleanGroups(a_frame);
		StartBuilds(a_context, a_frame);
		PruneBuilds(a_frame);
		ProcessBuilds(a_context, a_frame);

		_stats.pendingGroups = 0;
		_stats.pendingObjects = 0;
		for (const auto& group : _groups) {
			if (group.alive) {
				++_stats.pendingGroups;
				_stats.pendingObjects += static_cast<std::uint32_t>(group.records.size());
			}
		}
		_stats.building = static_cast<std::uint32_t>(_buildQueue.size());
		_stats.active = 0;
		_stats.retiring = 0;
		for (const auto& storage : _bucketStorage) {
			const auto state = storage->state.load(std::memory_order_relaxed);
			_stats.active += state == BucketState::kActive ? 1 : 0;
			_stats.retiring += state == BucketState::kRetiring ? 1 : 0;
		}
		_stats.activeMembers = _activeMembers;
		_stats.vertexArenaUsed = _vertices.Used();
		_stats.indexArenaUsed = _indices.Used();
	}
}
