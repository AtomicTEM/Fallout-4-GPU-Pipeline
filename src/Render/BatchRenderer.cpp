#include "Render/BatchRenderer.h"

#include "Render/HiZ.h"
#include "Render/InputLayouts.h"
#include "Render/NvApi.h"
#include "Render/ShaderConstants.h"
#include "Render/ShaderLibrary.h"
#include "Render/StateGuards.h"
#include "Scene/BucketManager.h"

namespace GWP
{
	namespace
	{
		using CullConstants = ShaderConstants::Cull;

		using ShaderConstants::kArgsStride;
		inline constexpr std::uint32_t kArgsRecords = 1u << 20;
		inline constexpr std::uint32_t kMaxGroupsPerDimension = 65535;

		[[nodiscard]] std::pair<UINT, UINT> SplitGroups(std::uint32_t a_groups) noexcept
		{
			const auto x = std::min(std::max(a_groups, 1u), kMaxGroupsPerDimension);
			return { x, (a_groups + x - 1) / x };
		}
	}

	bool BatchRenderer::Initialize(ID3D11Device* a_device, ShaderLibrary& a_shaders, BucketManager& a_buckets)
	{
		_device = a_device;
		_buckets = &a_buckets;
		const auto& settings = Settings::Get();

		switch (settings.indirectMode) {
		case IndirectMode::kAuto:
			_mode = NvApi::Get().Initialize() ? IndirectMode::kNvMultiDraw : IndirectMode::kCompaction;
			break;
		case IndirectMode::kNvMultiDraw:
			_mode = NvApi::Get().Initialize() ? IndirectMode::kNvMultiDraw : IndirectMode::kCompaction;
			break;
		default:
			_mode = settings.indirectMode;
			break;
		}
		logger::info("batches: indirect mode {}", _mode == IndirectMode::kNvMultiDraw ? "NVAPI multi-draw"sv : (_mode == IndirectMode::kDrawLoop ? "draw loop"sv : "index compaction"sv));

		_compact = a_shaders.CompileCompute("Cull.hlsl", "CSCompact");
		_multiDraw = a_shaders.CompileCompute("Cull.hlsl", "CSMultiDraw");
		_constants = CreateConstantBuffer(a_device, sizeof(CullConstants));
		if (!_compact || !_multiDraw || !_constants) {
			return false;
		}

		if (!_work.Create(a_device, D3D11_BIND_SHADER_RESOURCE, 8, 8 * 65536, "cull work")) {
			return false;
		}

		ArenaBuffer::Desc args;
		args.name = "indirect args";
		args.bindFlags = D3D11_BIND_UNORDERED_ACCESS;
		args.miscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		args.initialBytes = static_cast<std::uint64_t>(kArgsRecords) * kArgsStride;
		args.maxBytes = args.initialBytes;
		args.uav = true;
		if (!_args.Create(a_device, args)) {
			return false;
		}
		_argsCapacity = static_cast<std::uint32_t>(_args.Size() / kArgsStride);

		ArenaBuffer::Desc ring;
		ring.name = "compaction ring";
		ring.bindFlags = D3D11_BIND_INDEX_BUFFER | D3D11_BIND_UNORDERED_ACCESS;
		ring.miscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		ring.initialBytes = static_cast<std::uint64_t>(settings.ringIndexMB) << 20;
		ring.maxBytes = ring.initialBytes;
		ring.uav = true;
		if (!_ring.Create(a_device, ring)) {
			return false;
		}
		_ringCapacity = _ring.Size() / 4;
		return true;
	}

	void BatchRenderer::BeginFrame()
	{
		// Cursors wrap instead of resetting: GPU commands on one context run in
		// order, so overwriting a region always happens after its last reader.
	}

