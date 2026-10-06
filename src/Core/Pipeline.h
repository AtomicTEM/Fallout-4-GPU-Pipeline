#pragma once

#include "Core/Calibration.h"
#include "Core/ImmediatePasses.h"
#include "Render/BatchRenderer.h"
#include "Render/HiZ.h"
#include "Render/ShaderLibrary.h"
#include "Scene/BucketManager.h"
#include "Scene/ObjectRegistry.h"
#include "Scene/ViewRegistry.h"
#include "Util/Counters.h"

namespace GWP
{
	enum class ShaderKind : std::uint32_t
	{
		kLighting,  // BSLightingShader: forward passes
		kUtility,   // BSUtilityShader: depth-only, shadow map and mask passes
		kPrePass    // BSDFPrePassShader: deferred G-buffer passes (most world geometry)
	};

	// Passes that draw an object's surface (as opposed to depth or shadows).
	[[nodiscard]] constexpr bool IsSurfacePass(ShaderKind a_kind) noexcept
	{
		return a_kind != ShaderKind::kUtility;
	}

	enum class FinishKind : std::uint32_t
	{
		kFinish,
		kPreResolveDepth,
		kPostResolveDepth
	};

	using RenderPassArray = RE::BSShaderProperty::RenderPassArray;
	using GetRenderPasses_t = RenderPassArray* (*)(RE::BSShaderProperty*, RE::BSGeometry*, std::uint32_t, RE::BSShaderAccumulator*);
	using GetRenderDepthPass_t = RE::BSRenderPass* (*)(RE::BSShaderProperty*, RE::BSGeometry*);

	// Owns every subsystem and receives every hook. Engine hooks may run on the
	// renderer's job threads; D3D hooks and frame maintenance run on the
	// render thread.
	class Pipeline
	{
	public:
		[[nodiscard]] static Pipeline& Get() noexcept
		{
			static Pipeline singleton;
			return singleton;
		}

		// F4SE plugin load: engine vtable hooks and device creation hooks.
		bool InstallEarlyHooks();

		// F4SE kGameDataReady: renderer exists, create GPU resources.
		void InitializeRenderer();

		[[nodiscard]] bool Ready() const noexcept { return _ready.load(std::memory_order_acquire); }

		// --- engine hooks ---------------------------------------------------
		RenderPassArray* OnGetRenderPasses(PassKind a_kind, GetRenderPasses_t a_original, RE::BSShaderProperty* a_property, RE::BSGeometry* a_geometry, std::uint32_t a_mode, RE::BSShaderAccumulator* a_accumulator);
		RE::BSRenderPass* OnGetRenderDepthPass(GetRenderDepthPass_t a_original, RE::BSShaderProperty* a_property, RE::BSGeometry* a_geometry);
		void OnStartAccumulating(RE::BSShaderAccumulator* a_accumulator);
		void OnFinishBegin(RE::BSShaderAccumulator* a_accumulator, FinishKind a_kind);
		void OnFinishEnd(RE::BSShaderAccumulator* a_accumulator, FinishKind a_kind);
		void OnSetupGeometry(ShaderKind a_kind, RE::BSRenderPass* a_pass);
		void OnRestoreGeometry(ShaderKind a_kind, RE::BSRenderPass* a_pass);

		// --- Direct3D hooks -------------------------------------------------
		// Returns true when the draw was replaced and must not be forwarded.
		bool OnDrawIndexed(ID3D11DeviceContext* a_context, UINT a_indexCount, UINT a_instanceCount, UINT a_startIndex, INT a_baseVertex, UINT a_startInstance, bool a_instanced);
		void OnPresent(IDXGISwapChain* a_swapChain);
		// Present after a plugin fault: only gives back detached command buffers.
		void ReattachAfterFault() noexcept;
		void ReportUnhookedContextClass() noexcept { _calibration.ReportUnhookedContextClass(); }

	private:
		enum class Decision : std::uint32_t
		{
			kNormal,      // engine renders the object itself
			kSuppressed,  // drawn by its batch; no passes returned
			kCarrier      // returned the anchor's passes on behalf of the batch
		};

		struct ViewFrame
		{
			ViewState* view{ nullptr };
			std::uint32_t epoch{ 0 };
			FinishKind kind{ FinishKind::kFinish };
		};

