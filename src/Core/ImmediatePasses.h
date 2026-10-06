#pragma once

#include "Util/SpinLock.h"

namespace GWP
{
	struct Bucket;
	struct ViewState;

	// Most world geometry is drawn by replaying a command buffer recorded for
	// each of its passes (BSRenderPass::commandBuffer). A replay never calls
	// SetupGeometry, so the pipeline could neither capture such an object's
	// buffers nor swap a batch anchor's draw for its batch. The engine draws a
	// pass without a command buffer through SetupGeometry instead: it records
	// none for some properties and draws their passes that way every frame.
	//
	// Detach() takes the buffer off the passes the pipeline needs to see (a
	// batch candidate's capture draw, a batch anchor's draws) after
	// GetRenderPasses returned them and before the engine registers them.
	// ReattachAll() puts every buffer back at the end of the frame. Nothing is
	// allocated or freed on the engine's behalf.
	//
	// It checks itself, over the whole session: if under 5% of the first 2000
	// detached passes are drawn through SetupGeometry, or the engine records new
	// buffers for more than 64 detached passes, or more than 16 detached passes
	// are freed before the end of the frame, it disables itself and
	// command-buffer objects are no longer batched.
	class ImmediatePasses
	{
	public:
		enum class Reason : std::uint32_t
		{
			kCapture,  // a batch candidate's draw, to capture its buffers
			kAnchor    // a batch anchor's draw, replaced by its batch
		};

		struct Stats
		{
			std::uint64_t captures{ 0 };        // passes detached for a capture
			std::uint64_t anchors{ 0 };         // passes detached for an anchor
			std::uint64_t drawn{ 0 };           // detached passes whose draw the D3D hook saw
			std::uint64_t undrawnAnchors{ 0 };  // anchor passes not drawn in a view that was rendered
			std::uint64_t rebuilt{ 0 };         // the engine gave a detached pass a new buffer
			std::uint64_t lost{ 0 };            // the pass no longer belonged to its geometry
		};

		// Any thread, between GetRenderPasses and the registration of its
		// passes. Walks at most a_maxPasses passes of a_geometry chained from
		// a_pass and detaches their command buffers. a_bucket and a_view (may be
		// null) identify an anchor's batch and view, to flag a batch whose
		// anchor was not drawn. Returns true if none of the walked passes
		// replays a command buffer afterwards (false when disabled and one does).
		bool Detach(void* a_pass, const RE::BSGeometry* a_geometry, Reason a_reason, Bucket* a_bucket, const ViewState* a_view, std::uint32_t a_maxPasses);

		// Render thread, from the draw hook: a_pass (the pass between
		// SetupGeometry and RestoreGeometry) was drawn.
		void NoteDrawn(const void* a_pass);

		// Render thread: a_pass is an anchor pass a carrier detached this frame.
		[[nodiscard]] bool IsAnchorPass(const void* a_pass);

		// Render thread, at Present, before view counters are reset: puts every
		// detached buffer back and checks that detaching works. Also called
		// after a plugin fault, when no other hook runs any more.
		void ReattachAll();

		[[nodiscard]] bool Enabled() const noexcept { return _enabled.load(std::memory_order_relaxed); }
		void Disable(std::string_view a_reason);

		// Any thread: claims one of a_limit capture detaches for a_frame.
		[[nodiscard]] bool ReserveCapture(std::uint32_t a_frame, std::uint32_t a_limit) noexcept;

		[[nodiscard]] Stats TakeStats() noexcept;

	private:
		struct Entry
		{
			void* pass{ nullptr };
			std::byte* buffer{ nullptr };
			const RE::BSGeometry* geometry{ nullptr };
			Bucket* bucket{ nullptr };
			const ViewState* view{ nullptr };
			bool drawn{ false };
		};

		SpinLock _lock;
		std::vector<Entry> _entries;                          // _lock
		std::unordered_map<const void*, std::size_t> _index;  // _lock: pass -> entry
		std::atomic<std::size_t> _count{ 0 };
		std::atomic_bool _enabled{ true };
		std::atomic<std::uint64_t> _budget{ 0 };  // frame << 32 | captures reserved

		Stats _stats;                     // _lock
		std::vector<Entry> _reattaching;  // render thread
		std::uint64_t _totalDetached{ 0 };
		std::uint64_t _totalDrawn{ 0 };
		std::uint64_t _totalRebuilt{ 0 };
		std::uint64_t _totalLost{ 0 };
	};
}
