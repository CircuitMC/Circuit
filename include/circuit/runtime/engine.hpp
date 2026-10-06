// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/contracts/commands.hpp>
#include <circuit/contracts/trace.hpp>
#include <circuit/contracts/version.hpp>
#include <circuit/gameplay/world_types.hpp>
#include <circuit/runtime/job_runner.hpp>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

// Region-parallel simulation engine. Loaded chunk columns are partitioned
// into regions with one logical writer each. Within a phase every region runs
// its roots in the serial order; access outside the region aborts that
// attempt, rolls the affected regions back, merges them and runs them again.
// Level-wide counters (scheduled-tick sub-order, block-event order) are staged
// per region and assigned at commit in serial order. Region layout therefore
// changes throughput, never results.
namespace circuit::runtime {

enum class LayoutPolicy : std::uint8_t {
    adaptive,      // merge on conflict; split components that stopped interacting
    single_region, // every loaded chunk in one region: serial execution through the same machinery
    perturbed,     // test mode: seeded random merges and splits at tick boundaries
};

struct EngineOptions {
    unsigned workers{1};                  // used when `runner` is empty
    std::shared_ptr<JobRunner> runner;    // e.g. SeededRunner for schedule exploration
    bool record_trace{true};
    LayoutPolicy layout{LayoutPolicy::adaptive};
    std::uint64_t layout_seed{1};
    std::uint32_t split_interval{20};     // ticks between split passes
    std::uint32_t quiet_ticks{40};        // interaction-free ticks before a split
    // Deliberately unsafe test mode that skips ownership checks. It exists to
    // show that differential exploration detects missing isolation, and is
    // only accepted with a non-concurrent runner.
    bool unchecked_ownership_for_testing{false};
};

struct EngineStats {
    // Wall-clock nanoseconds spent per phase (indexed by contracts::Phase),
    // including coordination and commit.
    std::array<std::uint64_t, 7> phase_nanoseconds{};
    std::uint64_t phases{};
    std::uint64_t attempts{};
    std::uint64_t tasks{};
    std::uint64_t conflict_aborts{};
    std::uint64_t injected_aborts{};
    std::uint64_t merges{};
    std::uint64_t splits{};
    std::uint64_t max_tasks_in_phase{};
};

class Engine;

// Immutable query view of the state committed at the end of a tick. It
// reports stale_view after the engine advances; it must not outlive the engine.
class WorldView {
public:
    [[nodiscard]] contracts::Tick tick() const noexcept { return tick_; }
    [[nodiscard]] contracts::Result<gameplay::BlockState> block(contracts::BlockPos pos) const;
    [[nodiscard]] contracts::Result<std::optional<gameplay::BlockEntity>> block_entity(contracts::BlockPos pos) const;
    [[nodiscard]] contracts::Result<std::optional<gameplay::Entity>> entity(contracts::EntityId id) const;

private:
    friend class Engine;
    WorldView(const Engine* engine, std::uint64_t epoch, contracts::Tick tick) : engine_(engine), epoch_(epoch), tick_(tick) {}
    const Engine* engine_;
    std::uint64_t epoch_;
    contracts::Tick tick_;
};

class Engine {
public:
    Engine(const contracts::VersionBundle& bundle, std::uint64_t seed, EngineOptions options = {});
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Executes one logical tick. Any failure leaves the engine unusable;
    // logical ticks are never skipped or repeated.
    void advance(const std::vector<contracts::Command>& commands);
    [[nodiscard]] contracts::Tick time() const noexcept;
    [[nodiscard]] const std::vector<contracts::TraceRecord>& trace() const noexcept;
    [[nodiscard]] gameplay::WorldDump dump() const;
    [[nodiscard]] const EngineStats& stats() const noexcept;
    [[nodiscard]] std::size_t region_count() const noexcept;
    [[nodiscard]] std::size_t loaded_chunks() const noexcept;
    [[nodiscard]] WorldView view() const noexcept;

    struct Impl;

private:
    friend class WorldView;
    std::unique_ptr<Impl> impl_;
};

} // namespace circuit::runtime
