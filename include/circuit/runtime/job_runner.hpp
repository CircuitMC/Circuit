// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <circuit/runtime/executor.hpp>
#include <cstdint>
#include <optional>
#include <span>

namespace circuit::runtime {

// A unit of phase work that runs to completion in steps. Tasks never wait
// for each other; a runner may interleave their steps in any order.
class Task {
public:
    virtual ~Task() = default;
    // Executes one step; returns false once the task has finished.
    virtual bool step() = 0;
    // Test hook: abandon the task as if it had hit a conflict. The engine
    // rolls it back and runs it again.
    virtual void inject_abort() = 0;
};

class JobRunner {
public:
    virtual ~JobRunner() = default;
    // Runs every task to completion. `attempt` counts re-runs within a phase.
    virtual void run(std::span<Task* const> tasks, std::uint32_t attempt) = 0;
    // True if tasks may execute simultaneously on different threads.
    [[nodiscard]] virtual bool concurrent() const noexcept = 0;
};

// Runs tasks on worker threads: up to `workers` threads, the calling thread
// included, take tasks from a shared index until none is left. A single task,
// or a single worker, runs on the calling thread, and a runner of one worker
// starts no thread. Workers never wait for other pool jobs.
//
// run() returns only when no task is executing any more. When a task throws,
// the thread it ran on takes no further task, the other threads finish the
// tasks they can still take, and run() then rethrows the first exception; the
// tasks nobody took have not run. The runner can be used again afterwards.
// run() is called by one thread at a time, the engine's coordinator.
class PoolRunner final : public JobRunner {
public:
    explicit PoolRunner(unsigned workers);
    void run(std::span<Task* const> tasks, std::uint32_t attempt) override;
    [[nodiscard]] bool concurrent() const noexcept override { return workers_ > 1; }

private:
    std::optional<StageExecutor> pool_; // the workers - 1 helper threads; empty at one worker
    unsigned workers_;
};

// Deterministic schedule exploration on the calling thread. A seed fully
// determines the interleaving of task steps and any injected aborts, so a
// failure is reproduced by rerunning with the same seed.
struct ExplorationOptions {
    enum class Strategy : std::uint8_t { random, priority };
    std::uint64_t seed{1};
    Strategy strategy{Strategy::random};
    double priority_change{0.05}; // priority strategy: chance the running task drops to lowest priority
    double abort_probability{0};  // chance per step of an injected abort (first two attempts only)
};

class SeededRunner final : public JobRunner {
public:
    explicit SeededRunner(ExplorationOptions options);
    void run(std::span<Task* const> tasks, std::uint32_t attempt) override;
    [[nodiscard]] bool concurrent() const noexcept override { return false; }
    [[nodiscard]] std::uint64_t steps() const noexcept { return steps_; }
    [[nodiscard]] std::uint64_t injected() const noexcept { return injected_; }

private:
    std::uint64_t next();
    double unit();
    ExplorationOptions options_;
    std::uint64_t state_;
    std::uint64_t steps_{};
    std::uint64_t injected_{};
};

} // namespace circuit::runtime
