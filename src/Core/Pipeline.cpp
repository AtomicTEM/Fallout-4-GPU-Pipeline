#include "Core/Pipeline.h"

#include "Engine/EngineHooks.h"
#include "Engine/Layouts.h"
#include "Render/D3DHooks.h"
#include "Render/InputLayouts.h"
#include "Settings.h"
#include "Util/Math.h"

namespace GWP
{
	namespace
	{
		// Returned for batched objects: an empty pass list, exactly what the
		// renderer sees for an object that has nothing to draw.
		RenderPassArray g_emptyPasses{};

		inline constexpr std::uint32_t kGarbageInterval = 120;
		inline constexpr std::uint32_t kGarbageAge = 1800;
		inline constexpr std::uint32_t kRejectedRetry = 3600;

		[[nodiscard]] bool TransformMatches(const RE::NiTransform& a_world, const RE::NiBound& a_model, const RE::NiBound& a_worldBound, bool a_transposed) noexcept
		{
			const auto affine = Math::Affine::FromNiTransform(a_world, a_transposed);
			const double in[3]{ a_model.center.x, a_model.center.y, a_model.center.z };
			double out[3]{};
			affine.Apply(in, out);
			const double tolerance = 0.05 + 1e-4 * (std::abs(out[0]) + std::abs(out[1]) + std::abs(out[2]));
			return std::abs(out[0] - a_worldBound.center.x) <= tolerance &&
			       std::abs(out[1] - a_worldBound.center.y) <= tolerance &&
			       std::abs(out[2] - a_worldBound.center.z) <= tolerance;
		}

		// RendererData::GetSingleton() resolves its ID through CommonLibF4RD's
		// fatal path, which would end the game; resolve it softly instead.
		[[nodiscard]] RE::BSGraphics::RendererData* FindRendererData()
		{
			const auto result = REL::IDDatabase::get().resolve(REL::ID(1235449, 2704429));
			if (!result) {
				logger::error("pipeline: renderer data unresolved: {} {}", REL::id_resolve_status_text(result.status), result.note);
				return nullptr;
			}
			return *reinterpret_cast<RE::BSGraphics::RendererData**>(REL::Module::get().base() + *result.rva);
		}

		[[nodiscard]] bool HasRotation(const RE::NiTransform& a_world) noexcept
		{
			const auto& r = a_world.rotate.entry;
			return std::abs(r[0].pt[1]) > 0.05F || std::abs(r[0].pt[2]) > 0.05F || std::abs(r[1].pt[2]) > 0.05F ||
			       std::abs(r[1].pt[0]) > 0.05F || std::abs(r[2].pt[0]) > 0.05F || std::abs(r[2].pt[1]) > 0.05F;
		}
	}

	Pipeline::ThreadState& Pipeline::TLS() noexcept
	{
		thread_local ThreadState state;
		return state;
	}

	// ------------------------------------------------------------------------
	// lifecycle

	bool Pipeline::InstallEarlyHooks()
	{
		if (!EngineHooks::Install()) {
			return false;
		}
		_triShapeVTable = EngineHooks::TriShapeVTable();
		_hooksInstalled = true;

		if (!D3DHooks::InstallCreateDeviceHooks()) {
			logger::warn("pipeline: D3D11 device creation imports not found; device hooks will be installed late");
		}
		return true;
	}

	void Pipeline::InitializeRenderer()
	{
		std::scoped_lock initLock{ _initLock };
		if (_ready.load(std::memory_order_acquire) || !_hooksInstalled) {
			return;
		}

		const auto& settings = Settings::Get();
		auto* const renderer = FindRendererData();
		if (!renderer || !renderer->device || !renderer->context) {
			logger::error("pipeline: renderer data is not available");
			return;
		}

		_device = renderer->device;
		_immediate = renderer->context;
		D3DHooks::InstallObjectHooks(renderer->device, renderer->context, renderer->renderWindow[0].swapChain);
		if (!D3DHooks::HookedBeforeRendererInit()) {
			logger::warn("pipeline: Direct3D hooks were installed after renderer start-up; objects using input layouts created earlier cannot be batched");
		}

		Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
		Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
		DXGI_ADAPTER_DESC adapterDesc{};
		if (SUCCEEDED(_device->QueryInterface(IID_PPV_ARGS(dxgiDevice.GetAddressOf()))) &&
			SUCCEEDED(dxgiDevice->GetAdapter(adapter.GetAddressOf())) &&
			SUCCEEDED(adapter->GetDesc(&adapterDesc))) {
			char name[128]{};
			::WideCharToMultiByte(CP_UTF8, 0, adapterDesc.Description, -1, name, static_cast<int>(sizeof(name)) - 1, nullptr, nullptr);
			logger::info("pipeline: adapter \"{}\" vendor 0x{:04X}, feature level 0x{:X}", name, adapterDesc.VendorId, static_cast<std::uint32_t>(_device->GetFeatureLevel()));
		}

		if (settings.mode == PipelineMode::kBatch) {
			if (_device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) {
				logger::error("pipeline: feature level 11_0 is required for batching; running in observe mode");
			} else if (!_shaders.Initialize(_device) || !_buckets.Initialize(_device, _shaders) ||
					   !_batches.Initialize(_device, _shaders, _buckets) || !_hiz.Initialize(_device, _shaders)) {
				logger::error("pipeline: GPU resources could not be created; running in observe mode");
				_buckets.Shutdown();
				_batches.Release();
				_hiz.Release();
				Settings::Get().mode = PipelineMode::kObserve;
			}
		}

		_renderThread = std::this_thread::get_id();
		_ready.store(true, std::memory_order_release);
		logger::info("pipeline: ready ({} mode); batching starts once calibration passes", Settings::Get().mode == PipelineMode::kBatch ? "batch"sv : "observe"sv);
	}