		struct ThreadState
		{
			std::array<ViewFrame, 8> views{};
			std::uint32_t depth{ 0 };

			const void* pass{ nullptr };  // between SetupGeometry and RestoreGeometry
			RE::BSGeometry* passGeometry{ nullptr };
			ShaderKind passShader{ ShaderKind::kLighting };
			bool inPass{ false };
			bool passDrawn{ false };

			RE::BSGeometry* lastGeometry{ nullptr };
			Decision lastDecision{ Decision::kNormal };
			Bucket* lastBucket{ nullptr };

			std::uint32_t sampleCounter{ 0 };
		};

		static ThreadState& TLS() noexcept;

		[[nodiscard]] static bool QuickEligible(RE::BSGeometry* a_geometry) noexcept;
		[[nodiscard]] ObjectRecord* Track(RE::BSGeometry* a_geometry, std::uint32_t a_frame);
		// Lets the engine build the object's passes. a_detachForCapture routes
		// passes that would replay a command buffer through SetupGeometry for
		// this frame, so the capture sees the draw.
		[[nodiscard]] RenderPassArray* CallNormal(GetRenderPasses_t a_original, RE::BSShaderProperty* a_property, RE::BSGeometry* a_geometry, std::uint32_t a_mode, RE::BSShaderAccumulator* a_accumulator, ObjectRecord* a_record = nullptr, bool a_detachForCapture = false);
		// Walks the passes the engine returned for a_geometry: records shader
		// statistics and whether any pass replays a command buffer. Returns true
		// if one does.
		bool InspectPasses(ObjectRecord* a_record, const RenderPassArray* a_passes, const RE::BSGeometry* a_geometry) noexcept;
		[[nodiscard]] bool CommandBuffersDetectable() const noexcept;
		// Objects drawn from command buffers can be captured and anchor batches.
		[[nodiscard]] bool CommandBufferObjectsBatchable() const noexcept;
		// Whether this registration of a command-buffer candidate should be
		// drawn through SetupGeometry so its buffers can be captured.
		[[nodiscard]] bool WantsCaptureDraw(ObjectRecord& a_record, PassKind a_kind, std::uint32_t a_mode, bool a_started, std::uint32_t a_frame);
		// Routes a batch anchor's depth pass through SetupGeometry, or retires
		// the batch if that is not possible.
		void DetachAnchorDepthPass(Bucket& a_bucket, RE::BSRenderPass* a_pass);
		void SampleReplayDraw(ID3D11DeviceContext* a_context);
		void RebuildMeshMap(std::uint32_t a_frame);
		void UpdateStability(ObjectRecord& a_record, RE::BSGeometry* a_geometry, std::uint32_t a_frame);
		void Evict(ObjectRecord& a_record, std::uint32_t a_frame);
		[[nodiscard]] bool Fading(RE::BSGeometry* a_geometry) const noexcept;

		void Capture(ID3D11DeviceContext* a_context, ObjectRecord& a_record, UINT a_indexCount, UINT a_instanceCount, UINT a_startIndex, INT a_baseVertex);
		bool DrawBatch(ID3D11DeviceContext* a_context, Bucket& a_bucket, ThreadState& a_tls, UINT a_indexCount, UINT a_instanceCount, UINT a_startIndex, INT a_baseVertex);
		void BuildHiZ(ID3D11DeviceContext* a_context, ViewState& a_view);
		[[nodiscard]] static std::optional<bool> DetectReversedZ(const RE::NiCamera* a_camera);
		void FrameMaintenance(ID3D11DeviceContext* a_context, std::uint32_t a_frame);
		void CollectGarbage(std::uint32_t a_frame);
		void LogStats(std::uint32_t a_frame);
		void LogObjectDiagnostics();

		[[nodiscard]] bool BatchingActive() const noexcept
		{
			return _batchingEnabled.load(std::memory_order_relaxed) && _calibration.BatchingAllowed();
		}

		// threads currently inside a hooked FinishAccumulating
		std::atomic<std::uint32_t> _activeFinishes{ 0 };

		std::mutex _initLock;
		bool _hooksInstalled{ false };
		std::atomic_bool _ready{ false };
		std::atomic_bool _batchingEnabled{ true };
		std::atomic<std::uint32_t> _frame{ 1 };

