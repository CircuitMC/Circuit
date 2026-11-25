// SPDX-License-Identifier: AGPL-3.0-only
#include <atomic>
#include <exception>
#include <memory>
#include <mutex>
#include <circuit/runtime/job_runner.hpp>
#include <algorithm>
#include <functional>
#include <numeric>
#include <vector>

namespace circuit::runtime {

PoolRunner::PoolRunner(unsigned workers) : workers_(workers) {
    if (workers > 1) pool_.emplace(workers - 1);
}

void PoolRunner::run(std::span<Task* const> tasks, std::uint32_t) {
    const auto drain = [](Task* task) {
        while (task->step()) {
        }
    };
    if (tasks.size() <= 1 || workers_ <= 1) {
        for (auto* task : tasks) drain(task);
        return;
    }
    // Helpers and the calling thread take tasks from a shared index: one pool
    // job per helper instead of one per task, and no thread sits idle while
    // the caller waits.
    struct Shared {
        std::atomic<std::size_t> next{0};
        std::mutex mutex;
        std::exception_ptr error; // the first exception of the batch, on any thread
    };
    auto shared = std::make_shared<Shared>();
    const auto take = [tasks, shared, drain] {
        std::exception_ptr caught;
        try {
            for (std::size_t i; (i = shared->next.fetch_add(1, std::memory_order_relaxed)) < tasks.size();) drain(tasks[i]);
        } catch (...) {
            caught = std::current_exception();
        }
        // Outside the handler, so that a helper holds no reference to the
        // exception once it has handed it over.
        if (caught) {
            std::lock_guard lock(shared->mutex);
            if (!shared->error) shared->error = std::move(caught);
        }
    };
    const auto helpers = std::min<std::size_t>(workers_, tasks.size()) - 1;
    auto done = pool_->submit(std::vector<std::function<void()>>(helpers, take));
    take();
    // The helpers work on the caller's tasks, so nothing leaves this function
    // before they have finished, whichever thread a task threw on. `take`
    // does not throw; `done` holds an exception only when a helper job could
    // not be queued.
    done.wait();
    // The caller takes the exception over: a helper may let go of `shared`
    // after this function has returned, and must not be the one that frees an
    // exception the caller has looked at.
    std::exception_ptr error;
    {
        std::lock_guard lock(shared->mutex);
        error = std::move(shared->error);
    }
    if (error) std::rethrow_exception(error);
    done.get();
}

SeededRunner::SeededRunner(ExplorationOptions options) : options_(options), state_(options.seed) {}

std::uint64_t SeededRunner::next() {
    state_ += 0x9E3779B97F4A7C15ULL;
    auto z = state_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

double SeededRunner::unit() { return static_cast<double>(next() >> 11) * 0x1.0p-53; }

void SeededRunner::run(std::span<Task* const> tasks, std::uint32_t attempt) {
    std::vector<Task*> live(tasks.begin(), tasks.end());
    // Priority strategy: a random initial order; the running task now and then
    // drops to the lowest priority, which yields a different interleaving.
    if (options_.strategy == ExplorationOptions::Strategy::priority)
        for (std::size_t i = live.size(); i > 1; --i) std::swap(live[i - 1], live[next() % i]);
    while (!live.empty()) {
        std::size_t index = 0;
        if (options_.strategy == ExplorationOptions::Strategy::random) index = next() % live.size();
        auto* task = live[index];
        ++steps_;
        if (attempt < 2 && options_.abort_probability > 0 && unit() < options_.abort_probability) {
            ++injected_;
            task->inject_abort();
            live.erase(live.begin() + static_cast<std::ptrdiff_t>(index));
            continue;
        }
        if (!task->step()) {
            live.erase(live.begin() + static_cast<std::ptrdiff_t>(index));
            continue;
        }
        if (options_.strategy == ExplorationOptions::Strategy::priority && live.size() > 1
            && unit() < options_.priority_change)
            std::rotate(live.begin() + static_cast<std::ptrdiff_t>(index),
                        live.begin() + static_cast<std::ptrdiff_t>(index) + 1, live.end());
    }
}

} // namespace circuit::runtime