	bool BatchRenderer::PrepareView(ID3D11DeviceContext* a_context, std::span<const std::uint32_t> a_members, std::uint32_t a_epoch, const OcclusionInput* a_occlusion)
	{
		_currentEpoch = a_epoch;
		_viewBuckets.clear();

		// Pass 1: find the batches present in this view.
		std::uint64_t indexTotal = 0;
		std::uint32_t memberTotal = 0;
		for (const auto id : a_members) {
			const auto* const slot = _buckets->Member(id);
			if (!slot || slot->bucket == kInvalidIndex) {
				continue;
			}
			auto* const bucket = _buckets->Get(slot->bucket);
			const auto state = bucket ? bucket->state.load(std::memory_order_acquire) : BucketState::kFree;
			if (state != BucketState::kActive && state != BucketState::kRetiring) {
				continue;
			}
			if (bucket->viewEpoch != a_epoch) {
				bucket->viewEpoch = a_epoch;
				bucket->viewMembers = 0;
				bucket->viewIndexTotal = 0;
				bucket->viewDrawCount = 0;
				_viewBuckets.push_back(bucket);
			}
			++bucket->viewMembers;
			bucket->viewIndexTotal += slot->gpu.indexCount;
			indexTotal += slot->gpu.indexCount;
			++memberTotal;
		}
		if (_viewBuckets.empty()) {
			return true;
		}

		const bool perMember = _mode != IndirectMode::kCompaction || indexTotal > _ringCapacity;
		if (_mode == IndirectMode::kCompaction && perMember) {
			++_stats.fallbackViews;
		}
		_currentMode = perMember ? ViewMode::kPerMember : ViewMode::kCompaction;

		const auto recordsNeeded = perMember ? memberTotal : static_cast<std::uint32_t>(_viewBuckets.size());
		if (recordsNeeded > _argsCapacity) {
			logger::error("batches: view needs {} indirect records (capacity {})", recordsNeeded, _argsCapacity);
			for (auto* bucket : _viewBuckets) {
				bucket->viewEpoch = 0;
			}
			return false;
		}
		const auto argsBase = _argsCursor + recordsNeeded > _argsCapacity ? 0 : _argsCursor;
		_argsCursor = argsBase + recordsNeeded;

		// Pass 2: assign indirect records (and ring ranges for compaction).
		std::uint64_t ringBase = 0;
		if (!perMember) {
			ringBase = _ringCursor + indexTotal > _ringCapacity ? 0 : _ringCursor;
			_ringCursor = ringBase + indexTotal;
			_argsInit.resize(_viewBuckets.size() * 5);
		}

		std::uint64_t ringOffset = ringBase;
		std::uint32_t record = argsBase;
		for (std::size_t i = 0; i < _viewBuckets.size(); ++i) {
			auto* const bucket = _viewBuckets[i];
			bucket->viewCompacted = !perMember;
			bucket->viewSlot = record;
			if (perMember) {
				record += bucket->viewMembers;
				bucket->viewDrawCount = 0;  // used as a cursor in pass 3
			} else {
				_argsInit[i * 5 + 0] = 0;  // IndexCountPerInstance, accumulated on the GPU
				_argsInit[i * 5 + 1] = 1;  // InstanceCount
				_argsInit[i * 5 + 2] = static_cast<std::uint32_t>(ringOffset);
				_argsInit[i * 5 + 3] = 0;  // BaseVertexLocation
				_argsInit[i * 5 + 4] = 0;  // StartInstanceLocation
				ringOffset += bucket->viewIndexTotal;
				++record;
			}
		}

		// Pass 3: work items, grouped per batch for per-member records.
		_workItems.assign(static_cast<std::size_t>(memberTotal) * 2, 0);
		std::uint32_t cursor = 0;
		for (const auto id : a_members) {
			const auto* const slot = _buckets->Member(id);
			if (!slot || slot->bucket == kInvalidIndex) {
				continue;
			}
			auto* const bucket = _buckets->Get(slot->bucket);
			if (!bucket || bucket->viewEpoch != a_epoch) {
				continue;
			}

			std::uint32_t position = 0;
			if (perMember) {
				position = bucket->viewSlot - argsBase + bucket->viewDrawCount++;
			} else {
				position = cursor++;
			}
			_workItems[position * 2 + 0] = id;
			_workItems[position * 2 + 1] = bucket->viewSlot;
		}

		if (!_work.Upload(a_context, _workItems.data(), memberTotal * 8)) {
			return false;
		}
		if (!perMember) {
			D3D11_BOX box{};
			box.left = argsBase * kArgsStride;
			box.right = box.left + static_cast<UINT>(_viewBuckets.size()) * kArgsStride;
			box.bottom = 1;
			box.back = 1;
			a_context->UpdateSubresource(_args.Buffer(), 0, &box, _argsInit.data(), 0, 0);
		}

		CullConstants constants{};
		const bool occlusion = a_occlusion && a_occlusion->hiz;
		if (occlusion) {
			const auto& hiz = *a_occlusion->hiz;
			std::memcpy(constants.viewProj, hiz.ViewProj(), sizeof(constants.viewProj));
			constants.hizSize[0] = static_cast<float>(hiz.Width());
			constants.hizSize[1] = static_cast<float>(hiz.Height());
			constants.hizSize[2] = 1.0F / static_cast<float>(hiz.Width());
			constants.hizSize[3] = 1.0F / static_cast<float>(hiz.Height());
			constants.occlusion = 1;
			constants.reversedZ = hiz.ReversedZ() ? 1 : 0;
			constants.hizMips = hiz.Mips();
			constants.depthBias = a_occlusion->depthBias;
			++_stats.occlusionViews;
		}
		constants.workCount = memberTotal;
		constants.argsBase = argsBase;

		UINT groupsX = 1;
		UINT groupsY = 1;
		if (perMember) {
			std::tie(groupsX, groupsY) = SplitGroups((memberTotal + 63) / 64);
		} else {
			std::tie(groupsX, groupsY) = SplitGroups(memberTotal);
		}
		constants.groupsX = groupsX;

		{
			ComputeStateGuard guard{ a_context };
			ID3D11Buffer* const cb = _constants.Get();
			UploadConstants(a_context, cb, constants);

			ID3D11ShaderResourceView* const srvs[4]{
				_buckets->MemberTable().SRV(),
				_work.SRV(),
				_buckets->IndexArena().SRV(),
				occlusion ? a_occlusion->hiz->SRV() : nullptr
			};
			ID3D11UnorderedAccessView* const uavs[2]{ _args.UAV(), perMember ? nullptr : _ring.UAV() };

			a_context->CSSetShader(perMember ? _multiDraw.Get() : _compact.Get(), nullptr, 0);
			a_context->CSSetConstantBuffers(0, 1, &cb);
			a_context->CSSetShaderResources(0, 4, srvs);
			a_context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
			a_context->Dispatch(groupsX, groupsY, 1);
		}

		++_stats.views;
		_stats.workItems += memberTotal;
		return true;
	}

