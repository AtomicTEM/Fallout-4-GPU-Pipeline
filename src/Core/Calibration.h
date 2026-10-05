#pragma once

namespace GWP
{
	enum class PassKind : std::uint32_t
	{
		kMain = 0,    // BSShaderProperty::GetRenderPasses
		kShadow = 1,  // BSShaderProperty::GetRenderPasses_ShadowMapOrMask
		kCount = 2
	};

	// Runtime verification of every renderer assumption the batching path
	// relies on. Batching stays off until each check has enough samples and
	// passes; a later violation switches it off again. Nothing here uses
	// executable addresses, so the same checks run on OG, NG and AE.
	class Calibration
	{
	public:
		static constexpr std::size_t kPassScanQwords = 8;
		static constexpr std::uint32_t kModeBuckets = 64;

		// --- sampling (any thread) -------------------------------------------

		// Learns where BSRenderPass stores its geometry pointer by looking for
		// the geometry in passes returned by GetRenderPasses.
		void SamplePass(const void* a_pass, const RE::BSGeometry* a_geometry) noexcept;

		void SampleRenderMode(PassKind a_kind, std::uint32_t a_mode) noexcept;

		void SampleSetup(bool a_insideView) noexcept { (a_insideView ? _setupInsideView : _setupOutsideView).fetch_add(1, std::memory_order_relaxed); }
		void SampleMissedDraw() noexcept { _missedDraws.fetch_add(1, std::memory_order_relaxed); }
		void SampleObservedDraw() noexcept { _observedDraws.fetch_add(1, std::memory_order_relaxed); }
		void SampleDepthPass(bool a_paired, bool a_nonNull) noexcept;
		void SampleUnknownLayout() noexcept { _unknownLayouts.fetch_add(1, std::memory_order_relaxed); }
		void SampleTransform(bool a_matchesRowMajor, bool a_matchesTransposed) noexcept;
		void SampleAnchorOutsideView() noexcept { _anchorOutsideView.fetch_add(1, std::memory_order_relaxed); }
		void SampleRegistrationBeforeStart() noexcept { _registrationBeforeStart.fetch_add(1, std::memory_order_relaxed); }
		void ReportUnhookedContextClass() noexcept { _unhookedContextClass.store(true, std::memory_order_relaxed); }

		// --- results ---------------------------------------------------------

		[[nodiscard]] std::optional<std::size_t> PassGeometryOffset() const noexcept
		{
			const auto offset = _passGeometryOffset.load(std::memory_order_acquire);
			return offset >= 0 ? std::optional<std::size_t>{ static_cast<std::size_t>(offset) } : std::nullopt;
		}

		[[nodiscard]] bool ModeAllowed(PassKind a_kind, std::uint32_t a_mode) const noexcept
		{
			return (_allowedModes[static_cast<std::uint32_t>(a_kind)].load(std::memory_order_relaxed) >> std::min(a_mode, kModeBuckets - 1)) & 1;
		}

		[[nodiscard]] bool BatchingAllowed() const noexcept { return _batchingAllowed.load(std::memory_order_acquire); }
		[[nodiscard]] bool MainViewAllowed() const noexcept { return _mainViewAllowed.load(std::memory_order_acquire); }
		[[nodiscard]] bool TransformsTransposed() const noexcept { return _transposed; }
		[[nodiscard]] bool Complete() const noexcept { return _complete; }

		// Render thread, once per frame. Returns true when the batching
		// permission changed this frame.
		bool Evaluate(std::uint32_t a_frame);

		// Render thread: hard stop after an unrecoverable inconsistency.
		void Revoke(std::string_view a_reason);

		void LogSummary() const;

	private:
		std::array<std::atomic<std::uint32_t>, kPassScanQwords> _passHits{};
		std::atomic<std::uint32_t> _passSamples{ 0 };
		std::atomic<std::int32_t> _passGeometryOffset{ -1 };

		std::array<std::array<std::atomic<std::uint32_t>, kModeBuckets>, static_cast<std::size_t>(PassKind::kCount)> _modes{};
		std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(PassKind::kCount)> _allowedModes{};

		std::atomic<std::uint32_t> _setupInsideView{ 0 };
		std::atomic<std::uint32_t> _setupOutsideView{ 0 };
		std::atomic<std::uint32_t> _missedDraws{ 0 };
		std::atomic<std::uint32_t> _observedDraws{ 0 };
		std::atomic<std::uint32_t> _depthPaired{ 0 };
		std::atomic<std::uint32_t> _depthUnpaired{ 0 };
		std::atomic<std::uint32_t> _depthNonNull{ 0 };
		std::atomic<std::uint32_t> _unknownLayouts{ 0 };
		std::atomic<std::uint32_t> _transformRowMajor{ 0 };
		std::atomic<std::uint32_t> _transformTransposed{ 0 };
		std::atomic<std::uint32_t> _transformSamples{ 0 };
		std::atomic<std::uint32_t> _anchorOutsideView{ 0 };
		std::atomic<std::uint32_t> _registrationBeforeStart{ 0 };
		std::atomic<bool> _unhookedContextClass{ false };

		std::atomic<bool> _batchingAllowed{ false };
		std::atomic<bool> _mainViewAllowed{ false };
		bool _transposed{ false };
		bool _complete{ false };
		bool _revoked{ false };
		std::uint32_t _framesObserved{ 0 };
		std::uint32_t _lastAnchorOutsideView{ 0 };
		std::string _blockReason;
	};
}
