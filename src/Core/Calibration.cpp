#include "Core/Calibration.h"

#include "Render/D3DHooks.h"
#include "Settings.h"

namespace GWP
{
	void Calibration::SamplePass(const void* a_pass, const RE::BSGeometry* a_geometry) noexcept
	{
		if (!a_pass || !a_geometry) {
			return;
		}

		const auto* const words = static_cast<const std::uintptr_t*>(a_pass);
		const auto target = reinterpret_cast<std::uintptr_t>(a_geometry);

		const auto learned = _passGeometryOffset.load(std::memory_order_relaxed);
		if (learned >= 0) {
			// Keep verifying after learning; a contradiction revokes batching.
			const auto index = static_cast<std::size_t>(learned) / sizeof(std::uintptr_t);
			_passSamples.fetch_add(1, std::memory_order_relaxed);
			if (words[index] == target) {
				_passHits[index].fetch_add(1, std::memory_order_relaxed);
			}
			return;
		}

		for (std::size_t i = 0; i < kPassScanQwords; ++i) {
			if (words[i] == target) {
				_passHits[i].fetch_add(1, std::memory_order_relaxed);
			}
		}
		_passSamples.fetch_add(1, std::memory_order_relaxed);
	}

	void Calibration::SampleSetup(SetupPlace a_place, bool a_renderThread, bool a_finishElsewhere) noexcept
	{
		_setups[static_cast<std::size_t>(a_place)].fetch_add(1, std::memory_order_relaxed);
		if (a_place == SetupPlace::kOutsideBatchable) {
			if (!a_renderThread) {
				_batchableOnWorker.fetch_add(1, std::memory_order_relaxed);
			}
			if (a_finishElsewhere) {
				_batchableDuringFinish.fetch_add(1, std::memory_order_relaxed);
			}
		}
	}

	void Calibration::SampleRenderMode(PassKind a_kind, std::uint32_t a_mode) noexcept
	{
		_modes[static_cast<std::size_t>(a_kind)][std::min(a_mode, kModeBuckets - 1)].fetch_add(1, std::memory_order_relaxed);
	}

	void Calibration::SampleDepthPass(bool a_paired, bool a_nonNull) noexcept
	{
		(a_paired ? _depthPaired : _depthUnpaired).fetch_add(1, std::memory_order_relaxed);
		if (a_nonNull) {
			_depthNonNull.fetch_add(1, std::memory_order_relaxed);
		}
	}

	void Calibration::SampleTransform(bool a_matchesRowMajor, bool a_matchesTransposed) noexcept
	{
		_transformSamples.fetch_add(1, std::memory_order_relaxed);
		if (a_matchesRowMajor) {
			_transformRowMajor.fetch_add(1, std::memory_order_relaxed);
		}
		if (a_matchesTransposed) {
			_transformTransposed.fetch_add(1, std::memory_order_relaxed);
		}
	}

	void Calibration::Revoke(std::string_view a_reason)
	{
		if (_revoked) {
			return;
		}
		_revoked = true;
		_batchingAllowed.store(false, std::memory_order_release);
		_mainViewAllowed.store(false, std::memory_order_release);
		logger::error("calibration: batching disabled for this session: {}", a_reason);
	}