	// ------------------------------------------------------------------------
	// helpers

	bool Pipeline::QuickEligible(RE::BSGeometry* a_geometry) noexcept
	{
		auto& self = Get();
		if (!a_geometry || Engine::VTableOf(a_geometry) != self._triShapeVTable) {
			return false;
		}
		if (Engine::Geometry::SkinInstance(a_geometry)) {
			return false;
		}

		namespace VF = Engine::VertexFlags;
		const auto flags = Engine::Geometry::VertexDescFlags(Engine::Geometry::VertexDesc(a_geometry));
		if ((flags & VF::kVertex) == 0 || (flags & (VF::kSkinned | VF::kLandData | VF::kEyeData)) != 0) {
			return false;
		}

		const auto* const alpha = Engine::Geometry::AlphaProperty(a_geometry);
		return !alpha || !Engine::AlphaState::Read(alpha).Blended();
	}

	bool Pipeline::Fading(RE::BSGeometry* a_geometry) const noexcept
	{
		if (Engine::AsAVObject(a_geometry)->fadeAmount < 0.999F) {
			return true;
		}
		const auto* const property = Engine::Geometry::ShaderProperty(a_geometry);
		return property && property->alpha < 0.999F;
	}

	ObjectRecord* Pipeline::Track(RE::BSGeometry* a_geometry, std::uint32_t a_frame)
	{
		auto* record = _registry.FindOrCreate(a_geometry, a_frame);
		if (record && !record->Matches(a_geometry)) {
			// The address now holds a different object, or this one changed its
			// mesh/material. Forget the old record and start over.
			Evict(*record, a_frame);
			record = _registry.FindOrCreate(a_geometry, a_frame);
		}
		return record;
	}

	void Pipeline::Evict(ObjectRecord& a_record, std::uint32_t a_frame)
	{
		const auto previous = a_record.state.exchange(ObjectState::kEvicted, std::memory_order_acq_rel);
		if (previous == ObjectState::kEvicted) {
			return;
		}

		if (previous == ObjectState::kMember) {
			if (auto* const slot = _buckets.Member(a_record.member.load(std::memory_order_relaxed))) {
				slot->record.store(nullptr, std::memory_order_release);
			}
			if (auto* const bucket = _buckets.Get(a_record.bucket.load(std::memory_order_relaxed))) {
				bucket->evictions.fetch_add(1, std::memory_order_relaxed);
			}
		}
		_registry.Remove(&a_record, a_frame);
		_evictions.Add();
	}

	void Pipeline::UpdateStability(ObjectRecord& a_record, RE::BSGeometry* a_geometry, std::uint32_t a_frame)
	{
		if (!a_record.lock.try_lock()) {
			return;  // another thread is updating it this frame
		}
		std::scoped_lock lock{ std::adopt_lock, a_record.lock };

		const auto* const object = Engine::AsAVObject(a_geometry);
		if (!Math::NearlyEqual(object->world, a_record.world)) {
			a_record.world = object->world;
			a_record.worldBound = object->worldBound;
			a_record.stableSinceFrame = a_frame;
			return;
		}

		if (a_frame - a_record.stableSinceFrame < Settings::Get().stableFrames) {
			return;
		}

		a_record.worldBound = object->worldBound;
		a_record.numVertices = Engine::Geometry::NumVertices(a_geometry);
		a_record.numTriangles = Engine::Geometry::NumTriangles(a_geometry);
		a_record.alpha = Engine::AlphaState::Read(Engine::Geometry::AlphaProperty(a_geometry));
		a_record.shadowCaster = object->ShadowCaster();

		auto expected = ObjectState::kTracking;
		a_record.state.compare_exchange_strong(expected, ObjectState::kCandidate, std::memory_order_acq_rel);
	}

