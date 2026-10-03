// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <asio/thread_pool.hpp>
#include <functional>
#include <future>
#include <vector>

namespace circuit::runtime {
// The coordinator awaits completion; workers never wait for other pool jobs.
// Jobs must own disjoint mutable state or operate on immutable views.
class StageExecutor {
public:
    explicit StageExecutor(unsigned workers);
    ~StageExecutor();
    StageExecutor(const StageExecutor&) = delete;
    StageExecutor& operator=(const StageExecutor&) = delete;
    [[nodiscard]] std::future<void> submit(std::vector<std::function<void()>> jobs);
private:
    asio::thread_pool pool_;
};
}
