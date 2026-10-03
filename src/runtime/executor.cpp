// SPDX-License-Identifier: AGPL-3.0-only
#include <circuit/runtime/executor.hpp>
#include <asio/post.hpp>
#include <atomic>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace circuit::runtime {
namespace {
unsigned checked_workers(unsigned n) {
    if (n == 0 || n > 64) throw std::invalid_argument("workers must be in [1,64]");
    return n;
}
struct Batch {
    // A submission sentinel keeps the promise alive while more jobs are posted.
    std::atomic_size_t remaining{1};
    std::mutex mutex;
    std::exception_ptr error;
    std::promise<void> done;
    void complete(std::exception_ptr failure = {}) {
        if (failure) {
            std::lock_guard lock(mutex);
            if (!error) error = failure;
        }
        if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            if (error) done.set_exception(error);
            else done.set_value();
        }
    }
};
}
StageExecutor::StageExecutor(unsigned workers) : pool_(checked_workers(workers)) {}
StageExecutor::~StageExecutor() { pool_.join(); }
std::future<void> StageExecutor::submit(std::vector<std::function<void()>> jobs) {
    auto batch = std::make_shared<Batch>();
    auto future = batch->done.get_future();
    for (auto& job : jobs) {
        batch->remaining.fetch_add(1, std::memory_order_relaxed);
        try {
            asio::post(pool_, [batch, fn = std::move(job)] {
                try { fn(); batch->complete(); }
                catch (...) { batch->complete(std::current_exception()); }
            });
        } catch (...) {
            batch->complete(std::current_exception());
            break;
        }
    }
    batch->complete();
    return future;
}
}