	RenderPassArray* Pipeline::CallNormal(GetRenderPasses_t a_original, RE::BSShaderProperty* a_property, RE::BSGeometry* a_geometry, std::uint32_t a_mode, RE::BSShaderAccumulator* a_accumulator)
	{
		auto& tls = TLS();
		tls.lastGeometry = a_geometry;
		tls.lastDecision = Decision::kNormal;
		tls.lastBucket = nullptr;

		auto* const passes = a_original(a_property, a_geometry, a_mode, a_accumulator);
		if (passes && passes->passList && (!_calibration.PassGeometryOffset() || (++tls.sampleCounter & 15) == 0)) {
			_calibration.SamplePass(passes->passList, a_geometry);
		}

		tls.lastGeometry = a_geometry;
		tls.lastDecision = Decision::kNormal;
		return passes;
	}

	// ------------------------------------------------------------------------
	// engine hooks

	RenderPassArray* Pipeline::OnGetRenderPasses(PassKind a_kind, GetRenderPasses_t a_original, RE::BSShaderProperty* a_property, RE::BSGeometry* a_geometry, std::uint32_t a_mode, RE::BSShaderAccumulator* a_accumulator)
	{
		if (!Ready() || !QuickEligible(a_geometry)) {
			return a_original(a_property, a_geometry, a_mode, a_accumulator);
		}

		const auto frame = _frame.load(std::memory_order_relaxed);
		_registrations.Add();
		if (!_calibration.Complete() || (TLS().sampleCounter & 63) == 0) {
			_calibration.SampleRenderMode(a_kind, a_mode);
		}

		auto* const record = Track(a_geometry, frame);
		if (!record) {
			return CallNormal(a_original, a_property, a_geometry, a_mode, a_accumulator);
		}
		record->lastSeenFrame.store(frame, std::memory_order_relaxed);

		// Only registrations in a view whose StartAccumulating was seen are ever
		// batched; calibration uses this to judge where the passes are drawn.
		const auto* const queue = _views.Find(a_accumulator);
		const bool started = queue && queue->epoch.load(std::memory_order_relaxed) != 0;
		(started ? record->startedViewFrame : record->otherQueueFrame).store(frame, std::memory_order_relaxed);

		const auto state = record->state.load(std::memory_order_acquire);
		if (state == ObjectState::kTracking) {
			UpdateStability(*record, a_geometry, frame);
		}

		// Is this object part of a live batch?
		Bucket* bucket = nullptr;
		std::uint32_t member = kInvalidIndex;
		if (state == ObjectState::kMember) {
			bucket = _buckets.Get(record->bucket.load(std::memory_order_relaxed));
			member = record->member.load(std::memory_order_relaxed);
		} else if (auto* const anchored = _buckets.FindByAnchor(a_geometry)) {
			// An anchor whose record was replaced still belongs to its batch.
			bucket = anchored;
			member = anchored->firstMember;
		}
		if (!bucket || bucket->state.load(std::memory_order_acquire) != BucketState::kActive) {
			return CallNormal(a_original, a_property, a_geometry, a_mode, a_accumulator);
		}

		// Is batching enabled for this kind of view?
		const auto& settings = Settings::Get();
		const bool viewKindEnabled = a_kind == PassKind::kShadow ? settings.batchShadows : (settings.batchMainView && _calibration.MainViewAllowed());
		if (!BatchingActive() || !viewKindEnabled || !_calibration.ModeAllowed(a_kind, a_mode) || Fading(bucket->anchor)) {
			return CallNormal(a_original, a_property, a_geometry, a_mode, a_accumulator);
		}

		const bool isAnchor = a_geometry == bucket->anchor;
		if (!isAnchor) {
			// Moved or fading members are drawn by the engine on their own.
			if (!Math::NearlyEqual(Engine::AsAVObject(a_geometry)->world, record->world)) {
				if (state == ObjectState::kMember) {
					Evict(*record, frame);
				}
				return CallNormal(a_original, a_property, a_geometry, a_mode, a_accumulator);
			}
			if (Fading(a_geometry)) {
				return CallNormal(a_original, a_property, a_geometry, a_mode, a_accumulator);
			}
		} else if (!Math::NearlyEqual(Engine::AsAVObject(a_geometry)->world, bucket->anchorWorld)) {
			bucket->retireRequested.store(true, std::memory_order_relaxed);
		}

		auto* const view = _views.Acquire(a_accumulator);
		const auto epoch = view ? view->epoch.load(std::memory_order_acquire) : 0;
		if (epoch == 0) {
			// StartAccumulating was never seen for this accumulator: no batching in it.
			_calibration.SampleRegistrationBeforeStart();
			return CallNormal(a_original, a_property, a_geometry, a_mode, a_accumulator);
		}

		if (!_views.Append(*view, member)) {
			// The view's list is full. Other members can simply draw themselves,
			// but the anchor must still pass the gate so its passes are never
			// registered twice; retire the batch so this cannot repeat.
			if (!isAnchor) {
				return CallNormal(a_original, a_property, a_geometry, a_mode, a_accumulator);
			}
			bucket->drawAnomalies.fetch_add(100, std::memory_order_relaxed);
		}

		// The first registration of the batch in this view carries the anchor's
		// passes; every other one returns nothing.
		auto& gate = bucket->gates[_views.IndexOf(view)];
		auto previous = gate.load(std::memory_order_relaxed);
		bool carrier = false;
		while (previous != epoch) {
			if (gate.compare_exchange_weak(previous, epoch, std::memory_order_acq_rel, std::memory_order_relaxed)) {
				carrier = true;
				break;
			}
		}

		auto& tls = TLS();
		if (!carrier) {
			tls.lastGeometry = a_geometry;
			tls.lastDecision = Decision::kSuppressed;
			tls.lastBucket = bucket;
			_suppressed.Add();
			return &g_emptyPasses;
		}

		// Nested depth-pass requests made while building the anchor's passes
		// belong to the anchor itself.
		tls.lastGeometry = bucket->anchor;
		tls.lastDecision = Decision::kNormal;
		auto* const passes = a_original(bucket->anchorProperty, bucket->anchor, a_mode, a_accumulator);

		tls.lastGeometry = a_geometry;
		tls.lastDecision = isAnchor ? Decision::kNormal : Decision::kCarrier;
		tls.lastBucket = bucket;
		if (!passes || !passes->passList) {
			// Nothing will draw the members that were already suppressed.
			bucket->drawAnomalies.fetch_add(100, std::memory_order_relaxed);
		}
		_carriers.Add();
		return passes;
	}

