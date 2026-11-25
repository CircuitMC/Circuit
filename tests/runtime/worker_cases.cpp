// SPDX-License-Identifier: AGPL-3.0-only
// The worker threads of the runtime: what PoolRunner and StageExecutor
// guarantee when a job throws, and which threads they start.
#include "../support/check.hpp"
#include "threads.hpp"
#include <circuit/contracts/version.hpp>
#include <circuit/runtime/engine.hpp>
#include <circuit/runtime/executor.hpp>
#include <circuit/runtime/job_runner.hpp>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using circuit::runtime::PoolRunner;
using circuit::runtime::StageExecutor;
using circuit::runtime::Task;
using circuit::test::eventually;
using circuit::test::require;
using circuit::test::settled_thread_count;

namespace {

// A task of one step that calls a function.
class CallTask final : public Task {
public:
    explicit CallTask(std::function<void()> body) : body_(std::move(body)) {}
    bool step() override {
        body_();
        return false;
    }
    void inject_abort() override {}

private:
    std::function<void()> body_;
};

struct Tasks {
    std::vector<std::unique_ptr<CallTask>> owned;
    std::vector<Task*> pointers;
    Tasks(std::size_t count, const std::function<void(std::size_t)>& body) {
        for (std::size_t i = 0; i < count; ++i) {
            owned.push_back(std::make_unique<CallTask>([body, i] { body(i); }));
            pointers.push_back(owned.back().get());
        }
    }
};

// Runs the tasks and returns the message of the exception run() threw, or an
// empty string when it returned normally.
std::string thrown_by(PoolRunner& runner, const Tasks& tasks) {
    try {
        runner.run(tasks.pointers, 0);
    } catch (const std::runtime_error& e) {
        return e.what();
    }
    return {};
}

// A batch of tasks of several steps each; true when every step of every task
// ran exactly once.
bool runs_a_plain_batch(PoolRunner& runner, std::size_t count) {
    class Counting final : public Task {
    public:
        bool step() override { return ++steps < 3; }
        void inject_abort() override {}
        int steps{0};
    };
    std::vector<Counting> tasks(count);
    std::vector<Task*> pointers;
    for (auto& task : tasks) pointers.push_back(&task);
    runner.run(pointers, 0);
    for (const auto& task : tasks)
        if (task.steps != 3) return false;
    return true;
}

} // namespace

CIRCUIT_TEST("pool runner waits for the helpers when the coordinator's task throws") {
    PoolRunner runner(4);
    for (int round = 0; round < 20; ++round) {
        const auto coordinator = std::this_thread::get_id();
        std::atomic<int> started{0}, finished{0};
        std::atomic<bool> coordinator_threw{false};
        const Tasks tasks(8, [&](std::size_t) {
            if (std::this_thread::get_id() == coordinator) {
                // Throw only while a helper is inside a task of this batch.
                (void)eventually([&] { return started.load() > 0; });
                coordinator_threw.store(true);
                throw std::runtime_error("coordinator");
            }
            ++started;
            (void)eventually([&] { return coordinator_threw.load(); });
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            ++finished;
        });
        const auto message = thrown_by(runner, tasks);
        // Read before anything else: was a helper still in a task on return?
        const int started_on_return = started.load(), finished_on_return = finished.load();
        // A runner that returned early still uses the tasks; let it finish
        // before they are destroyed, so that the failure is a report.
        (void)eventually([&] { return finished.load() == 7; });
        require(message == "coordinator", "run() did not rethrow the coordinator's exception");
        require(started_on_return > 0, "no helper was inside a task when the coordinator threw");
        require(started_on_return == finished_on_return, "run() returned while helper tasks were executing");
        require(finished_on_return == 7, "the helpers did not finish the tasks the coordinator left");
        require(runs_a_plain_batch(runner, 16), "the runner cannot be used after the coordinator's exception");
    }
}

CIRCUIT_TEST("pool runner rethrows a helper's exception after every other task has run") {
    PoolRunner runner(4);
    for (int round = 0; round < 20; ++round) {
        const auto coordinator = std::this_thread::get_id();
        std::atomic<int> finished{0};
        std::atomic<bool> helper_threw{false};
        const Tasks tasks(8, [&](std::size_t) {
            if (std::this_thread::get_id() != coordinator) {
                if (!helper_threw.exchange(true)) throw std::runtime_error("helper");
            } else {
                // The coordinator leaves the first throw to a helper.
                (void)eventually([&] { return helper_threw.load(); });
            }
            ++finished;
        });
        const auto message = thrown_by(runner, tasks);
        const int finished_on_return = finished.load();
        require(message == "helper", "run() did not rethrow the helper's exception");
        require(finished_on_return == 7, "run() returned before the other tasks had run");
        require(runs_a_plain_batch(runner, 16), "the runner cannot be used after a helper's exception");
    }
}

CIRCUIT_TEST("pool runner rethrows one exception when every thread's task throws") {
    PoolRunner runner(4);
    for (int round = 0; round < 20; ++round) {
        std::atomic<int> entered{0}, left{0};
        const Tasks tasks(8, [&](std::size_t i) {
            ++entered;
            // Keep the threads in their tasks together, so that several throw.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            ++left;
            throw std::runtime_error("task " + std::to_string(i));
        });
        const auto message = thrown_by(runner, tasks);
        const int entered_on_return = entered.load(), left_on_return = left.load();
        require(message.starts_with("task "), "run() did not rethrow a task's exception");
        require(entered_on_return == left_on_return, "run() returned while tasks were executing");
        // A thread takes no further task after one of its tasks has thrown.
        require(entered_on_return >= 1 && entered_on_return <= 4, "a thread took a task after its task had thrown");
        require(runs_a_plain_batch(runner, 16), "the runner cannot be used after every thread's exception");
    }
}

