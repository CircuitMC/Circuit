// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/runtime/executor.hpp>
#include <cstdint>
#include <map>
#include <vector>

namespace circuit::runtime {
struct ChunkCounters {
    std::uint64_t time_advances{};
    std::uint64_t signals{};
    bool operator==(const ChunkCounters&) const = default;
};
struct DemoSnapshot {
    std::uint64_t tick{};
    std::vector<ChunkCounters> chunks;
    bool operator==(const DemoSnapshot&) const = default;
};
// Synthetic scheduler fixture, NOT a Minecraft rules implementation.
// All entry points are coordinator-only. Signal delivery is conservatively serial.
class DemoKernel {
public:
    DemoKernel(unsigned workers, std::size_t chunks = 4);
    void schedule(std::uint64_t due_tick, std::size_t target, unsigned hops);
    void advance(bool reverse_submission = false, std::size_t event_budget = 100000);
    [[nodiscard]] const DemoSnapshot& snapshot() const noexcept { return state_; }
private:
    struct Signal { std::size_t target; unsigned hops; };
    using EventKey = std::pair<std::uint64_t, std::uint64_t>;
    StageExecutor executor_;
    DemoSnapshot state_;
    std::map<EventKey, Signal> pending_;
    std::uint64_t next_sequence_{};
};
}