	RE::BSRenderPass* Pipeline::OnGetRenderDepthPass(GetRenderDepthPass_t a_original, RE::BSShaderProperty* a_property, RE::BSGeometry* a_geometry)
	{
		if (!Ready()) {
			return a_original(a_property, a_geometry);
		}

		// Pair the request with the registration made immediately before it on
		// this thread. The decision is consumed so a stale one (from another
		// view or frame) can never suppress an unrelated depth pass.
		auto& tls = TLS();
		if (tls.lastGeometry == a_geometry && a_geometry) {
			tls.lastGeometry = nullptr;
			switch (tls.lastDecision) {
			case Decision::kSuppressed:
				_calibration.SampleDepthPass(true, false);
				return nullptr;
			case Decision::kCarrier:
				{
					auto* const bucket = tls.lastBucket;
					auto* const pass = a_original(bucket->anchorProperty, bucket->anchor);
					_calibration.SampleDepthPass(true, pass != nullptr);
					return pass;
				}
			default:
				{
					auto* const pass = a_original(a_property, a_geometry);
					_calibration.SampleDepthPass(true, pass != nullptr);
					return pass;
				}
			}
		}

		auto* const pass = a_original(a_property, a_geometry);
		if (QuickEligible(a_geometry)) {
			_calibration.SampleDepthPass(false, pass != nullptr);
		}
		return pass;
	}

	void Pipeline::OnStartAccumulating(RE::BSShaderAccumulator* a_accumulator)
	{
		if (!Ready()) {
			return;
		}
		auto* const view = _views.Acquire(a_accumulator);
		if (!view) {
			return;
		}
		const auto capacity = std::max<std::uint32_t>(_buckets.TotalActiveMembers() * 2 + 4096, 4096);
		_views.Begin(*view, capacity, _frame.load(std::memory_order_relaxed));
		view->starts.fetch_add(1, std::memory_order_relaxed);
	}

	void Pipeline::OnFinishBegin(RE::BSShaderAccumulator* a_accumulator, FinishKind a_kind)
	{
		_activeFinishes.fetch_add(1, std::memory_order_relaxed);
		auto& tls = TLS();
		if (tls.depth < tls.views.size()) {
			auto* const view = Ready() ? _views.Find(a_accumulator) : nullptr;
			tls.views[tls.depth] = { view, view ? view->epoch.load(std::memory_order_acquire) : 0, a_kind };
			if (view) {
				view->finishes.fetch_add(1, std::memory_order_relaxed);
			}
		}
		++tls.depth;
	}

	void Pipeline::OnFinishEnd(RE::BSShaderAccumulator*, FinishKind a_kind)
	{
		auto& tls = TLS();
		if (tls.depth == 0) {
			return;
		}
		_activeFinishes.fetch_sub(1, std::memory_order_relaxed);
		--tls.depth;
		if (tls.depth >= tls.views.size()) {
			return;
		}

		const auto& frame = tls.views[tls.depth];
		if (!frame.view || frame.view != _mainView.load(std::memory_order_relaxed) || a_kind == FinishKind::kPostResolveDepth) {
			return;
		}
		if (Settings::Get().mode != PipelineMode::kBatch || !Settings::Get().occlusionCulling || !BatchingActive()) {
			return;
		}
		if (_hizBuiltFrame == _frame.load(std::memory_order_relaxed) || std::this_thread::get_id() != _renderThread) {
			return;
		}
		BuildHiZ(_immediate.Get(), *frame.view);
	}