	bool BatchRenderer::DrawBucket(ID3D11DeviceContext* a_context, Bucket& a_bucket, std::uint32_t a_epoch)
	{
		if (a_bucket.viewEpoch != a_epoch || a_bucket.viewMembers == 0) {
			return false;
		}

		InputAssemblerGuard ia{ a_context };
		auto* const layout = InputLayouts::Get().GetWidenedVariant(_device, ia.Layout(), a_bucket.plan.positionBytes, a_bucket.plan.shift);
		if (!layout) {
			++_stats.layoutFailures;
			return false;
		}

		ID3D11Buffer* const vertices = _buckets->VertexArena().Buffer();
		const UINT stride = a_bucket.plan.dstStride;
		const UINT offset = static_cast<UINT>(a_bucket.vertexOffset);
		a_context->IASetInputLayout(layout);
		a_context->IASetVertexBuffers(0, 1, &vertices, &stride, &offset);

		if (a_bucket.viewCompacted) {
			a_context->IASetIndexBuffer(_ring.Buffer(), DXGI_FORMAT_R32_UINT, 0);
			a_context->DrawIndexedInstancedIndirect(_args.Buffer(), a_bucket.viewSlot * kArgsStride);
		} else {
			a_context->IASetIndexBuffer(_buckets->IndexArena().Buffer(), DXGI_FORMAT_R32_UINT, 0);
			const bool multiDrawn = _mode == IndirectMode::kNvMultiDraw &&
			                        NvApi::Get().MultiDrawIndexedInstancedIndirect(a_context, a_bucket.viewDrawCount, _args.Buffer(), a_bucket.viewSlot * kArgsStride, kArgsStride);
			if (multiDrawn) {
				++_stats.multiDrawCalls;
			} else {
				for (std::uint32_t i = 0; i < a_bucket.viewDrawCount; ++i) {
					a_context->DrawIndexedInstancedIndirect(_args.Buffer(), (a_bucket.viewSlot + i) * kArgsStride);
				}
				_stats.loopDraws += a_bucket.viewDrawCount;
			}
		}

		++_stats.batchDraws;
		return true;
	}
}
