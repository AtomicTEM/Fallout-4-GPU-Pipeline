#include "Scene/ViewRegistry.h"

namespace GWP
{
	ViewState* ViewRegistry::Acquire(const RE::BSShaderAccumulator* a_accumulator) noexcept
	{
		if (!a_accumulator) {
			return nullptr;
		}
		for (auto& view : _views) {
			const auto* current = view.accumulator.load(std::memory_order_acquire);
			if (current == a_accumulator) {
				return &view;
			}
			if (!current) {
				const RE::BSShaderAccumulator* expected = nullptr;
				if (view.accumulator.compare_exchange_strong(expected, a_accumulator, std::memory_order_acq_rel)) {
					return &view;
				}
				if (expected == a_accumulator) {
					return &view;
				}
			}
		}
		return nullptr;
	}

	ViewState* ViewRegistry::Find(const RE::BSShaderAccumulator* a_accumulator) const noexcept
	{
		for (auto& view : _views) {
			const auto* current = view.accumulator.load(std::memory_order_acquire);
			if (current == a_accumulator) {
				return const_cast<ViewState*>(&view);
			}
			if (!current) {
				break;
			}
		}
		return nullptr;
	}

	std::uint32_t ViewRegistry::Begin(ViewState& a_view, std::uint32_t a_capacity, std::uint32_t a_frame)
	{
		// The accumulator is between FinishAccumulating and the next culling
		// pass, so no worker can be appending to it right now.
		auto* storage = a_view.storage.load(std::memory_order_acquire);
		if (!storage || storage->capacity < a_capacity) {
			auto replacement = std::make_unique<ViewState::Storage>();
			replacement->capacity = std::max<std::uint32_t>(std::bit_ceil(std::max<std::uint32_t>(a_capacity, 4096)), 4096);
			replacement->items = std::make_unique<std::uint32_t[]>(replacement->capacity);
			a_view.storage.store(replacement.release(), std::memory_order_release);
			if (storage) {
				std::scoped_lock lock{ _retiredLock };
				_retired.emplace_back(a_frame, std::unique_ptr<ViewState::Storage>{ storage });
			}
		}

		a_view.count.store(0, std::memory_order_relaxed);
		a_view.overflowed.store(false, std::memory_order_relaxed);
		const auto epoch = _nextEpoch.fetch_add(1, std::memory_order_relaxed);
		a_view.epoch.store(epoch, std::memory_order_release);
		return epoch;
	}

	bool ViewRegistry::Append(ViewState& a_view, std::uint32_t a_member) noexcept
	{
		auto* const storage = a_view.storage.load(std::memory_order_acquire);
		if (!storage) {
			return false;
		}
		const auto index = a_view.count.fetch_add(1, std::memory_order_relaxed);
		if (index >= storage->capacity) {
			a_view.count.fetch_sub(1, std::memory_order_relaxed);
			a_view.overflowed.store(true, std::memory_order_relaxed);
			return false;
		}
		storage->items[index] = a_member;
		return true;
	}

	std::span<const std::uint32_t> ViewRegistry::Items(const ViewState& a_view) const noexcept
	{
		auto* const storage = a_view.storage.load(std::memory_order_acquire);
		if (!storage) {
			return {};
		}
		const auto count = std::min(a_view.count.load(std::memory_order_acquire), storage->capacity);
		return { storage->items.get(), count };
	}

	void ViewRegistry::Collect(std::uint32_t a_frame)
	{
		std::scoped_lock lock{ _retiredLock };
		while (!_retired.empty() && a_frame - _retired.front().first >= 8) {
			_retired.pop_front();
		}
	}
}