	void Pipeline::OnSetupGeometry(ShaderKind a_kind, RE::BSRenderPass* a_pass)
	{
		auto& tls = TLS();
		tls.inPass = true;
		tls.passDrawn = false;
		tls.passShader = a_kind;
		tls.passGeometry = nullptr;

		if (!Ready() || !a_pass) {
			return;
		}
		const auto offset = _calibration.PassGeometryOffset();
		if (!offset) {
			return;
		}

		auto* const geometry = Engine::Field<RE::BSGeometry*>(a_pass, *offset);
		if (!QuickEligible(geometry)) {
			return;
		}
		tls.passGeometry = geometry;

		const bool inside = tls.depth > 0 && tls.depth <= tls.views.size() && tls.views[tls.depth - 1].view;
		const bool renderThread = std::this_thread::get_id() == _renderThread;
		if (inside) {
			_calibration.SampleSetup(SetupPlace::kInside, renderThread, false);
		} else {
			// Passes drawn outside a started view only matter for objects that
			// were queued in one (this frame or, if the frame turned meanwhile,
			// the previous one).
			const auto frame = _frame.load(std::memory_order_relaxed);
			const auto recent = [frame](std::uint32_t a_stamp) { return a_stamp != 0 && a_stamp + 1 >= frame; };
			auto place = SetupPlace::kOutsideOther;
			if (const auto* const record = _registry.Find(geometry); record && recent(record->startedViewFrame.load(std::memory_order_relaxed))) {
				place = recent(record->otherQueueFrame.load(std::memory_order_relaxed)) ? SetupPlace::kOutsideAmbiguous : SetupPlace::kOutsideBatchable;
			}
			const bool finishElsewhere = _activeFinishes.load(std::memory_order_relaxed) > tls.depth;
			_calibration.SampleSetup(place, renderThread, finishElsewhere);
		}
		if (inside) {
			auto* const view = tls.views[tls.depth - 1].view;
			if (a_kind == ShaderKind::kLighting) {
				++view->lightingSetups;
			} else {
				++view->utilitySetups;
			}
		}
	}

	void Pipeline::OnRestoreGeometry(ShaderKind, RE::BSRenderPass*)
	{
		auto& tls = TLS();
		if (tls.inPass && tls.passGeometry && !tls.passDrawn) {
			_calibration.SampleMissedDraw();
		}
		tls.inPass = false;
		tls.passGeometry = nullptr;
	}

	// ------------------------------------------------------------------------
	// Direct3D hooks

	bool Pipeline::OnDrawIndexed(ID3D11DeviceContext* a_context, UINT a_indexCount, UINT a_instanceCount, UINT a_startIndex, INT a_baseVertex, UINT, bool)
	{
		auto& tls = TLS();
		if (!tls.inPass || !tls.passGeometry || tls.passDrawn) {
			return false;
		}
		tls.passDrawn = true;
		_calibration.SampleObservedDraw();

		auto* const geometry = tls.passGeometry;
		const bool batchMode = Settings::Get().mode == PipelineMode::kBatch;

		// Is this the anchor of a batch? Then the batch is drawn instead.
		if (batchMode) {
			if (auto* const bucket = _buckets.FindByAnchor(geometry)) {
				const auto state = bucket->state.load(std::memory_order_acquire);
				if (state == BucketState::kActive || state == BucketState::kRetiring) {
					return DrawBatch(a_context, *bucket, tls, a_indexCount, a_instanceCount, a_startIndex, a_baseVertex);
				}
			}
		}

		// Confirm the NiTransform convention (worldBound == world * modelBound)
		// on a sample of the objects the engine draws.
		if (!_calibration.Complete() && (++tls.sampleCounter & 7) == 0) {
			const auto* const object = Engine::AsAVObject(geometry);
			const auto& model = Engine::Field<RE::NiBound>(geometry, Engine::Offsets::kGeometryModelBound);
			if (HasRotation(object->world) && std::abs(model.center.x) + std::abs(model.center.y) + std::abs(model.center.z) > 4.0F) {
				_calibration.SampleTransform(TransformMatches(object->world, model, object->worldBound, false), TransformMatches(object->world, model, object->worldBound, true));
			}
		}

		// Otherwise capture the engine's buffers for objects waiting to be merged.
		if (batchMode && tls.passShader == ShaderKind::kLighting) {
			auto* const record = _registry.Find(geometry);
			if (record && record->state.load(std::memory_order_acquire) == ObjectState::kCandidate && record->Matches(geometry)) {
				Capture(a_context, *record, a_indexCount, a_instanceCount, a_startIndex, a_baseVertex);
			}
		}
		return false;
	}