CIRCUIT_TEST("pool runner with one worker runs tasks in order on the calling thread and stops at an exception") {
    PoolRunner runner(1);
    require(!runner.concurrent(), "one worker is not concurrent");
    const auto caller = std::this_thread::get_id();
    std::vector<std::size_t> order;
    bool elsewhere = false;
    const Tasks tasks(6, [&](std::size_t i) {
        elsewhere |= std::this_thread::get_id() != caller;
        order.push_back(i);
        if (i == 3) throw std::runtime_error("fourth");
    });
    require(thrown_by(runner, tasks) == "fourth", "the exception did not leave run()");
    require(!elsewhere, "a task ran on another thread");
    require(order == std::vector<std::size_t>{0, 1, 2, 3}, "tasks ran out of order or after the exception");
    require(runs_a_plain_batch(runner, 16), "the runner cannot be used after an exception");
}

CIRCUIT_TEST("pool runner completes many batches at 2/4/8 workers") {
    for (unsigned workers : {2u, 4u, 8u}) {
        PoolRunner runner(workers);
        require(runner.concurrent(), "several workers are concurrent");
        for (std::size_t round = 0; round < 500; ++round)
            require(runs_a_plain_batch(runner, 1 + round % 24), "a task did not run to completion exactly once");
    }
}

CIRCUIT_TEST("pool runner and engine start no thread at one worker") {
    const auto before = settled_thread_count();
    require(before.has_value(), "the threads of the process cannot be counted (/proc/self/task)");
    {
        PoolRunner runner(1);
        require(settled_thread_count() == before, "a pool runner of one worker started a thread");
        require(runs_a_plain_batch(runner, 16), "the tasks did not run");
        require(settled_thread_count() == before, "a pool runner of one worker started a thread in run()");
    }
    {
        // The count does see worker threads: workers - 1 helpers, gone with the runner.
        PoolRunner runner(4);
        require(settled_thread_count() == *before + 3, "a pool runner of four workers does not have three helper threads");
        require(runs_a_plain_batch(runner, 16), "the tasks did not run");
        require(settled_thread_count() == *before + 3, "run() changed the number of threads");
    }
    require(eventually([&] { return settled_thread_count() == before; }), "the helper threads outlived the runner");
    {
        const auto bundle = circuit::contracts::BundleRegistry::select_for_simulation("java-1.21.8");
        require(bundle.has_value(), "no bundle for java-1.21.8");
        circuit::runtime::EngineOptions options;
        options.workers = 1;
        circuit::runtime::Engine engine(**bundle, 1, options);
        require(settled_thread_count() == before, "an engine of one worker started a thread");
        for (int tick = 0; tick < 3; ++tick) engine.advance({});
        require(settled_thread_count() == before, "an engine of one worker started a thread while ticking");
    }
}

CIRCUIT_TEST("pool runner and stage executor reject worker counts outside their range") {
    const auto invalid = [](const std::function<void()>& make) {
        try {
            make();
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    require(invalid([] { StageExecutor pool(0); }), "zero threads accepted");
    require(invalid([] { StageExecutor pool(65); }), "65 threads accepted");
    require(!invalid([] { StageExecutor pool(64); }), "64 threads rejected");
    require(invalid([] { PoolRunner runner(66); }), "66 workers accepted");
    require(!invalid([] { PoolRunner runner(65); }), "65 workers (64 helpers) rejected");
    // No worker at all behaves like one: everything on the calling thread.
    PoolRunner none(0);
    require(!none.concurrent() && runs_a_plain_batch(none, 4), "a runner of zero workers did not run the tasks");
}

CIRCUIT_TEST("stage executor keeps the first exception of a batch and runs the jobs after it") {
    // One thread takes the jobs in submission order, so "first" is exact here.
    StageExecutor pool(1);
    std::vector<int> order;
    std::string message;
    try {
        pool.submit({[&] { order.push_back(0); },
                     [&] { order.push_back(1); throw std::runtime_error("first"); },
                     [&] { order.push_back(2); throw std::runtime_error("second"); },
                     [&] { order.push_back(3); }})
            .get();
    } catch (const std::runtime_error& e) {
        message = e.what();
    }
    require(message == "first", "the batch does not hold its first exception");
    require(order == std::vector<int>{0, 1, 2, 3}, "a job after the failing one did not run, or the order changed");
    pool.submit({[&] { order.push_back(4); }}).get();
    require(order.back() == 4, "the executor cannot be used after a failed batch");
}

CIRCUIT_TEST("stage executor runs every submitted job before it is destroyed") {
    std::atomic<int> count{0};
    {
        StageExecutor pool(3);
        std::vector<std::function<void()>> jobs;
        for (int i = 0; i < 200; ++i)
            jobs.emplace_back([&] {
                std::this_thread::yield();
                ++count;
            });
        // The future is dropped: the destructor alone waits for the jobs.
        (void)pool.submit(std::move(jobs));
    }
    require(count == 200, "the destructor left submitted jobs behind");
}

CIRCUIT_TEST("stage executor starts the threads it is given and joins them") {
    const auto before = settled_thread_count();
    require(before.has_value(), "the threads of the process cannot be counted (/proc/self/task)");
    {
        StageExecutor pool(5);
        require(settled_thread_count() == *before + 5, "an executor of five threads does not have five threads");
        pool.submit({}).get();
    }
    require(eventually([&] { return settled_thread_count() == before; }), "the threads outlived the executor");
}
