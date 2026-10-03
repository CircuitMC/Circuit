// SPDX-License-Identifier: AGPL-3.0-only
#include <circuit/runtime/demo_kernel.hpp>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace circuit::runtime {
DemoKernel::DemoKernel(unsigned workers, std::size_t chunks)
    : executor_(workers), state_{0, std::vector<ChunkCounters>(chunks)} {
    if (chunks == 0) throw std::invalid_argument("at least one chunk is required");
}
void DemoKernel::schedule(std::uint64_t due_tick, std::size_t target, unsigned hops) {
    if (due_tick <= state_.tick || target >= state_.chunks.size())
        throw std::invalid_argument("event must target an existing chunk in a future tick");
    if (next_sequence_ == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("event sequence exhausted");
    pending_.emplace(EventKey{due_tick, next_sequence_}, Signal{target, hops});
    ++next_sequence_;
}
void DemoKernel::advance(bool reverse_submission, std::size_t event_budget) {
    if (state_.tick == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("tick exhausted");
    auto staged = state_;
    auto events = pending_;
    auto sequence = next_sequence_;
    ++staged.tick;
    std::vector<std::function<void()>> jobs;
    for (std::size_t i = 0; i < staged.chunks.size(); ++i)
        jobs.emplace_back([&staged, i] { ++staged.chunks[i].time_advances; });
    if (reverse_submission) std::reverse(jobs.begin(), jobs.end());
    executor_.submit(std::move(jobs)).get();

    std::size_t delivered = 0;
    while (!events.empty() && events.begin()->first.first <= staged.tick) {
        // Abort the unpublished tick as a whole: never silently drop events.
        if (delivered++ >= event_budget) throw std::runtime_error("event budget exceeded; tick not published");
        const auto signal = events.begin()->second;
        events.erase(events.begin());
        auto& counter = staged.chunks[signal.target].signals;
        if (counter == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("signal counter exhausted");
        ++counter;
        if (signal.hops > 0) {
            if (sequence == std::numeric_limits<std::uint64_t>::max())
                throw std::overflow_error("event sequence exhausted");
            events.emplace(EventKey{staged.tick, sequence++},
                Signal{(signal.target + 1) % staged.chunks.size(), signal.hops - 1});
        }
    }
    state_ = std::move(staged);
    pending_ = std::move(events);
    next_sequence_ = sequence;
}
}
