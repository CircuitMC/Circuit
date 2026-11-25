// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <functional>
#include <future>
#include <memory>
#include <vector>

namespace circuit::runtime {
// The coordinator awaits completion; workers never wait for other pool jobs.
// Jobs must own disjoint mutable state or operate on immutable views.
class StageExecutor {
public:
    // Starts `workers` threads (1 to 64); throws std::invalid_argument otherwise.
    explicit StageExecutor(unsigned workers);
    // Runs every job already submitted, then joins the threads.
    ~StageExecutor();
    StageExecutor(const StageExecutor&) = delete;
    StageExecutor& operator=(const StageExecutor&) = delete;
    // Queues the jobs and returns at once. The future becomes ready when every
    // job of the batch has returned or thrown; it then holds the first
    // exception, if any. A job that throws does not stop the others.
    [[nodiscard]] std::future<void> submit(std::vector<std::function<void()>> jobs);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
