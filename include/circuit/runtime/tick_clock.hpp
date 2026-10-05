// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <chrono>
#include <cstdint>

namespace circuit::runtime {

// Wall-clock pacing for the control loop. Logical time is independent of it:
// each executed tick advances logical time by exactly one, and no logical
// tick is ever skipped or repeated. When the loop falls behind, due ticks run
// back to back; once the backlog exceeds `max_backlog` ticks, the wall-clock
// schedule is re-anchored and the backlog is dropped instead of being caught
// up. The backlog limit models the documented 2-second limit of the target
// version and is unverified.
class TickClock {
public:
    using clock = std::chrono::steady_clock;

    explicit TickClock(clock::time_point start, clock::duration period = std::chrono::milliseconds(50),
                       std::uint32_t max_backlog = 40)
        : deadline_(start), period_(period), max_backlog_(max_backlog) {}

    // True if a tick should run at `now`.
    bool due(clock::time_point now) {
        if (now < deadline_) return false;
        const auto behind = static_cast<std::uint64_t>((now - deadline_) / period_);
        if (behind > max_backlog_) {
            dropped_ += behind;
            ++overloads_;
            deadline_ = now;
        }
        return true;
    }
    // Records that one tick ran; the next deadline is one period later.
    void ticked() { deadline_ += period_; }

    [[nodiscard]] clock::time_point deadline() const noexcept { return deadline_; }
    [[nodiscard]] std::uint64_t dropped_backlog() const noexcept { return dropped_; }
    [[nodiscard]] std::uint64_t overloads() const noexcept { return overloads_; }

private:
    clock::time_point deadline_;
    clock::duration period_;
    std::uint32_t max_backlog_;
    std::uint64_t dropped_{};
    std::uint64_t overloads_{};
};

} // namespace circuit::runtime