	bool Calibration::Evaluate(std::uint32_t)
	{
		const auto& settings = Settings::Get();
		const auto minSamples = std::max<std::uint32_t>(settings.calibrationMinSamples, 64);
		++_framesObserved;

		if (_revoked) {
			return false;
		}

		// Learn the BSRenderPass -> geometry offset.
		const auto samples = _passSamples.load(std::memory_order_relaxed);
		if (_passGeometryOffset.load(std::memory_order_relaxed) < 0 && samples >= minSamples) {
			std::int32_t found = -1;
			std::uint32_t candidates = 0;
			for (std::size_t i = 0; i < kPassScanQwords; ++i) {
				if (_passHits[i].load(std::memory_order_relaxed) == samples) {
					found = static_cast<std::int32_t>(i * sizeof(std::uintptr_t));
					++candidates;
				}
			}
			if (candidates == 1) {
				for (auto& hits : _passHits) {
					hits.store(0, std::memory_order_relaxed);
				}
				_passSamples.store(0, std::memory_order_relaxed);
				_passGeometryOffset.store(found, std::memory_order_release);
				logger::info("calibration: BSRenderPass geometry pointer at +0x{:X} ({} samples)", found, samples);
			} else if (samples > minSamples * 16) {
				Revoke(fmt::format("BSRenderPass layout not recognised ({} candidate offsets)", candidates));
				return true;
			}
		} else if (_passGeometryOffset.load(std::memory_order_relaxed) >= 0 && samples >= minSamples) {
			const auto index = static_cast<std::size_t>(_passGeometryOffset.load(std::memory_order_relaxed)) / sizeof(std::uintptr_t);
			const auto hits = _passHits[index].load(std::memory_order_relaxed);
			if (hits + samples / 1000 < samples) {
				Revoke(fmt::format("BSRenderPass geometry offset contradicted ({} of {} samples)", samples - hits, samples));
				return true;
			}
		}

		// Render modes that make up at least 2% of registrations are batchable.
		for (std::size_t kind = 0; kind < _modes.size(); ++kind) {
			std::uint64_t total = 0;
			for (const auto& count : _modes[kind]) {
				total += count.load(std::memory_order_relaxed);
			}
			if (total < minSamples) {
				continue;
			}
			std::uint64_t mask = 0;
			for (std::uint32_t mode = 0; mode < kModeBuckets - 1; ++mode) {
				if (_modes[kind][mode].load(std::memory_order_relaxed) * 50 >= total) {
					mask |= 1ull << mode;
				}
			}
			_allowedModes[kind].store(mask, std::memory_order_relaxed);
		}

		// NiTransform convention, from worldBound == world * modelBound.
		const auto transformSamples = _transformSamples.load(std::memory_order_relaxed);
		bool transformKnown = false;
		if (transformSamples >= 64) {
			const auto rowMajor = _transformRowMajor.load(std::memory_order_relaxed);
			const auto transposed = _transformTransposed.load(std::memory_order_relaxed);
			if (rowMajor * 100 >= transformSamples * 95) {
				_transposed = false;
				transformKnown = true;
			} else if (transposed * 100 >= transformSamples * 95) {
				_transposed = true;
				transformKnown = true;
			}
		}

		// Passes of objects that were never queued in a started view are never
		// batched, so only passes of objects that were can block batching.
		const auto inside = _setups[static_cast<std::size_t>(SetupPlace::kInside)].load(std::memory_order_relaxed);
		const auto outside = _setups[static_cast<std::size_t>(SetupPlace::kOutsideBatchable)].load(std::memory_order_relaxed);
		const auto observed = _observedDraws.load(std::memory_order_relaxed);
		const auto missed = _missedDraws.load(std::memory_order_relaxed);

		// key: the kind of reason, which decides when it is logged again
		std::string_view key;
		std::string reason;
		if (_passGeometryOffset.load(std::memory_order_relaxed) < 0) {
			key = "layout";
			reason = "learning BSRenderPass layout";
		} else if (_framesObserved < settings.calibrationMinFrames) {
			key = "frames";
			reason = "observing frames";
		} else if (inside + outside < minSamples || observed + missed < minSamples) {
			key = "waiting";
			reason = "waiting for world geometry to be drawn";
		} else if (outside * 1000 > inside + outside) {
			key = "outside";
			reason = fmt::format(
				"{} of {} passes of batchable objects were rendered outside BSShaderAccumulator::FinishAccumulating "
				"({} on worker threads, {} while another thread was inside FinishAccumulating)",
				outside, inside + outside, _batchableOnWorker.load(std::memory_order_relaxed), _batchableDuringFinish.load(std::memory_order_relaxed));
		} else if (missed * 1000 > observed + missed) {
			key = "missed";
			reason = fmt::format("{} of {} geometry passes issued no draw visible to the Direct3D hook", missed, observed + missed);
		} else if (!transformKnown) {
			key = "transform";
			reason = transformSamples < 64 ? "waiting for transform samples" : "NiTransform convention could not be confirmed";
		} else if (!D3DHooks::DeviceHooked()) {
			key = "device";
			reason = "Direct3D device hooks are not installed";
		}

		const bool allowed = reason.empty();
		const auto depthNonNull = _depthNonNull.load(std::memory_order_relaxed);
		const auto depthUnpaired = _depthUnpaired.load(std::memory_order_relaxed);
		const bool mainView = allowed && (depthNonNull == 0 || depthUnpaired * 1000 <= depthNonNull);

		// Runtime monitoring once batching is live.
		if (_batchingAllowed.load(std::memory_order_relaxed)) {
			const auto outsideNow = _anchorOutsideView.load(std::memory_order_relaxed);
			if (outsideNow > _lastAnchorOutsideView + 16) {
				Revoke(fmt::format("{} batch anchors were drawn outside an accumulator", outsideNow - _lastAnchorOutsideView));
				return true;
			}
			_lastAnchorOutsideView = outsideNow;
		}

		const bool changed = allowed != _batchingAllowed.load(std::memory_order_relaxed) || mainView != _mainViewAllowed.load(std::memory_order_relaxed);
		// Log a new kind of reason at once; the same one (with updated counts)
		// at most every 30 seconds.
		const auto now = std::chrono::steady_clock::now();
		if (!reason.empty() && (key != _blockKey || now - _blockLogged >= 30s)) {
			logger::info("calibration: batching on hold: {}", reason);
			if (key == "outside"sv) {
				LogSummary();
			}
			_blockLogged = now;
		}
		_blockKey = key;
		if (changed) {
			_mainViewAllowed.store(mainView, std::memory_order_release);
			_batchingAllowed.store(allowed, std::memory_order_release);
			if (allowed) {
				_complete = true;
				logger::info("calibration: all checks passed, batching allowed (main view: {}, transforms transposed: {})", mainView, _transposed);
				if (!mainView) {
					logger::warn("calibration: depth pre-pass requests could not be paired with registrations ({} unpaired of {}), only shadow views are batched", depthUnpaired, depthNonNull);
				}
				LogSummary();
			}
		}
		return changed;
	}

