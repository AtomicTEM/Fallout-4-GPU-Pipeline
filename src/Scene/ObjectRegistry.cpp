#include "Scene/ObjectRegistry.h"

namespace GWP
{
	bool ObjectRecord::Matches(RE::BSGeometry* a_geometry) const noexcept
	{
		return geometry == a_geometry &&
		       rendererData == Engine::Geometry::RendererData(a_geometry) &&
		       property == Engine::Geometry::ShaderProperty(a_geometry) &&
		       vertexDesc == Engine::Geometry::VertexDesc(a_geometry);
	}

	ObjectRecord* ObjectRegistry::Find(const RE::BSGeometry* a_geometry) const
	{
		const auto& shard = _shards[ShardOf(a_geometry)];
		std::shared_lock lock{ shard.lock };
		const auto it = shard.map.find(a_geometry);
		return it != shard.map.end() ? it->second : nullptr;
	}

	ObjectRecord* ObjectRegistry::FindOrCreate(RE::BSGeometry* a_geometry, std::uint32_t a_frame)
	{
		if (auto* existing = Find(a_geometry)) {
			return existing;
		}

		auto& shard = _shards[ShardOf(a_geometry)];
		std::unique_lock lock{ shard.lock };
		if (const auto it = shard.map.find(a_geometry); it != shard.map.end()) {
			return it->second;
		}

		auto* const record = Allocate();
		if (!record) {
			return nullptr;
		}

		const auto* const object = Engine::AsAVObject(a_geometry);
		record->geometry = a_geometry;
		record->rendererData = Engine::Geometry::RendererData(a_geometry);
		record->property = Engine::Geometry::ShaderProperty(a_geometry);
		record->vertexDesc = Engine::Geometry::VertexDesc(a_geometry);
		record->world = object->world;
		record->worldBound = object->worldBound;
		record->numVertices = Engine::Geometry::NumVertices(a_geometry);
		record->numTriangles = Engine::Geometry::NumTriangles(a_geometry);
		record->alpha = Engine::AlphaState::Read(Engine::Geometry::AlphaProperty(a_geometry));
		record->shadowCaster = object->ShadowCaster();
		record->firstSeenFrame = a_frame;
		record->stableSinceFrame = a_frame;
		record->lastSeenFrame.store(a_frame, std::memory_order_relaxed);
		record->state.store(ObjectState::kTracking, std::memory_order_release);

		shard.map.emplace(a_geometry, record);
		return record;
	}

	void ObjectRegistry::Remove(ObjectRecord* a_record, std::uint32_t a_frame)
	{
		auto& shard = _shards[ShardOf(a_record->geometry)];
		{
			std::unique_lock lock{ shard.lock };
			const auto it = shard.map.find(a_record->geometry);
			if (it == shard.map.end() || it->second != a_record) {
				return;
			}
			shard.map.erase(it);
		}

		std::scoped_lock lock{ _poolLock };
		_retired.emplace_back(a_frame, a_record);
	}

	void ObjectRegistry::Recycle(std::uint32_t a_frame)
	{
		std::scoped_lock lock{ _poolLock };
		while (!_retired.empty() && a_frame - _retired.front().first >= kRecycleDelay) {
			auto* const record = _retired.front().second;
			_retired.pop_front();

			record->geometry = nullptr;
			record->rendererData = nullptr;
			record->property = nullptr;
			record->capture = {};
			record->pendingGroup = kInvalidIndex;
			record->holdsReference = false;
			record->bucket.store(kInvalidIndex, std::memory_order_relaxed);
			record->member.store(kInvalidIndex, std::memory_order_relaxed);
			_free.push_back(record);
		}
	}

	ObjectRecord* ObjectRegistry::Allocate()
	{
		std::scoped_lock lock{ _poolLock };
		if (!_free.empty()) {
			auto* const record = _free.back();
			_free.pop_back();
			return record;
		}

		if (_chunkUsed == kChunkSize) {
			_chunks.emplace_back(std::make_unique<ObjectRecord[]>(kChunkSize));
			_chunkUsed = 0;
		}
		return &_chunks.back()[_chunkUsed++];
	}

	std::size_t ObjectRegistry::Size() const
	{
		std::size_t size = 0;
		for (const auto& shard : _shards) {
			std::shared_lock lock{ shard.lock };
			size += shard.map.size();
		}
		return size;
	}
}