	bool Pipeline::DrawBatch(ID3D11DeviceContext* a_context, Bucket& a_bucket, ThreadState& a_tls, UINT a_indexCount, UINT a_instanceCount, UINT a_startIndex, INT a_baseVertex)
	{
		if (a_tls.depth == 0 || a_tls.depth > a_tls.views.size() || !a_tls.views[a_tls.depth - 1].view) {
			_calibration.SampleAnchorOutsideView();
			return false;
		}

		const auto& frame = a_tls.views[a_tls.depth - 1];
		auto& view = *frame.view;
		if (a_bucket.gates[_views.IndexOf(&view)].load(std::memory_order_acquire) != frame.epoch) {
			return false;  // the batch was not used in this view; the anchor draws itself
		}

		if (a_instanceCount != 1) {
			a_bucket.drawAnomalies.fetch_add(1, std::memory_order_relaxed);
			return false;
		}

		if (Settings::Get().validateDraws) {
			Microsoft::WRL::ComPtr<ID3D11Buffer> indexBuffer;
			DXGI_FORMAT format{};
			UINT offset{};
			a_context->IAGetIndexBuffer(indexBuffer.GetAddressOf(), &format, &offset);
			if (indexBuffer.Get() != a_bucket.anchorIndexBuffer || a_indexCount != a_bucket.anchorIndexCount ||
				a_startIndex != a_bucket.anchorStartIndex || a_baseVertex != a_bucket.anchorBaseVertex) {
				a_bucket.drawAnomalies.fetch_add(1, std::memory_order_relaxed);
			}
		}

		if (view.preparedEpoch != frame.epoch) {
			const auto& settings = Settings::Get();
			const auto currentFrame = _frame.load(std::memory_order_relaxed);
			const bool occlusion = settings.occlusionCulling && &view == _mainView.load(std::memory_order_relaxed) && !_cameraCut && _hiz.UsableFor(currentFrame);

			OcclusionInput input{ &_hiz, settings.occlusionDepthBias };
			view.preparedEpoch = frame.epoch;
			if (!_batches.PrepareView(a_context, _views.Items(view), frame.epoch, occlusion ? &input : nullptr)) {
				++_failedDraws;
				return false;
			}
		}

		if (_batches.DrawBucket(a_context, a_bucket, frame.epoch)) {
			++_replacedDraws;
			return true;
		}

		a_bucket.drawAnomalies.fetch_add(1, std::memory_order_relaxed);
		++_failedDraws;
		return false;
	}

	void Pipeline::Capture(ID3D11DeviceContext* a_context, ObjectRecord& a_record, UINT a_indexCount, UINT a_instanceCount, UINT a_startIndex, INT a_baseVertex)
	{
		const auto frame = _frame.load(std::memory_order_relaxed);
		const auto reject = [&](ObjectState a_state) {
			auto expected = ObjectState::kCandidate;
			if (a_record.state.compare_exchange_strong(expected, a_state, std::memory_order_acq_rel)) {
				std::scoped_lock lock{ a_record.lock };
				a_record.stableSinceFrame = frame;
			}
			++_rejectedCaptures;
		};

		DrawCapture capture;
		ID3D11Buffer* indexBuffer = nullptr;
		a_context->IAGetIndexBuffer(&indexBuffer, &capture.indexFormat, &capture.indexOffset);
		capture.indexBuffer.Attach(indexBuffer);

		ID3D11Buffer* vertexBuffer = nullptr;
		a_context->IAGetVertexBuffers(0, 1, &vertexBuffer, &capture.stride, &capture.vertexOffset);
		capture.vertexBuffer.Attach(vertexBuffer);

		Microsoft::WRL::ComPtr<ID3D11InputLayout> layout;
		a_context->IAGetInputLayout(layout.GetAddressOf());
		capture.layout = layout.Get();

		capture.indexCount = a_indexCount;
		capture.startIndex = a_startIndex;
		capture.baseVertex = a_baseVertex;

		const auto expectedStride = static_cast<UINT>(a_record.vertexDesc & 0xF) * 4;
		if (a_instanceCount != 1 || !capture.indexBuffer || !capture.vertexBuffer || !capture.layout ||
			(capture.indexFormat != DXGI_FORMAT_R16_UINT && capture.indexFormat != DXGI_FORMAT_R32_UINT) ||
			a_indexCount == 0 || a_indexCount != a_record.numTriangles * 3 || capture.stride != expectedStride ||
			a_record.numVertices == 0) {
			reject(ObjectState::kRejected);
			return;
		}

		if (!InputLayouts::Get().Find(capture.layout)) {
			_calibration.SampleUnknownLayout();
			reject(ObjectState::kRejected);
			return;
		}

		a_record.capture = std::move(capture);
		auto expected = ObjectState::kCandidate;
		if (!a_record.state.compare_exchange_strong(expected, ObjectState::kCaptured, std::memory_order_acq_rel)) {
			a_record.capture = {};
			return;
		}

		++_captures;
		_buckets.AddCaptured(a_record, frame);
	}

