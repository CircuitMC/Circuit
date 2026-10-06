// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/contracts/commands.hpp>
#include <circuit/contracts/trace.hpp>
#include <circuit/contracts/version.hpp>
#include <circuit/gameplay/world_types.hpp>
#include <circuit/runtime/job_runner.hpp>
#include <array>
#include <functional>
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
    bool measure_tasks{false}; // time every task step (profiling; adds clock reads per root)
    // A phase attempt with at most this many roots runs on the calling thread
    // (only with the built-in pool runner). 0 always uses the pool.
    std::size_t inline_roots{32};
    LayoutPolicy layout{LayoutPolicy::adaptive};
    std::uint64_t layout_seed{1};
    std::uint32_t split_interval{20};     // ticks between split passes
    std::uint32_t quiet_ticks{40};        // interaction-free ticks before a split
    // Fills the blocks of a column created for the first time (not a reloaded
    // one): `blocks(section)` yields a section's 4096 states, y-major. It must
    // be deterministic, since replays and checkpoints depend on it. Empty:
    // new columns are air.
    std::function<void(contracts::ChunkPos, const std::function<std::array<gameplay::BlockState, 4096>&(std::size_t)>&)>
        generator;
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
    // Parallel phases: wall time inside the job runner, and, with
    // EngineOptions::measure_tasks, the time tasks spent in rule code (work)
    // and the sum over runner calls of the longest task (the critical path).
    std::uint64_t runner_nanoseconds{};
    std::uint64_t work_nanoseconds{};
    std::uint64_t critical_nanoseconds{};
    std::uint64_t commit_nanoseconds{}; // serial commit of parallel phases
};

class Engine;

// Copy of one loaded chunk column's state between ticks. Sections are shared
// with the engine copy-on-write, so taking a snapshot is cheap and later
// writes do not change it; it may outlive the tick and move to other threads.
struct ColumnSnapshot {
    using Blocks = std::array<gameplay::BlockState, 4096>; // y-major, then z, then x
    contracts::ChunkPos pos;
    contracts::Generation generation;
    gameplay::ChunkLevel level{};
    std::vector<std::shared_ptr<const Blocks>> sections; // bottom to top; nullptr: all air
    std::vector<std::pair<contracts::BlockPos, gameplay::BlockEntity>> block_entities;
    std::vector<gameplay::Entity> entities;
};

// A consistent image of the whole world between ticks, for background
// checkpoints. `state` is a canonical dump without its block list; the blocks
// are `sections`, one list per entry of `state.chunks` (bottom to top, null
// for an empty section), shared copy-on-write with the engine: the engine
// copies a section before writing to it while a snapshot holds it. Taking a
// snapshot costs time in the number of columns, entities and scheduled ticks,
// not in the number of blocks. Release it on the thread that advances the
// engine, between ticks.
struct WorldSnapshot {
    gameplay::WorldDump state;
    std::int32_t min_y{};
    std::vector<std::vector<std::shared_ptr<const ColumnSnapshot::Blocks>>> sections;
};

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
    // Continues from a canonical dump (a checkpoint). The restored engine
    // behaves exactly like the one that produced the dump: its next tick yields
    // the same trace and state. Throws std::invalid_argument for a dump that is
    // inconsistent with the bundle (blocks outside known columns, invalid
    // states, duplicate ids).
    Engine(const contracts::VersionBundle& bundle, std::uint64_t seed, const gameplay::WorldDump& state,
           EngineOptions options = {});
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Executes one logical tick. Any failure leaves the engine unusable;
    // logical ticks are never skipped or repeated.
    void advance(const std::vector<contracts::Command>& commands);
    [[nodiscard]] contracts::Tick time() const noexcept;
    [[nodiscard]] const std::vector<contracts::TraceRecord>& trace() const noexcept;
    [[nodiscard]] gameplay::WorldDump dump() const;
    [[nodiscard]] WorldSnapshot snapshot() const;
    [[nodiscard]] const EngineStats& stats() const noexcept;
    [[nodiscard]] std::size_t region_count() const noexcept;
    [[nodiscard]] std::size_t loaded_chunks() const noexcept;
    [[nodiscard]] WorldView view() const noexcept;
    // Snapshot of a loaded column; nullopt when the column is not loaded.
    // Must not be called while advance() runs.
    [[nodiscard]] std::optional<ColumnSnapshot> column(contracts::ChunkPos pos) const;
    // Loaded columns and their levels, in coordinate order.
    [[nodiscard]] std::vector<std::pair<contracts::ChunkPos, gameplay::ChunkLevel>> loaded_columns() const;

    struct Impl;

private:
    friend class WorldView;
    std::unique_ptr<Impl> impl_;
};

} // namespace circuit::runtime
