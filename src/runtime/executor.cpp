// SPDX-License-Identifier: AGPL-3.0-only
#include <circuit/runtime/executor.hpp>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

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

// A fixed set of threads that take jobs from one queue in submission order.
struct StageExecutor::Impl {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::function<void()>> queue;
    bool stopping{false};
    std::vector<std::thread> threads;

    explicit Impl(unsigned workers) {
        threads.reserve(workers);
        try {
            for (unsigned i = 0; i < workers; ++i) threads.emplace_back([this] { work(); });
        } catch (...) {
            stop();
            throw;
        }
    }
    ~Impl() { stop(); }

    void post(std::function<void()> job) {
        {
            std::lock_guard lock(mutex);
            queue.push_back(std::move(job));
        }
        wake.notify_one();
    }
    void work() {
        std::unique_lock lock(mutex);
        for (;;) {
            wake.wait(lock, [this] { return stopping || !queue.empty(); });
            // Stopping ends a thread only once the queue is empty.
            if (queue.empty()) return;
            auto job = std::move(queue.front());
            queue.pop_front();
            lock.unlock();
            job(); // never throws: submit() wraps every job
            job = nullptr;
            lock.lock();
        }
    }
    void stop() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        wake.notify_all();
        for (auto& thread : threads) thread.join();
        threads.clear();
    }
};

StageExecutor::StageExecutor(unsigned workers) : impl_(std::make_unique<Impl>(checked_workers(workers))) {}
StageExecutor::~StageExecutor() = default;
std::future<void> StageExecutor::submit(std::vector<std::function<void()>> jobs) {
    auto batch = std::make_shared<Batch>();
    auto future = batch->done.get_future();
    for (auto& job : jobs) {
        batch->remaining.fetch_add(1, std::memory_order_relaxed);
        try {
            impl_->post([batch, fn = std::move(job)] {
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