	std::optional<bool> Pipeline::DetectReversedZ(const RE::NiCamera* a_camera)
	{
		const auto& matrix = Engine::Camera::WorldToCam(a_camera);
		const auto& world = Engine::Camera::World(a_camera);
		const auto nearPlane = std::max(Engine::Camera::Near(a_camera), 1.0F);
		const auto farPlane = std::max(Engine::Camera::Far(a_camera), nearPlane * 4.0F);

		// Probe along every axis of the camera frame; the view direction is the
		// one where clip w grows with distance.
		for (std::uint32_t axis = 0; axis < 3; ++axis) {
			for (const double sign : { 1.0, -1.0 }) {
				const double direction[3]{
					sign * world.rotate.entry[0].pt[axis],
					sign * world.rotate.entry[1].pt[axis],
					sign * world.rotate.entry[2].pt[axis]
				};
				const auto probe = [&](double a_distance) {
					return Math::MulRowMajor(matrix,
						world.translate.x + direction[0] * a_distance,
						world.translate.y + direction[1] * a_distance,
						world.translate.z + direction[2] * a_distance);
				};

				const auto nearClip = probe(nearPlane * 2.0);
				const auto farClip = probe(std::min<double>(farPlane * 0.5, 100000.0));
				if (nearClip[3] <= 0.0 || farClip[3] <= nearClip[3]) {
					continue;
				}
				const auto nearDepth = nearClip[2] / nearClip[3];
				const auto farDepth = farClip[2] / farClip[3];
				if (nearDepth < -0.01 || nearDepth > 1.01 || farDepth < -0.01 || farDepth > 1.01 || std::abs(farDepth - nearDepth) < 1e-6) {
					continue;
				}
				return nearDepth > farDepth;
			}
		}
		return std::nullopt;
	}

	void Pipeline::BuildHiZ(ID3D11DeviceContext* a_context, ViewState& a_view)
	{
		_hizBuiltFrame = _frame.load(std::memory_order_relaxed);

		const auto* const accumulator = a_view.accumulator.load(std::memory_order_acquire);
		const auto* const camera = accumulator ? accumulator->camera : nullptr;
		if (!camera) {
			return;
		}

		const auto reversed = DetectReversedZ(camera);
		if (!reversed) {
			static bool warned = false;
			if (!warned) {
				logger::warn("hiz: could not determine the depth convention of the main camera; occlusion culling disabled");
				warned = true;
			}
			return;
		}

		const auto& position = Engine::Camera::World(camera).translate;
		const auto dx = position.x - _lastCameraPosition.x;
		const auto dy = position.y - _lastCameraPosition.y;
		const auto dz = position.z - _lastCameraPosition.z;
		const auto cut = Settings::Get().cameraCutDistance;
		_cameraCut = dx * dx + dy * dy + dz * dz > cut * cut;
		_lastCameraPosition = position;

		Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depth;
		a_context->OMGetRenderTargets(0, nullptr, depth.GetAddressOf());
		UINT viewports = 1;
		D3D11_VIEWPORT viewport{};
		a_context->RSGetViewports(&viewports, &viewport);
		if (!depth || viewports == 0) {
			return;
		}

		_hiz.Build(a_context, depth.Get(), viewport, Engine::Camera::WorldToCam(camera), *reversed, _hizBuiltFrame);
	}

	// ------------------------------------------------------------------------
	// frame maintenance

	void Pipeline::OnPresent(IDXGISwapChain*)
	{
		const auto frame = _frame.fetch_add(1, std::memory_order_acq_rel) + 1;
		if (!Ready()) {
			// Fallback if kGameDataReady has not arrived yet: initialise once the
			// renderer has presented a few frames.
			if (_hooksInstalled && frame > 30) {
				InitializeRenderer();
			}
			return;
		}
		FrameMaintenance(_immediate.Get(), frame);
	}