	void Calibration::LogSummary() const
	{
		logger::info("calibration: pass offset {} | setups inside view {}, outside: batchable {} ({} on worker threads, {} during another thread's FinishAccumulating), ambiguous {}, never batched {} | draws observed/missed {}/{}",
			_passGeometryOffset.load(), _setups[0].load(), _setups[1].load(), _batchableOnWorker.load(), _batchableDuringFinish.load(), _setups[2].load(), _setups[3].load(),
			_observedDraws.load(), _missedDraws.load());
		logger::info("calibration: depth passes paired/unpaired/non-null {}/{}/{} | transforms row/transposed {}/{} of {} | unknown layouts {} | registrations before start {}",
			_depthPaired.load(), _depthUnpaired.load(), _depthNonNull.load(), _transformRowMajor.load(), _transformTransposed.load(), _transformSamples.load(),
			_unknownLayouts.load(), _registrationBeforeStart.load());
		for (std::size_t kind = 0; kind < _modes.size(); ++kind) {
			std::string modes;
			for (std::uint32_t mode = 0; mode < kModeBuckets; ++mode) {
				const auto count = _modes[kind][mode].load(std::memory_order_relaxed);
				if (count) {
					modes += fmt::format(" {}:{}", mode, count);
				}
			}
			logger::info("calibration: {} render modes{} (allowed mask 0x{:X})", kind == 0 ? "main"sv : "shadow"sv, modes, _allowedModes[kind].load());
		}
	}
}