		std::uintptr_t _triShapeVTable{ 0 };

		ID3D11Device* _device{ nullptr };
		Microsoft::WRL::ComPtr<ID3D11DeviceContext> _immediate;

		Calibration _calibration;
		ImmediatePasses _immediatePasses;
		bool _immediatePassesEnabled{ true };  // render thread
		ObjectRegistry _registry;
		ViewRegistry _views;
		ShaderLibrary _shaders;
		BucketManager _buckets;
		BatchRenderer _batches;
		HiZ _hiz;

		// main view (largest number of lighting passes last frame)
		std::atomic<const ViewState*> _mainView{ nullptr };
		std::uint32_t _hizBuiltFrame{ 0 };
		RE::NiPoint3 _lastCameraPosition;
		bool _cameraCut{ true };

		bool _toggleKeyDown{ false };
		bool _bucketsTransposed{ false };
		std::chrono::steady_clock::time_point _lastStats{ std::chrono::steady_clock::now() };
		std::uint32_t _lastStatsFrame{ 0 };

		// per-interval counters
		StripedCounter _registrations;
		StripedCounter _suppressed;
		StripedCounter _carriers;
		StripedCounter _evictions;
		StripedCounter _captureBudgetSkips;  // capture draws deferred to a later frame
		StripedCounter _captureGiveUps;      // candidates whose capture draw never came
		// Diagnostics for the stats lines (reset with them): how often tracked
		// objects were found moved, and one example of a change and of a
		// transform that matched neither convention.
		struct TransformChange
		{
			RE::NiTransform before;
			RE::NiTransform after;
		};
		struct TransformMismatch
		{
			RE::NiTransform world;
			RE::NiBound model;
			RE::NiBound worldBound;
			bool stable{ false };
		};
		std::atomic<std::uint64_t> _stabilityChecks{ 0 };
		std::atomic<std::uint64_t> _stabilityResets{ 0 };
		std::atomic<std::uint64_t> _promotions{ 0 };
		SpinLock _exampleLock;
		std::optional<TransformChange> _changeExample;   // _exampleLock
		std::optional<TransformMismatch> _mismatchExample;  // render thread
		// command-buffer diagnostics (any thread unless noted; reset with the stats)
		std::atomic<std::uint64_t> _passesInspected{ 0 };
		std::atomic<std::uint64_t> _passesWithCommandBuffer{ 0 };
		std::atomic<std::uint64_t> _passWalkMismatches{ 0 };
		std::array<std::atomic<std::uint64_t>, 4> _passShaders{};  // prepass, lighting, utility, other
		std::array<std::atomic<std::uintptr_t>, 4> _otherShaderVTables{};
		std::uint64_t _replayDraws{ 0 };       // render thread
		std::uint64_t _replaySamples{ 0 };     // render thread
		std::uint64_t _replayMatched{ 0 };     // render thread
		std::uint64_t _replayAmbiguous{ 0 };   // render thread
		std::uint64_t _replayUnmatched{ 0 };   // render thread

		// (vertex buffer, index buffer) -> number of recently drawn objects
		// using them; render thread, rebuilt periodically.
		struct MeshKeyHash
		{
			std::size_t operator()(const std::pair<void*, void*>& a_key) const noexcept
			{
				return std::hash<void*>{}(a_key.first) ^ (std::hash<void*>{}(a_key.second) << 1);
			}
		};
		std::unordered_map<std::pair<void*, void*>, std::uint32_t, MeshKeyHash> _meshMap;
		std::uint32_t _meshVertexBuffers{ 0 };
		std::uint32_t _meshPooled{ 0 };
		std::uint32_t _meshDescMismatches{ 0 };

		std::uint64_t _lightingDraws{ 0 };           // render thread
		std::uint64_t _utilityDraws{ 0 };            // render thread
		std::uint64_t _transformSamplesAll{ 0 };     // render thread
		std::uint64_t _transformMatchesAll{ 0 };     // render thread

		std::uint64_t _replacedDraws{ 0 };   // render thread
		std::uint64_t _failedDraws{ 0 };     // render thread
		std::uint64_t _captures{ 0 };        // render thread
		std::uint64_t _rejectedCaptures{ 0 };  // render thread
		std::thread::id _renderThread;
	};
}