	void Pipeline::FrameMaintenance(ID3D11DeviceContext* a_context, std::uint32_t a_frame)
	{
		auto& settings = Settings::Get();
		_renderThread = std::this_thread::get_id();

		// Hotkey: toggle batching for A/B comparisons.
		if (settings.toggleKey != 0) {
			const bool down = (::GetAsyncKeyState(static_cast<int>(settings.toggleKey)) & 0x8000) != 0;
			if (down && !_toggleKeyDown) {
				const bool enabled = !_batchingEnabled.load(std::memory_order_relaxed);
				_batchingEnabled.store(enabled, std::memory_order_relaxed);
				logger::info("pipeline: batching {} by hotkey", enabled ? "enabled"sv : "disabled"sv);
			}
			_toggleKeyDown = down;
		}

		const bool wasAllowed = _calibration.BatchingAllowed();
		if (_calibration.Evaluate(a_frame) && wasAllowed && !_calibration.BatchingAllowed()) {
			_buckets.RetireAll(a_frame);
		}
		if (_calibration.Complete() && _calibration.TransformsTransposed() != _bucketsTransposed) {
			// Batches merged before the convention was confirmed are wrong.
			_bucketsTransposed = _calibration.TransformsTransposed();
			_buckets.SetTransposedTransforms(_bucketsTransposed);
			_buckets.RetireAll(a_frame);
			logger::warn("pipeline: NiTransform rotations are stored transposed; rebuilding batches");
		}

		// The view with the most lighting passes is the main camera.
		const ViewState* mainView = nullptr;
		std::uint32_t best = 64;
		_views.ForEach([&](ViewState& a_view) {
			if (a_view.lightingSetups > best) {
				best = a_view.lightingSetups;
				mainView = &a_view;
			}
			a_view.lightingSetups = 0;
			a_view.utilitySetups = 0;
			a_view.starts.store(0, std::memory_order_relaxed);
			a_view.finishes.store(0, std::memory_order_relaxed);
		});
		_mainView.store(mainView, std::memory_order_relaxed);
		if (!mainView) {
			_hiz.Invalidate();
		}

		if (settings.mode == PipelineMode::kBatch && !(_calibration.Complete() && !_calibration.BatchingAllowed())) {
			_buckets.Update(a_context, _registry, a_frame);
		}

		if (a_frame % kGarbageInterval == 0) {
			CollectGarbage(a_frame);
		}
		_registry.Recycle(a_frame);
		_views.Collect(a_frame);

		LogStats(a_frame);
	}

	void Pipeline::CollectGarbage(std::uint32_t a_frame)
	{
		std::vector<ObjectRecord*> stale;
		_registry.ForEach([&](ObjectRecord& a_record) {
			const auto state = a_record.state.load(std::memory_order_acquire);
			const auto unseen = a_frame - a_record.lastSeenFrame.load(std::memory_order_relaxed);
			if ((state == ObjectState::kTracking || state == ObjectState::kCandidate || state == ObjectState::kRejected) && unseen > kGarbageAge) {
				stale.push_back(&a_record);
			} else if (state == ObjectState::kRejected && a_frame - a_record.stableSinceFrame > kRejectedRetry) {
				auto expected = ObjectState::kRejected;
				a_record.state.compare_exchange_strong(expected, ObjectState::kTracking, std::memory_order_acq_rel);
			}
		});
		for (auto* record : stale) {
			_registry.Remove(record, a_frame);
		}
	}

	void Pipeline::LogStats(std::uint32_t a_frame)
	{
		const auto& settings = Settings::Get();
		if (settings.statsIntervalSeconds == 0) {
			return;
		}

		const auto now = std::chrono::steady_clock::now();
		const auto elapsed = std::chrono::duration<double>(now - _lastStats).count();
		if (elapsed < settings.statsIntervalSeconds) {
			return;
		}

		const auto frames = std::max<std::uint32_t>(a_frame - _lastStatsFrame, 1);
		const auto perFrame = [&](std::uint64_t a_value) { return static_cast<double>(a_value) / frames; };

		const auto registrations = _registrations.Take();
		const auto suppressed = _suppressed.Take();
		const auto carriers = _carriers.Take();
		const auto evictions = _evictions.Take();
		const auto& buckets = _buckets.Stats();
		const auto& batches = _batches.Stats();

		logger::info("stats: {:.1f} fps | batching {} | eligible registrations {:.0f}/frame, suppressed {:.0f}, carriers {:.0f}, replaced draws {:.0f}, failed {:.1f}",
			frames / elapsed, BatchingActive() ? "on"sv : "off"sv, perFrame(registrations), perFrame(suppressed), perFrame(carriers), perFrame(_replacedDraws), perFrame(_failedDraws));
		logger::info("stats: records {} | captures {} (rejected {}) evictions {} | groups {} ({} objects) building {} active {} ({} members) retiring {}",
			_registry.Size(), _captures, _rejectedCaptures, evictions, buckets.pendingGroups, buckets.pendingObjects, buckets.building, buckets.active, buckets.activeMembers, buckets.retiring);
		logger::info("stats: views {:.1f}/frame, work items {:.0f}/frame, multi-draw calls {:.0f}, loop draws {:.0f}, fallback views {}, layout failures {}, occlusion views {} | arena VB {} MB IB {} MB",
			perFrame(batches.views), perFrame(batches.workItems), perFrame(batches.multiDrawCalls), perFrame(batches.loopDraws), batches.fallbackViews, batches.layoutFailures, batches.occlusionViews,
			buckets.vertexArenaUsed >> 20, buckets.indexArenaUsed >> 20);

		_batches.ResetStats();
		_replacedDraws = 0;
		_failedDraws = 0;
		_captures = 0;
		_rejectedCaptures = 0;
		_lastStats = now;
		_lastStatsFrame = a_frame;
	}
}
